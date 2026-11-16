#!/usr/bin/env python3
"""One-command photodiode bench: flash the C3, then log the sensor stream.

Usage:
    .venv/bin/python scripts/photodiode_run.py [--threshold N] [--port PORT] [--baud BAUD] [--skip-flash]

    --threshold N   open/closed ADC threshold 0..4095 to apply over serial
    --skip-flash    don't rebuild/upload, just start logging
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ENV = "bench-photodiode"
VENV_PY = os.path.join(ROOT, ".venv", "bin", "python")
LOGGER = os.path.join(ROOT, "scripts", "photodiode_logger.py")
ADC_MAX = 4095   # 12-bit ADC


def main():
    ap = argparse.ArgumentParser(description="Flash and log the photodiode bench.")
    ap.add_argument("-t", "--threshold", type=int, default=None,
                    help=f"open/closed ADC threshold 0..{ADC_MAX} to apply over serial")
    ap.add_argument("-p", "--port", default=None, help="serial port (auto-detected if omitted)")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="baud rate (default 115200)")
    ap.add_argument("--skip-flash", action="store_true",
                    help="skip rebuild/upload and go straight to logging")
    args = ap.parse_args()

    if args.threshold is not None and not (0 <= args.threshold <= ADC_MAX):
        ap.error(f"threshold must be in 0..{ADC_MAX} (got {args.threshold})")

    if not args.skip_flash:
        print(f"==> building & uploading [{ENV}]")
        try:
            rc = subprocess.call(["pio", "run", "-e", ENV, "-t", "upload"], cwd=ROOT)
        except FileNotFoundError:
            print("'pio' not found on PATH", file=sys.stderr)
            return 1
        if rc != 0:
            print("flash failed", file=sys.stderr)
            return rc

    # Replace this process with the logger so Ctrl+C goes straight to it.
    cmd = [VENV_PY, LOGGER]
    if args.threshold is not None:
        cmd += ["--threshold", str(args.threshold)]
    if args.port is not None:
        cmd += ["--port", args.port]
    cmd += ["--baud", str(args.baud)]
    try:
        os.execv(cmd[0], cmd)
    except OSError as e:
        print(f"failed to start logger: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
