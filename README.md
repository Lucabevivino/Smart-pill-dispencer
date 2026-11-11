# Smart Pill Dispenser

A two-device ESP32 system that helps people take their medication on time. A central **Hub** keeps track of every pill box and its weekly schedule; a small **Clip** attached to each pill box lights up and sounds an alarm when it is time to take that pill, then reports back whether the dose was taken, missed, or the box was left open.

Built with [PlatformIO](https://platformio.org/) (Arduino framework). Devices talk over [ESP-NOW](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_now.html) — no router or internet connection required between them.

## Devices

| Device | Board | Role |
| --- | --- | --- |
| **Hub** | ESP32 (WROOM devkit) | Main controller. Drives a 16×2 LCD + three-button menu, runs WiFi + NTP, discovers clips, and assigns each pill a weekly schedule and the current time. |
| **Clip** | ESP32-C3 (DFRobot Beetle) | Companion device per pill box. Registers with the hub, blinks on demand, sounds the intake alarm from its assigned schedule, and reports adherence via a box sensor. |

## Features

- **Pill management on the Hub** — list, add, refill and remove pills (up to 4 clips, nickname ≤ 5 chars) with counts (`taken` / `total` / `remaining`).
- **Weekly schedules** — each pill has a per-weekday schedule with up to `MAX_INTAKES_PER_DAY` intake times.
- **Automatic clip discovery** — the Hub beacons every 250 ms; clips scan all WiFi channels and register via HELLO.
- **Time sync** — the Hub distributes NTP time to clips so they alarm at the right local time even after deep sleep.
- **Reliable delivery** — scheduled/alarm messages carry a `seq` byte and are retransmitted with backoff until ACKed (Hub persists its queue to NVS, the Clip keeps its queue in RTC memory).
- **Adherence reporting** — each clip reports `MSG_TAKEN`, `MSG_MISSED`, or `MSG_BOX_OPEN` for every intake window.
- **Deep sleep** — clips sleep between events and wake for the next intake or re-sync.

## Getting started

### 1. Prerequisites

- [PlatformIO](https://docs.platformio.org/en/latest/core/installation/index.html) (or the PlatformIO IDE / VS Code extension).
- Two ESP32 boards: an ESP32 devkit for the Hub and an ESP32-C3 (e.g. DFRobot Beetle) for each Clip.

### 2. Configure WiFi credentials

The Hub needs WiFi for NTP time. Create `include/wifi_config.h` (this file is gitignored, so it won't be committed):

```cpp
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#define WIFI_SSID          "your-ssid"
#define WIFI_PASSWORD      "your-password"

#endif // WIFI_CONFIG_H
```

### 3. Build & flash

```sh
# Hub (board: esp32dev)
pio run -e esp32-wroom-devkitm-1 -t upload

# Clip (board: dfrobot_beetle_esp32c3)
pio run -e esp32-c3-devkitm-1 -t upload
```

Serial runs at 115200 baud:

```sh
pio run -e esp32-wroom-devkitm-1 -t monitor
pio run -e esp32-c3-devkitm-1 -t monitor
```

## Wiring

### Hub — 16×2 HD44780 LCD (4-bit mode)

| LCD pin | Signal | Connect to |
| --- | --- | --- |
| 1 | VSS | GND |
| 2 | VDD | +3.3 V (or +5 V — level-shift data lines if 5 V) |
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
| UP | GPIO 13 | Previous view / go back |
| DOWN | GPIO 14 | Next view |
| SELECT | GPIO 12 | Confirm / enter |

### Clip — LED, buzzer, box sensor

| Signal | Pin (default) | Notes |
| --- | --- | --- |
| LED | `CLIP_ALARM_LED_PIN` = GPIO 5 | Active-high |
| Buzzer | `CLIP_ALARM_BUZZER_PIN` = GPIO 7 | Passive buzzer, LEDC PWM (two-tone siren 950/650 Hz) |
| Box sensor | `CLIP_ADHERENCE_SENSOR_PIN` = GPIO 0 | Must be ADC-capable (GPIO 0–4 on ESP32-C3; avoid GPIO 2, a strapping pin) |

## Configuration

All key settings live as `#define`s at the top of the two entry points (or in the listed headers):

| Setting | Location | Default | Meaning |
| --- | --- | --- | --- |
| `WIFI_SSID` / `WIFI_PASSWORD` | `include/wifi_config.h` | — | Hub WiFi for NTP |
| `TZ_GMT_OFFSET_SEC` / `TZ_DST_OFFSET_SEC` | `src/Hub/main.cpp` | `3600` / `3600` | Local timezone offset (UTC+1 / DST) |
| `CLIP_ALARM_LED_PIN` / `CLIP_ALARM_BUZZER_PIN` | `src/Clip/main.cpp` | 5 / 7 | Clip alert output |
| `CLIP_ADHERENCE_SENSOR_PIN` | `src/Clip/main.cpp` | 0 | Box-sensor input |
| `DEEP_SLEEP_ON` | `src/Clip/main.cpp` | `1` | Enable deep sleep between events |
| `CLIP_SENSOR_TYPE` | `lib/clip_adherence/clip_adherence.cpp` | `CLIP_SENSOR_SIMULATED` | Sensor backend (see below) |
| `CLIP_RESYNC_INTERVAL_SEC` | `lib/clip_comms/clip_comms.h` | 30 s (dev) | RTC re-anchor interval (intended: 2 h) |

### Box sensor types

`CLIP_SENSOR_TYPE` in `lib/clip_adherence/clip_adherence.cpp` selects how "box open" is detected:

- `CLIP_SENSOR_SIMULATED` — no GPIO, simulates the box (default; works without hardware).
- `CLIP_SENSOR_PHOTOTRANSISTOR` — open = ambient light above `CLIP_SENSOR_LIGHT_THRESHOLD` (a photoresistor/LDR works the same way).
- `CLIP_SENSOR_IR_REFLECTIVE` — open = no reflection, below `CLIP_SENSOR_REFLECT_THRESHOLD` (TCRT5000-style).

Set `CLIP_SENSOR_DEBUG` to `1` to stream the raw ADC value over serial for threshold calibration (works without a hub or schedule).

## How it works

1. The **Hub** joins WiFi, fetches NTP time, and beacons its presence on the current WiFi channel every `HUB_BEACON_INTERVAL_MS` (250 ms).
2. An unassigned **Clip** scans every channel (`CLIP_SCAN_DWELL_MS` = 600 ms) until it hears the beacon, then sends a HELLO to register.
3. The Hub assigns the clip a pill, a weekly schedule, and the current time (reliably, with ACK/retry).
4. The Clip stores its schedule and hub in RTC memory, deep-sleeps, and wakes for the next intake (or re-sync). It re-anchors its clock every `CLIP_RESYNC_INTERVAL_SEC`.
5. On an intake, the Clip flashes the LED and sounds the buzzer for the intake window (30 min), then reports taken / missed / box-left-open back to the Hub.

ESP-NOW receive callbacks run in the ESP-NOW task and only push frames into ring buffers (`lib/comms_ring`); the main loop drains them, keeping all shared state on a single thread.

## Directory structure

```
src/Hub/main.cpp          Hub firmware (LCD + button menu, pill list, NVS persistence)
src/Clip/main.cpp         Clip firmware (comms ↔ alarm wiring, deep sleep + re-sync)
lib/view_management/      Menu/view library (Menu struct, create_view())
lib/node_management/      Clip registry (MAC + discovered/assigned flags, max 4, NVS)
lib/protocol/             Shared Hub↔Clip wire types (schedule, time, ESP-NOW messages)
lib/comms_ring/           SPSC ring buffer for inbound ESP-NOW frames
lib/reliable_tx/          Reliable outbound delivery (queue + retransmit until ACK)
lib/hub_comms/            Hub-side ESP-NOW + WiFi/NTP (beacon, register, assign/blink/time/remove)
lib/clip_comms/           Clip-side ESP-NOW (scan, HELLO/register, ASSIGN, RTC persistence)
lib/clip_alarm/           Clip alert output (LED + LEDC two-tone buzzer)
lib/clip_adherence/       Clip adherence brain (box + intake state machines)
docs/                     Reference papers (PDFs) and architecture diagrams (Fig2–Fig6)
```

## Not yet implemented

- **Box sensor hardware** — the phototransistor and IR-reflective backends are implemented but default to `CLIP_SENSOR_SIMULATED`; no physical opening-detection sensor is wired yet and the ADC thresholds are uncalibrated placeholders.
- **Protocol hardening** — no encryption (`encrypt = false`), no `version` check on receive, no pairing/auth; any ESP-NOW peer on the channel can register.

## References

Design notes and the underlying papers live in `docs/`, including the architecture diagrams (`Fig2_system_overview`, `Fig3_hub_architecture`, `Fig4_node_architecture`, `Fig5_node_statechart`, `Fig6_opening_detection`).
