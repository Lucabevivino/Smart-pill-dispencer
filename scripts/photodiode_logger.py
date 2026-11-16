#!/usr/bin/env python3
"""Capture the `bench-photodiode` serial stream into an enumerated, dated log.

Each run appends a new text log under test_logs/photodiode/ named
`<YYYY-MM-DD>_<NNN>.txt`, where NNN is the next free number for that date.
Every serial line is timestamped with local wall-clock time.

Options:
    -t, --threshold N   set the open/closed threshold over serial after boot
    -p, --port PORT     serial port (auto-detected if omitted)
    -b, --baud BAUD     baud rate (default 115200)

Example:
    .venv/bin/python scripts/photodiode_logger.py --threshold 1200
    .venv/bin/python scripts/photodiode_logger.py --port /dev/cu.usbmodem1101

Requires pyserial (installed in the project .venv).
"""
import argparse
import datetime as dt
import os
import sys
import time

LOG_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "test_logs", "photodiode"))
ADC_MAX = 4095   # 12-bit ADC


def next_log_path():
    os.makedirs(LOG_DIR, exist_ok=True)
    date = dt.date.today().isoformat()   # YYYY-MM-DD
    n = 1
    while True:
        path = os.path.join(LOG_DIR, f"{date}_{n:03d}.txt")
        if not os.path.exists(path):
            return path
        n += 1


def find_port():
    """Return (device, candidates). Picks the most likely real MCU serial port,
    skipping macOS pseudo-ports (debug-console, Bluetooth). Returns (None, ports)
    when nothing looks like a real USB-serial device."""
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


def main():
    try:
        import serial
    except ImportError:
        print("pyserial is required. Install it with:  .venv/bin/python -m pip install pyserial",
              file=sys.stderr)
        return 1

    ap = argparse.ArgumentParser(description="Log the bench-photodiode serial stream.")
    ap.add_argument("-t", "--threshold", type=int, default=None,
                    help=f"open/closed ADC threshold 0..{ADC_MAX} to apply over serial after boot")
    ap.add_argument("-p", "--port", default=None, help="serial port (auto-detected if omitted)")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="baud rate (default 115200)")
    args = ap.parse_args()

    if args.threshold is not None and not (0 <= args.threshold <= ADC_MAX):
        ap.error(f"threshold must be in 0..{ADC_MAX} (got {args.threshold})")

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

    path = next_log_path()
    print(f"logging to {path}")
    print("press Ctrl+C to stop")

    threshold_note = str(args.threshold) if args.threshold is not None else "firmware default"
    try:
        with serial.Serial(port, args.baud, timeout=1) as ser, open(path, "a", encoding="utf-8") as f:
            f.write(f"# photodiode bench log | {dt.datetime.now().isoformat(timespec='seconds')} "
                    f"| port={port} baud={args.baud} threshold={threshold_note}\n")
            f.flush()

            if args.threshold is not None:
                # Give the board time to boot before pushing the threshold command.
                time.sleep(2.0)
                ser.write(f"t={args.threshold}\n".encode())
                ser.flush()
                time.sleep(0.2)

            last_data = time.time()
            warned = False
            while True:
                raw = ser.readline()
                if not raw:
                    if not warned and time.time() - last_data > 8.0:
                        print(f"WARNING: no data received on {port} for 8 s. "
                              "Wrong port, or board not flashed? Use --port to choose.",
                              file=sys.stderr)
                        warned = True
                    continue
                last_data = time.time()
                text = raw.decode("utf-8", "replace").rstrip("\r\n")
                if not text:
                    continue
                stamped = f"{dt.datetime.now().isoformat(timespec='milliseconds')} {text}"
                print(stamped, flush=True)
                f.write(stamped + "\n")
                f.flush()
    except KeyboardInterrupt:
        print(f"\nstopped. log saved to {path}")
        return 0
    except serial.SerialException as e:
        print(f"serial error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
