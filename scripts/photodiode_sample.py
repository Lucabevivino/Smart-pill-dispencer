#!/usr/bin/env python3
"""Manually sample the box photodiode (D0) on demand, tagging each reading with
a user-entered angle.

The bench firmware streams continuously, but this script records only when you
ask it to: a background thread keeps the latest D0/D4/delta reading, and each
time you type an angle it writes one row

    <timestamp>,<angle>,<raw0>,<raw4>,<delta>,<state>,<threshold>,<millis>

to test_logs/photodiode/angles_<date>_<NNN>.txt. Use it to map the photodiode
response against the box-opening angle you measure manually.

The D4/delta columns are only populated when the two-diode firmware is running
(millis,raw0,raw4,delta,state,...). With the old single-diode firmware it still
records raw0 and leaves D4/delta blank.

Usage:
    .venv/bin/python scripts/photodiode_sample.py [--port PORT] [--baud BAUD]

    At the prompt, type an angle in degrees (e.g. "45") and press Enter to
    record the current D0 reading; press Enter with no angle to record without
    one; "q" quits. While you wait, a live readout of raw0/raw4/delta/state
    prints once per second whenever any of those values change, so you can
    watch the sensor respond as you adjust the wiring.

Requires pyserial (installed in the project .venv).
"""
import argparse
import datetime as dt
import os
import select
import sys
import threading
import time

LOG_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "test_logs", "photodiode"))


def next_path():
    os.makedirs(LOG_DIR, exist_ok=True)
    date = dt.date.today().isoformat()
    n = 1
    while True:
        path = os.path.join(LOG_DIR, f"angles_{date}_{n:03d}.txt")
        if not os.path.exists(path):
            return path
        n += 1


def find_port():
    from serial.tools import list_ports
    ports = list(list_ports.comports())

    def is_real(p):
        blob = (p.device + " " + (p.description or "")).lower()
        for junk in ("debug-console", "bluetooth", "incoming-port", "nrf"):
            if junk in blob:
                return False
        return True

    real = [p for p in ports if is_real(p)]
    if not real:
        return None, ports

    def prefer_usb(p):
        name = p.device.lower()
        for good in ("usbmodem", "usbserial", "wchusbserial", "slab"):
            if good in name:
                return 0
        return 1

    real.sort(key=prefer_usb)
    return real[0].device, real


class Latest:
    """Thread-safe holder for the most recent parsed sample."""
    def __init__(self):
        self._lock = threading.Lock()
        self.raw0 = self.raw4 = self.delta = self.state = self.threshold = self.millis = None

    def update(self, raw0, raw4, delta, state, threshold, millis):
        with self._lock:
            self.raw0, self.raw4, self.delta, self.state, self.threshold, self.millis = (
                raw0, raw4, delta, state, threshold, millis)

    def get(self):
        with self._lock:
            return (self.raw0, self.raw4, self.delta, self.state,
                    self.threshold, self.millis)


def reader_loop(ser, latest, stop):
    """Drain the serial stream and keep only the latest parsed sample."""
    while not stop.is_set():
        try:
            raw = ser.readline()
        except Exception:
            break
        if not raw:
            continue
        text = raw.decode("utf-8", "replace").strip()
        if not text or text[0] == "#":
            continue
        fields = text.split(",")
        try:
            millis = int(fields[0])
            if len(fields) >= 10:
                raw0 = int(fields[1])
                raw4 = int(fields[2])
                delta = int(fields[3])
                state = int(fields[4])
                threshold = int(fields[9])
            else:
                raw0 = int(fields[1])
                raw4 = None
                delta = None
                state = int(fields[2])
                threshold = int(fields[7])
        except (ValueError, IndexError):
            continue
        latest.update(raw0, raw4, delta, state, threshold, millis)


def record_sample(line, latest, f):
    """Validate one typed line and write a sample row. Returns False to quit."""
    line = line.strip()
    if line.lower() in ("q", "quit", "exit"):
        return False
    if line:
        try:
            float(line)
        except ValueError:
            print("  (not a number — enter degrees, blank, or q)")
            return True

    raw0, raw4, delta, state, threshold, millis = latest.get()
    ts = dt.datetime.now().isoformat(timespec="milliseconds")
    a = line if line else ""
    r4 = "" if raw4 is None else str(raw4)
    d = "" if delta is None else str(delta)
    st = "" if state is None else str(state)
    th = "" if threshold is None else str(threshold)
    row = f"{ts},{a},{raw0},{r4},{d},{st},{th},{millis}"
    print(f"  -> {row}")
    f.write(row + "\n")
    f.flush()
    return True


def main():
    try:
        import serial
    except ImportError:
        print("pyserial is required:  .venv/bin/python -m pip install pyserial", file=sys.stderr)
        return 1

    ap = argparse.ArgumentParser(description="On-demand photodiode sampling with a manual angle.")
    ap.add_argument("-p", "--port", default=None, help="serial port (auto-detected if omitted)")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="baud rate (default 115200)")
    args = ap.parse_args()

    port = args.port
    if port is None:
        found, candidates = find_port()
        if found is None:
            print("no usable serial port found; pass --port PORT", file=sys.stderr)
            for p in candidates:
                print(f"  {p.device}  ({p.description or '?'})", file=sys.stderr)
            return 1
        port = found
        print(f"auto-detected port: {port}")

    path = next_path()
    print(f"recording to {path}")

    stop = threading.Event()
    latest = Latest()

    with serial.Serial(port, args.baud, timeout=1) as ser, open(path, "a", encoding="utf-8") as f:
        f.write(f"# photodiode manual samples | {dt.datetime.now().isoformat(timespec='seconds')} "
                f"| port={port} baud={args.baud}\n")
        f.write("# timestamp,angle,raw0,raw4,delta,state,threshold,millis\n")
        f.flush()

        t = threading.Thread(target=reader_loop, args=(ser, latest, stop), daemon=True)
        t.start()

        print("waiting for data...")
        t0 = time.time()
        while latest.get()[0] is None:
            if time.time() - t0 > 10:
                print("no data after 10 s — wrong port, or board not flashed?", file=sys.stderr)
                return 1
            time.sleep(0.1)

        print("ready.")
        print("type an angle (deg) + Enter to sample D0  (blank = no angle, q = quit)")
        print("live readout: raw0/raw4/delta/state print whenever they change (1 s check)")

        if sys.stdin.isatty():
            last_shown = None
            while True:
                try:
                    r, _, _ = select.select([sys.stdin], [], [], 1.0)
                except (KeyboardInterrupt, ValueError, OSError):
                    break
                if r:
                    line = sys.stdin.readline()
                    if line == "":
                        break
                    if not record_sample(line, latest, f):
                        break
                    continue

                # 1 s elapsed with no input: show live values, but only on change
                # so a stuck 4095 doesn't spam the terminal.
                raw0, raw4, delta, state, _threshold, _millis = latest.get()
                if raw0 is None:
                    continue
                cur = (raw0, raw4, delta, state)
                if cur != last_shown:
                    r4 = "—" if raw4 is None else str(raw4)
                    d = "—" if delta is None else str(delta)
                    if state == 1:
                        st = "1 (open)"
                    elif state == 0:
                        st = "0 (closed)"
                    else:
                        st = "—"
                    print(f"  [live {dt.datetime.now().strftime('%H:%M:%S')}] "
                          f"raw0={raw0}  raw4={r4}  delta={d}  state={st}")
                    last_shown = cur
        else:
            while True:
                try:
                    line = input("angle> ")
                except (EOFError, KeyboardInterrupt):
                    break
                if not record_sample(line, latest, f):
                    break

    stop.set()
    print(f"\nsaved to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
