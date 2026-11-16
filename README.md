# Smart Pill Dispenser

A two-device ESP32 system that helps people take their medication on time. A central **Hub** keeps track of every pill box and its weekly schedule; a small **Clip** attached to each pill box lights up and sounds an alarm when it is time to take that pill, then reports back whether the dose was taken, missed, or the box was left open. The Hub keeps a persistent, server-ready log of every adherence event so a remote-collection client can be added on top.

Built with [PlatformIO](https://platformio.org/) (Arduino framework). Devices talk over [ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html) — no router or internet connection is required between them.

## Devices

| Device | Board | Role |
| --- | --- | --- |
| **Hub** | ESP32 (WROOM devkit, `esp32dev`) | Main controller. Drives a 16×2 HD44780 LCD + three-button menu, runs WiFi + NTP, discovers clips, assigns each pill a weekly schedule and the current time, and logs adherence + battery. |
| **Clip** | ESP32-C3 (DFRobot Beetle) | Companion device per pill box. Registers with the hub, blinks on demand, sounds the intake alarm from its assigned schedule, and reports adherence + battery via a reflective-IR box sensor. |

## Features

- **Pill management on the Hub** — list, add, refill and remove pills (up to 4 clips, nickname ≤ 5 chars) with counts (`taken` / `total` / `remaining`).
- **Weekly schedules** — each pill has a per-weekday schedule with up to `MAX_INTAKES_PER_DAY` intake times.
- **Automatic clip discovery** — the Hub beacons every 250 ms; clips scan all WiFi channels and register via HELLO.
- **Time sync** — the Hub distributes NTP time to clips (on assignment and hourly re-sync) so they alarm at the right local time even after deep sleep.
- **Reliable delivery** — scheduled/alarm messages carry a `seq` byte and are retransmitted with backoff until ACKed. The Hub persists its outbound queue and adherence log to NVS; the Clip keeps its queue in RTC memory (both survive reboot / deep sleep, and both are checksum-validated on restore).
- **Adherence state machine** — each intake opens a 30-minute window: the clip prompts (two-tone siren), escalates after 5 minutes of no response, and reports `TAKEN` / `MISSED` / `BOX_OPEN` to the Hub. Skipped windows (while asleep) are caught up and reported `MISSED`.
- **Close-box reminder** — a gentle chirp when the box is left open outside a dose window.
- **IR baseline auto-calibration** — the "closed" reading is re-sampled at every pairing, so the open/closed threshold adapts to each box's surface (fixed threshold only as a fallback).
- **Battery reporting** — clips measure their battery through a resistor divider and report millivolts at every sync; the Hub shows a low-battery warning.
- **Deep sleep** — clips sleep between events and wake for the next intake or re-sync.

## Getting started

### 1. Prerequisites

- [PlatformIO](https://docs.platformio.org/en/latest/core/installation/index.html) (CLI or the VS Code / PlatformIO IDE extension).
- An ESP32 devkit for the Hub and an ESP32-C3 (e.g. DFRobot Beetle) for each Clip.

### 2. Configure WiFi credentials

The Hub needs WiFi for NTP. Create `include/wifi_config.h` (this file is gitignored, so it won't be committed):

```cpp
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#define WIFI_SSID          "your-ssid"
#define WIFI_PASSWORD      "your-password"

#endif // WIFI_CONFIG_H
```

### 3. Build & flash

```sh
pio run -e esp32-wroom-devkitm-1 -t upload    # Hub
pio run -e esp32-c3-devkitm-1  -t upload      # Clip
```

The two sensor benches are standalone firmwares (no project libs), used to calibrate
the box-sensor thresholds — see [Sensor calibration](#sensor-calibration):

```sh
pio run -e bench-ir -t upload          # reflective-IR bench (open = low reading)
pio run -e bench-photodiode -t upload  # phototransistor bench (open = high reading)
```

Serial runs at 115200 baud on every environment:

```sh
pio run -e esp32-wroom-devkitm-1 -t monitor
pio run -e esp32-c3-devkitm-1  -t monitor
```

## Wiring

### Hub — 16×2 HD44780 LCD (4-bit mode)

| LCD pin | Signal | Connect to |
| --- | --- | --- |
| 1 | VSS | GND |
| 2 | VDD | +3.3 V (or +5 V — level-shift RS/E/D4–D7 if 5 V) |
| 3 | Vo | Wiper of a 10 kΩ contrast pot (VDD ↔ GND) |
| 4 | RS | `LCD_RS` = GPIO 23 |
| 5 | RW | GND |
| 6 | E | `LCD_EN` = GPIO 22 |
| 11 | D4 | `LCD_D4` = GPIO 21 |
| 12 | D5 | `LCD_D5` = GPIO 19 |
| 13 | D6 | `LCD_D6` = GPIO 18 |
| 14 | D7 | `LCD_D7` = GPIO 4 |
| 15 | A | VDD through a resistor (backlight anode) |
| 16 | K | GND (backlight cathode) |

Navigation buttons (active-low, button → GND with internal pull-up):

| Button | Pin | Function |
| --- | --- | --- |
| UP | GPIO 13 | Previous view / go back (also cancels an in-progress "Add a pill") |
| DOWN | GPIO 14 | Next view |
| SELECT | GPIO 12 | Confirm / enter |

### Clip — LED, buzzer, box sensor, battery

| Signal | Pin (default) | Notes |
| --- | --- | --- |
| LED | `CLIP_ALARM_LED_PIN` = GPIO 5 | Active-high |
| Buzzer | `CLIP_ALARM_BUZZER_PIN` = GPIO 7 | Passive buzzer, LEDC PWM (two-tone siren 950/650 Hz) |
| Box sensor | `CLIP_ADHERENCE_SENSOR_PIN` = GPIO 0 | Must be ADC-capable (GPIO 0–4 on ESP32-C3; avoid GPIO 2, a strapping pin) |
| Battery sense | `CLIP_BATTERY_PIN` = GPIO 3 | ADC input from a resistor divider (see below) |

The battery is a single-cell LiPo (~3.0–4.2 V), above the ESP32-C3's ~3.3 V ADC
input, so it is divided down first. `CLIP_BATTERY_DIVIDER` is the **(battery / ADC)**
ratio of that divider (default `2.0f` for a 1:1 divider); the clip multiplies the raw
reading back up before reporting millivolts. Set `CLIP_BATTERY_PIN` to `0xFF` to
disable battery sensing (reports 0 mV).

## Configuration

All key settings are `#define`s at the top of the two entry points or in the listed
headers:

| Setting | Location | Default | Meaning |
| --- | --- | --- | --- |
| `WIFI_SSID` / `WIFI_PASSWORD` | `include/wifi_config.h` | — | Hub WiFi for NTP |
| `TZ_GMT_OFFSET_SEC` / `TZ_DST_OFFSET_SEC` | `src/Hub/main.cpp` | `3600` / `3600` | Local timezone offset (UTC+1 / DST) |
| `BATTERY_LOW_MV` | `src/Hub/main.cpp` | `3300` | Below this the Hub shows a low-battery warning |
| `CLIP_ALARM_LED_PIN` / `CLIP_ALARM_BUZZER_PIN` | `src/Clip/main.cpp` | 5 / 7 | Clip alert output |
| `CLIP_ADHERENCE_SENSOR_PIN` | `src/Clip/main.cpp` | 0 | Box-sensor input |
| `CLIP_BATTERY_PIN` / `CLIP_BATTERY_DIVIDER` | `src/Clip/main.cpp` | 3 / `2.0f` | Battery ADC pin + divider ratio |
| `DEEP_SLEEP_ON` | `src/Clip/main.cpp` | `1` | Enable deep sleep between events |
| `CLIP_SENSOR_TYPE` | `lib/clip_adherence/clip_adherence.cpp` | `CLIP_SENSOR_IR_REFLECTIVE` | Sensor backend (see below) |
| `CLIP_SENSOR_REFLECT_MARGIN` | `lib/clip_adherence/clip_adherence.cpp` | `500` | Open = reading drops this far below the stored closed baseline |
| `CLIP_SENSOR_REFLECT_THRESHOLD` | `lib/clip_adherence/clip_adherence.cpp` | `1500` | Fallback open threshold until a baseline is sampled |
| `CLIP_SENSOR_LIGHT_THRESHOLD` | `lib/clip_adherence/clip_adherence.cpp` | `1500` | Phototransistor open threshold |
| `INTAKE_WINDOW_MINUTES` | `lib/clip_adherence/clip_adherence.cpp` | `30` | How long the user has to take the pill |
| `BOX_OPEN_GRACE_MS` | `lib/clip_adherence/clip_adherence.cpp` | `15000` | Box-open grace before flagging left-open |
| `CLIP_ESCALATE_AFTER_SEC` | `lib/clip_adherence/clip_adherence.cpp` | `300` | 5 min before escalating the alarm |
| `CLIP_RESYNC_INTERVAL_SEC` | `lib/clip_comms/clip_comms.h` | `3600` | RTC re-anchor interval (1 h) |

### Box sensor types

`CLIP_SENSOR_TYPE` in `lib/clip_adherence/clip_adherence.cpp` selects how "box open"
is detected:

- `CLIP_SENSOR_IR_REFLECTIVE` — reflective IR sensor (IR LED + IR phototransistor,
  e.g. TCRT5000) on the sensor pin. **Open** = nothing reflects back, so the reading
  drops below `baseline - CLIP_SENSOR_REFLECT_MARGIN`. This is the default; the
  baseline is re-sampled on every pairing.
- `CLIP_SENSOR_PHOTOTRANSISTOR` — open = ambient light above `CLIP_SENSOR_LIGHT_THRESHOLD`
  (a photoresistor/LDR works the same way).
- `CLIP_SENSOR_SIMULATED` — no GPIO; the box state is forced so each path can be tested
  without hardware.

Set `CLIP_SENSOR_DEBUG` to `1` to stream the raw ADC value (and the resulting open
state) over serial for calibration — works without a hub or schedule.

## How it works (workflow)

1. **Hub boot** — joins WiFi, fetches NTP time, restores its clip registry, pill list,
   reliable outbound queue and adherence log from NVS, then starts beacons every
   `HUB_BEACON_INTERVAL_MS` (250 ms) on the WiFi channel.
2. **Clip boot / discovery** — the clip brings up ESP-NOW and scans every channel
   (`CLIP_SCAN_DWELL_MS` = 600 ms each) for the hub's beacon. On a deep-sleep wake it
   restores the remembered hub from RTC memory and skips the scan.
3. **Registration (HELLO)** — the clip sends HELLO; the hub registers it (max 4) and
   ACKs with its node id.
4. **Pairing / assignment** — from the Hub menu ("Add a pill"), the user picks a free
   clip (it blinks to identify itself), names the pill, sets contents and a weekly
   schedule. The hub then sends `MSG_ASSIGN` (node id + schedule + current time)
   reliably. On receiving it, the clip sets its RTC, stores the schedule, re-samples
   the IR "closed" baseline, and reports its battery.
5. **Scheduling + deep sleep** — the clip computes the next intake and next hourly
   re-sync, then sleeps until the sooner of the two (gated by `DEEP_SLEEP_ON`).
6. **Intake** — at a scheduled time the clip wakes and sounds the two-tone siren. When
   the box is opened (IR reading drops below baseline), the clip marks the dose
   `TAKEN` and reports it. If the box stays closed past the 30-minute window it
   reports `MISSED`; after 5 minutes it escalates to a faster, more urgent siren. If
   the box is left open, it chirps a close-box reminder and, after the grace period,
   reports `BOX_OPEN`.
7. **Adherence to the Hub** — the Hub applies each event to the pill's counters,
   ACKs it, and appends `(node_id, type, seq, timestamp)` to a persistent, bounded
   ring log (survives reboot; de-duplicated by `seq`). The accessors
   `hub_comms_adherence_count()` / `hub_comms_adherence_get()` / `hub_comms_adherence_clear()`
   expose this log for tomorrow's remote-server client.
8. **Re-sync + battery** — every `CLIP_RESYNC_INTERVAL_SEC` (1 h) the clip re-anchors
   its clock from the hub and piggybacks a battery reading; the Hub stores
   `last_battery_mv` per node and warns below `BATTERY_LOW_MV`.
9. **Recovery** — a clip that reboots cold (RTC wiped) re-HELLOs and the hub re-sends
   its schedule; a clip that lost its hub re-scans; a hub reboot restores state from
   NVS without double-counting adherence.

ESP-NOW receive callbacks run in the ESP-NOW task and only push frames into ring
buffers (`lib/comms_ring`) or set small flags; the main loop drains them, keeping all
shared state on a single thread. Sends, Serial and LCD are only done from the main loop.

## Sensor calibration

The reflective-IR (and phototransistor) thresholds are placeholders until you measure
your own sensor. There is a bench firmware for each backend that streams the raw ADC
value plus rolling stats without needing a hub or schedule:

1. Flash the bench matching your sensor:
   - `pio run -e bench-ir -t upload` — reflective IR (open = **low** reading).
   - `pio run -e bench-photodiode -t upload` — phototransistor (open = **high** reading).
2. Point the sensor at a **closed** box and note the reading, then at an **open** box.
   The Python tools in `scripts/` (`ir_run.py` / `ir_logger.py` / `ir_analyze.py` and
   the `photodiode_*` equivalents) log and analyze these runs; see `test_logs/README.md`
   for the format and the `min_closed - max_open` separation the analyzer reports.
3. Set the threshold in `lib/clip_adherence/clip_adherence.cpp`:
   - IR reflective: the firmware derives the open threshold from the stored baseline,
     so calibrating mostly confirms `CLIP_SENSOR_REFLECT_MARGIN` is comfortably inside
     the closed↔open gap; `CLIP_SENSOR_REFLECT_THRESHOLD` remains the fallback.
   - Phototransistor: set `CLIP_SENSOR_LIGHT_THRESHOLD` between the closed and open
     readings.

## Directory structure

```
src/Hub/main.cpp          Hub firmware (LCD + button menu, pill list, NVS persistence)
src/Clip/main.cpp         Clip firmware (comms ↔ alarm wiring, battery, deep sleep + re-sync)
src/BenchIR/              Standalone reflective-IR box-sensor bench firmware
src/BenchPhotodiode/      Standalone phototransistor box-sensor bench firmware
lib/view_management/      Menu/view library (Menu struct, create_view())
lib/node_management/      Clip registry (MAC + discovered/assigned flags, max 4, NVS)
lib/protocol/             Shared Hub↔Clip wire types (schedule, time, messages, battery)
lib/comms_ring/           SPSC ring buffer for inbound ESP-NOW frames
lib/reliable_tx/          Reliable outbound delivery (queue + retransmit until ACK, CRC)
lib/hub_comms/            Hub-side ESP-NOW + WiFi/NTP + adherence log + battery
lib/clip_comms/           Clip-side ESP-NOW (scan, HELLO/register, ASSIGN, battery, RTC)
lib/clip_alarm/           Clip alert output (LED + LEDC two-tone buzzer, 3 patterns)
lib/clip_adherence/       Clip adherence brain (box + intake state machines, IR baseline)
scripts/                  Python bench tooling (run/logger/analyze per backend)
test_logs/                Human-collected bench logs, one subdir per sensor
docs/                     Reference papers (PDFs) and architecture diagrams (Fig2–Fig6)
arduino_uno_photodiode_bench/  Self-contained Arduino Uno copy of the photodiode bench
```

## Known limitations

- **No encryption / pairing / auth** — `encrypt = false`; any ESP-NOW peer on the
  channel can register as a clip. BEACON and HELLO are version-checked
  (`PROTOCOL_VERSION`) and reliable messages are length-checked, but there is no
  cryptographic authentication.
- **DST-correct scheduling** — the clip has no timezone; the hub sends timezone-adjusted
  local time and the clip treats days as fixed 24 h. Fine for a fixed-offset demo
  (CET), but it will drift around DST transitions.
- **Box-sensor thresholds** — the reflective-IR baseline is auto-calibrated per pairing,
  but the fixed fallback thresholds are still uncalibrated placeholders until you run
  the bench (see [Sensor calibration](#sensor-calibration)).
- **Battery** — the divider ratio is a build-time constant; there is no per-board
  calibration of the ADC reference, so reported millivolts are approximate.

## References

Design notes and the underlying papers live in `docs/`, including the architecture
diagrams (`Fig2_system_overview`, `Fig3_hub_architecture`, `Fig4_node_architecture`,
`Fig5_node_statechart`, `Fig6_opening_detection`).
