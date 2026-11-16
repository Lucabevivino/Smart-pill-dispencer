#!/usr/bin/env python3
"""Summarise a reflective-IR bench log: open-vs-closed ADC separation.

Reads one or more log files (enumerated .txt under test_logs/ir/). The sensor is
inverted vs the phototransistor bench: CLOSED = surface reflects IR back = HIGH
raw; OPEN = nothing reflects = LOW raw. For the OPEN and CLOSED states separately
it reports the sample count, min, max, mean and standard deviation, then computes
the separation margin (min_closed - max_open) and a recommended threshold at the
midpoint of the gap — the value to put in CLIP_SENSOR_REFLECT_THRESHOLD.

Usage:
    python3 scripts/ir_analyze.py [LOG ...]

    With no arguments it analyses every .txt under test_logs/ir/.
"""
import glob
import math
import os
import sys

LOG_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "test_logs", "ir"))


def parse_lines(lines):
    """Yield (raw, state) tuples for each data row.

    Format: millis,raw,state,min,max,mean,cycles,threshold  (state 1 = open).
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
            raw = int(fields[1])
            state = int(fields[2])
        except (ValueError, IndexError):
            continue
        yield raw, state


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
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for raw, state in parse_lines(f):
                (open_raw if state else closed_raw).append(raw)

    o = stats(open_raw)
    c = stats(closed_raw)

    print(f"logs analysed: {len(paths)}")
    print()
    print("reflective IR sensor (raw):")
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

    if o and c:
        margin = c["min"] - o["max"]
        print()
        print(f"separation margin (min_closed - max_open) = {margin}")
        if margin > 0:
            mid = (o["max"] + c["min"]) // 2
            print(f"recommended threshold (mid-gap)           = {mid}")
            print("set CLIP_SENSOR_REFLECT_THRESHOLD to this value in")
            print("lib/clip_adherence/clip_adherence.cpp")
            print()
            if margin < 500:
                print("WARNING: margin < 500 counts — the IR sensor may not be")
                print("reliable; check the reflection distance / surface and wiring.")
        else:
            print("FAIL: open and closed ranges overlap — the sensor cannot distinguish")
            print("the two states at the current threshold. Check wiring / reflection.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
