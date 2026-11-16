# Test logs

Human-collected logs from the sensor/peripheral bench firmware, one sub-directory
per device-under-test. The logger scripts create these sub-directories on first use.

## Running a bench

One-time setup (creates a project-local venv and installs pyserial):

```sh
python3 -m venv .venv
.venv/bin/python -m pip install pyserial
```

Run the whole photodiode bench (flash the ESP32-C3, then log) with an optional threshold:

```sh
.venv/bin/python scripts/photodiode_run.py --threshold 1200
```

Or flash and log as separate steps:

```sh
pio run -e bench-photodiode -t upload
.venv/bin/python scripts/photodiode_logger.py --threshold 1200
```

Analyse the newest log afterwards:

```sh
.venv/bin/python scripts/photodiode_analyze.py test_logs/photodiode/2026-11-12_002.txt
```

Common flags on `photodiode_run.py` / `photodiode_logger.py`:

| Flag | Meaning |
|------|---------|
| `-t, --threshold N` | open/closed ADC threshold (0..4095) to apply over serial after boot |
| `-p, --port PORT`   | serial port (auto-detected if omitted) |
| `-b, --baud BAUD`   | baud rate (default 115200) |
| `--skip-flash`      | (`run.py` only) skip build/upload and go straight to logging |

The logger writes each session to `test_logs/<name>/<date>_<NNN>.txt` and prints
each line it captures, so you can watch the stream live. Stop it with Ctrl+C.

To record individual readings against a manually measured angle (rather than a
continuous log), use the sampler:

```sh
.venv/bin/python scripts/photodiode_sample.py
```

It connects and waits; each time you type an angle (e.g. `45`) and press Enter
it writes one row `timestamp,angle,raw0,raw4,delta,state,threshold,millis` to
`test_logs/photodiode/angles_<date>_<NNN>.txt`. Blank = no angle, `q` quits.

## Reading the logs

Each log contains two kinds of line:

- **`#` lines** — headers/comments. The first is written by the logger
  (`# photodiode bench log | <timestamp> | port=… baud=… threshold=…`); the rest
  are the firmware's boot banner (`# PHOTODIODE_BENCH_BEGIN`, `# box_pin=0 ambient_pin=4 …`,
  `# format: …`, `# commands: …`). Ignore these when parsing.
- **Data lines** — one per sample, the logger's wall-clock timestamp followed by
  the firmware CSV:

```
2026-11-12T16:25:04.963 689448,4095,3500,-595,1,4095,4095,4095,0,1200
```

The CSV columns are `millis,raw0,raw4,delta,state,min,max,mean,cycles,threshold`:

| Column | Example | Meaning |
|--------|---------|---------|
| `millis`   | `689448`  | ms since the board booted (resets on flash / power-cycle) |
| `raw0`     | `4095`    | box-interior photodiode, **0..4095** (12-bit ADC) |
| `raw4`     | `3500`    | ambient/external photodiode, **0..4095** (12-bit ADC) |
| `delta`    | `-595`    | `raw4 - raw0` (ambient minus box) |
| `state`    | `1`       | `1` = open (`raw0 > threshold`), `0` = closed |
| `min`      | `4095`    | lowest `raw0` over the last 64 samples |
| `max`      | `4095`    | highest `raw0` over the last 64 samples |
| `mean`     | `4095`    | average `raw0` over the last 64 samples |
| `cycles`   | `0`       | number of completed open→closed transitions |
| `threshold`| `1200`    | the open/closed decision level in effect for this sample |

How to interpret a session:

- The timestamp prefix is **wall-clock** (added by the logger); `millis` is the
  board's own clock. `min`/`max`/`mean` are a rolling window, so they lag the
  instantaneous `raw0` by up to 64 samples (~6 s at 100 ms).
- `state` is **derived** from `raw0 > threshold` — it's not a separate
  measurement, so it's only as good as the threshold you set.
- For calibration you care about the **gap in `raw0`** between covered (closed)
  and uncovered (open). A bigger, stable gap = a more reliable sensor.
- A `raw0` frozen at **4095** means the ADC input is saturated at ~3.3 V
  (node pinned high). Frozen at **0** means it's at ground or floating low.
  Neither is a usable signal.
- **`delta` is the adaptive-threshold signal.** With the box open, the interior
  diode sees ~the same light as the ambient one, so `delta` sits near a small
  constant offset; with the box closed the interior diode goes dark while the
  ambient one keeps tracking, so `|delta|` grows. Watch `delta` while you vary
  the room light: if it stays near its offset when open and moves away when
  closed, a `delta`-based threshold is the robust route.
- `cycles` counts full open→closed round-trips, handy for confirming the state
  machine actually toggled during a cover/uncover test.

You don't have to eyeball this by hand — `scripts/photodiode_analyze.py` groups
every data line by `state` and reports the open-vs-closed min/max/mean/std for
both `raw0` and `delta`, the separation margin, and a recommended
`CLIP_SENSOR_LIGHT_THRESHOLD`.

## Reflective-IR bench

The reflective-IR sensor (TCRT5000-style) is the **default** box sensor on the clip
(`CLIP_SENSOR_IR_REFLECTIVE`). It has one ADC channel, not two, and its polarity is
**inverted** vs the phototransistor bench: with the box **closed** a surface sits in
front of the sensor and the IR bounces back → **high** reading; with the box **open**
nothing reflects → **low** reading. So the decision is `open = raw < threshold`.

Run it exactly like the photodiode bench:

```sh
.venv/bin/python scripts/ir_run.py --threshold 1500
# or flash + log separately:
pio run -e bench-ir -t upload
.venv/bin/python scripts/ir_logger.py --threshold 1500
# analyse afterwards:
.venv/bin/python scripts/ir_analyze.py test_logs/ir/2026-11-15_001.txt
```

Data lines are single-channel, 8 columns:

```
2026-11-15T10:02:11.481 123456,3100,0,2900,3300,3120,3,1500
```

| Column | Meaning |
|--------|---------|
| `millis`   | ms since boot (resets on flash / power-cycle) |
| `raw`      | reflective-IR reading, **0..4095** (12-bit ADC) |
| `state`    | `1` = open (`raw < threshold`), `0` = closed |
| `min`      | lowest `raw` over the last 64 samples |
| `max`      | highest `raw` over the last 64 samples |
| `mean`     | average `raw` over the last 64 samples |
| `cycles`   | number of completed open→closed transitions |
| `threshold`| the open/closed decision level in effect for this sample |

For calibration, cover/uncover the sensor a few times and then run
`scripts/ir_analyze.py`: it reports the open-vs-closed separation margin
(`min_closed - max_open`) and a recommended threshold at the gap midpoint — that's
the value to put in `CLIP_SENSOR_REFLECT_THRESHOLD` in
`lib/clip_adherence/clip_adherence.cpp`. The sensor is only reliable if the margin is
comfortably positive; `ir_analyze.py` warns when it drops below 500 counts.

## Naming convention

| Piece       | Pattern                            | Example (photodiode / IR)                  |
|-------------|------------------------------------|-------------------------------------------|
| PIO env     | `bench-<name>`                     | `bench-photodiode` / `bench-ir`           |
| firmware    | `src/Bench<Name>/main.cpp`         | `src/BenchPhotodiode/main.cpp` / `src/BenchIR/main.cpp` |
| run         | `scripts/<name>_run.py`            | `scripts/photodiode_run.py` / `scripts/ir_run.py` |
| logger      | `scripts/<name>_logger.py`         | `scripts/photodiode_logger.py` / `scripts/ir_logger.py` |
| analyser    | `scripts/<name>_analyze.py`        | `scripts/photodiode_analyze.py` / `scripts/ir_analyze.py` |
| sampler     | `scripts/<name>_sample.py`         | `scripts/photodiode_sample.py`            |
| log files   | `test_logs/<name>/<date>_<NNN>.txt`| `test_logs/photodiode/2026-11-12_001.txt` / `test_logs/ir/2026-11-15_001.txt` |

The logger names each session `<YYYY-MM-DD>_<NNN>.txt`, where `NNN` is the next
free number for that date. Log files are plain text and are **not** ignored by
git — add `test_logs/**/*.txt` to `.gitignore` if you would rather not commit test data.

To add a bench for a new sensor, follow the pattern: create
`src/Bench<Name>/main.cpp`, add `[env:bench-<name>]` to `platformio.ini` with a
matching `build_src_filter`, and add `<name>_run.py` / `<name>_logger.py` / `<name>_analyze.py`.
