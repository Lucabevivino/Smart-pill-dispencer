#!/usr/bin/env python3
"""Summarise a photodiode bench log: open-vs-closed ADC separation, plus the
ambient-vs-box delta (raw4 - raw0) used to study an adaptive threshold.

Reads one or more log files (enumerated .txt under test_logs/photodiode/). For
the OPEN and CLOSED states separately it reports, for the box sensor (raw0) and
for the delta (raw4 - raw0, ambient minus box), the sample count, min, max, mean
and standard deviation. From the box-sensor separation it computes the margin
(worst-open minus worst-closed) and a recommended threshold at the midpoint of
the gap — the value to put in CLIP_SENSOR_LIGHT_THRESHOLD.

The delta is the signal for an adaptive threshold: when the box is open the
interior photodiode sees roughly the same light as the ambient photodiode, so
delta ~ 0 (a small constant offset); when the box is closed the interior diode
goes dark while the ambient diode keeps tracking room light, so |delta| grows.
A decision based on delta (or on raw0 relative to raw4) is robust to changes in
room lighting in a way a fixed raw0 threshold is not.

Usage:
    python3 scripts/photodiode_analyze.py [LOG ...]

    With no arguments it analyses every .txt under test_logs/photodiode/.
"""
import glob
import math
import os
import sys

LOG_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "test_logs", "photodiode"))


def parse_lines(lines):
    """Yield (raw0, raw4, delta, state) tuples for each data row.

    Handles both the original single-diode format
        millis,raw,state,min,max,mean,cycles,threshold
    (raw4/delta are None) and the two-diode format
        millis,raw0,raw4,delta,state,min,max,mean,cycles,threshold.
    """
    for line in lines:
        line = line.strip()
        if not line:
            continue
        # A logger line is "<iso timestamp> <csv>"; a raw monitor line is just
        # "<csv>". Take the payload after the first whitespace, if any.
        parts = line.split(None, 1)
        payload = parts[1] if len(parts) == 2 else parts[0]
        if not payload or payload[0] == "#" or not payload[0].isdigit():
            continue
        fields = payload.split(",")
        try:
            if len(fields) >= 10:
                raw0 = int(fields[1])
                raw4 = int(fields[2])
                delta = int(fields[3])
                state = int(fields[4])
            else:
                raw0 = int(fields[1])
                raw4 = None
                delta = None
                state = int(fields[2])
        except (ValueError, IndexError):
            continue
        yield raw0, raw4, delta, state


def stats(values):
    n = len(values)
    if n == 0:
        return None
    mean = sum(values) / n
    var = sum((v - mean) ** 2 for v in values) / n
    return {"n": n, "min": min(values), "max": max(values),
            "mean": mean, "std": math.sqrt(var)}


def main():
    paths = sys.argv[1:]
    if not paths:
        paths = sorted(glob.glob(os.path.join(LOG_DIR, "*.txt")))
    if not paths:
        print(f"no logs found (looked in {LOG_DIR}); pass a path to a log file", file=sys.stderr)
        return 1

    open_raw, closed_raw = [], []
    open_delta, closed_delta = [], []
    has_delta = False
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw0, raw4, delta, state in parse_lines(f):
                (open_raw if state else closed_raw).append(raw0)
                if delta is not None:
                    has_delta = True
                    (open_delta if state else closed_delta).append(delta)

    o = stats(open_raw)
    c = stats(closed_raw)

    print(f"logs analysed: {len(paths)}")
    print()
    print("box sensor (raw0):")
    if o:
        print(f"  OPEN   : n={o['n']:>6}  min={o['min']:>4}  max={o['max']:>4}  "
              f"mean={o['mean']:7.1f}  std={o['std']:6.1f}")
    else:
        print("  OPEN   : (no samples)")
    if c:
        print(f"  CLOSED : n={c['n']:>6}  min={c['min']:>4}  max={c['max']:>4}  "
              f"mean={c['mean']:7.1f}  std={c['std']:6.1f}")
    else:
        print("  CLOSED : (no samples)")

    if has_delta:
        od = stats(open_delta)
        cd = stats(closed_delta)
        print()
        print("delta (raw4 - raw0, ambient minus box):")
        if od:
            print(f"  OPEN   : n={od['n']:>6}  min={od['min']:>5}  max={od['max']:>5}  "
                  f"mean={od['mean']:7.1f}  std={od['std']:6.1f}")
        else:
            print("  OPEN   : (no samples)")
        if cd:
            print(f"  CLOSED : n={cd['n']:>6}  min={cd['min']:>5}  max={cd['max']:>5}  "
                  f"mean={cd['mean']:7.1f}  std={cd['std']:6.1f}")
        else:
            print("  CLOSED : (no samples)")

    if o and c:
        margin = o["min"] - c["max"]
        print()
        print(f"separation margin (min_open - max_closed) = {margin}")
        if margin > 0:
            mid = (o["min"] + c["max"]) // 2
            print(f"recommended threshold (mid-gap)          = {mid}")
            print("set CLIP_SENSOR_LIGHT_THRESHOLD to this value in")
            print("lib/clip_adherence/clip_adherence.cpp")
            print()
            if margin < 500:
                print("WARNING: margin < 500 counts — the phototransistor may not be")
                print("reliable under this lighting; consider the IR-reflective backend.")
        else:
            print("FAIL: open and closed ranges overlap — the sensor cannot distinguish")
            print("the two states at the current threshold. Check wiring / light leak.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
