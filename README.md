# Smart Pill Dispenser (MedClip)

An IoT medication adherence system that combines ESP32 smart hardware, a unified Python server, a responsive web dashboard, and Google Cloud Firebase Firestore as the cloud source of truth.

---

## System Overview

```
                      +-----------------------------+
                      |   ESP32 Clip (on pill box)  |
                      |  - IR sensor detects opens  |
                      |  - Buzzer / LED alerts      |
                      +--------------+--------------+
                                     | ESP-NOW (No router needed)
                                     v
                      +-----------------------------+
                      |   ESP32 Hub (controller)    |
                      |  - 16x2 LCD + Button menu   |
                      |  - NTP time sync & storage  |
                      +--------------+--------------+
                                     | Wi-Fi HTTP POST (/log_pill or /api/clip-events)
                                     v
+-------------------------------------------------------------------------+
|                  Unified Server (web/server/server.py)                  |
|  - REST API (:8000) for hardware & simulated telemetry                  |
|  - Static host serving Web Frontend (:8000) with zero CORS issues       |
+--------------------+--------------------------------+-------------------+
                     |                                |
                     v                                v
       +----------------------------+   +----------------------------+
       |   Cloud Firebase Firestore |   |   MedClip Web Frontend     |
       |  - pill_logs (telemetry)   |   |   (Dashboard, Calendar,    |
       |  - medicines & intakes     |   |    Pill Setup, Pair Clips) |
       +----------------------------+   +----------------------------+
```

---

## Quick Start: Running the Whole Program

### 1. Prerequisites
- **Python 3.9+**
- Install dependencies:
  ```bash
  pip install firebase-admin requests
  ```
- *(Optional, for hardware)* [PlatformIO](https://platformio.org/) to build and flash ESP32 firmware.

### 2. Firebase Credentials Setup
Place your Firebase Admin SDK service account `.json` key inside `flask_backend/` (or repository root):
```plaintext
flask_backend/
└── smart-pill-dispenser-*-firebase-adminsdk-*.json
```
> **Security Notice:** All `*.json` key files are strictly ignored by `.gitignore` and must **never** be committed to GitHub.

### 3. Start the Unified Server (Frontend + Cloud Backend)

The server serves both the Web UI and the REST API on **port 8000**:

**On Windows (PowerShell):**
```powershell
cd web\server
.\run_dev.ps1
```
*(Or manually: `python server.py --seed`)*

**On macOS / Linux (Bash):**
```bash
cd web/server
./run_dev.sh
```

### 4. Open the Web Application
Navigate to **`http://localhost:8000`** in any web browser and log in:
- **Email:** `patient@medclip.local`
- **Password:** `medclip123`

---

## Part 1: Using Sample Data (Testing Without Hardware)

You do **not** need physical ESP32 boards to test the full system, evaluate adherence stats, or populate sample data for the frontend.

### A. Automatic Initial Seed
Starting the server with `--seed` automatically configures:
- Default test user (`patient@medclip.local`)
- One paired clip (**Clip 1** / `A1F3B2`)
- One scheduled medication (**Metformin 500 mg**, scheduled daily at `08:00` and `20:00`).

### B. Simulating Clip Events (`simulate_clip.py`)
Leave the server running in Terminal 1, open a **second terminal**, and simulate events:

```bash
cd web/server

# 1. Announce a new clip online (appears immediately in Setup tab)
python simulate_clip.py on F00D01 --name "Clip 2"

# 2. Simulate taking a pill right now (triggers opened state on slot 1)
python simulate_clip.py taken A1F3B2

# 3. Simulate dose taken at a specific scheduled time (e.g., 20:00 on-time intake)
python simulate_clip.py taken A1F3B2 --at 2026-11-20T20:00:00

# 4. Simulate closing the pill box after taking
python simulate_clip.py close A1F3B2 --slot 1

# 5. Simulate hardware event without auth headers (tests unauthenticated ESP flow)
python simulate_clip.py taken A1F3B2 --no-key

# 6. Test the direct hardware /log_pill endpoint
python simulate_clip.py taken ESP_001 --slot 1 --endpoint log_pill
```

### C. Batch Sample Ingestion (`flask_backend/test_api.py`)
To populate Firestore with realistic multi-slot events:
```bash
python flask_backend/test_api.py
```
This sends 3 realistic events (Slot 1 opened, Slot 1 closed, Slot 2 opened) directly to `http://127.0.0.1:5000/log_pill` or port `8000`.

### D. Verify Results in the Web UI
- **Dashboard:** Watch the on-time percentage ring, streak counter, and activity feed update instantly.
- **Calendar:** View doses marked on-time or late, or manually check/uncheck doses.
- **Medicines:** Check inventory counters auto-decrementing as doses are taken.
- **Firestore Console:** Review raw telemetry entries under the `pill_logs` collection.

---

## Part 2: Using the Physical Hardware (ESP32 Hub & Clip)

### 1. Hardware Overview & Wiring

| Device | Microcontroller | Primary Sensors & Actuators |
| :--- | :--- | :--- |
| **Hub** | ESP32 WROOM DevKit | 16×2 HD44780 LCD (4-bit), 3 navigation buttons (UP/DOWN/SELECT) |
| **Clip** | ESP32-C3 (DFRobot Beetle) | Reflective IR box sensor (`GPIO 0`), Buzzer (`GPIO 7`), Alert LED (`GPIO 5`), Battery sense (`GPIO 3`) |

#### Hub Pin Connections:
- `LCD_RS` = GPIO 23, `LCD_EN` = GPIO 22, `LCD_D4..D7` = GPIO 21, 19, 18, 4
- Buttons (Active-Low): `UP` = GPIO 13, `DOWN` = GPIO 14, `SELECT` = GPIO 12

#### Clip Pin Connections:
- Box Sensor: `GPIO 0` (ADC-capable)
- Buzzer: `GPIO 7` (LEDC PWM siren)
- LED: `GPIO 5` (Active-High)

### 2. Configure Wi-Fi Credentials
Create `include/wifi_config.h` (ignored by git):
```cpp
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#define WIFI_SSID     "Your-WiFi-Name"
#define WIFI_PASSWORD "Your-WiFi-Password"

#endif
```

### 3. Flash the Firmware (PlatformIO)
Connect the respective board via USB and run:
```bash
# Upload Hub firmware
pio run -e esp32-wroom-devkitm-1 -t upload

# Upload Clip firmware
pio run -e esp32-c3-devkitm-1 -t upload

# Monitor serial output (115200 baud)
pio run -e esp32-wroom-devkitm-1 -t monitor
```

### 4. Connect Hardware to the Unified Backend
Ensure the ESP32 Hub and the computer running `web/server/server.py` are on the same Wi-Fi network. Point the HTTP POST client in your C++ code to your laptop's local IP address:

```cpp
#include <WiFi.h>
#include <HTTPClient.h>

// Direct hardware endpoint (zero-config, no auth required):
const char* serverURL = "http://192.168.1.50:8000/log_pill";
// Or standard hub endpoint:
// const char* serverURL = "http://192.168.1.50:8000/api/clip-events";

void sendPillEvent(const char* deviceId, int slotId, const char* state, const char* timestamp) {
  HTTPClient http;
  http.begin(serverURL);
  http.addHeader("Content-Type", "application/json");

  String json = "{\"device_id\":\"" + String(deviceId) + 
                "\",\"slot_id\":" + String(slotId) + 
                ",\"clip_state\":\"" + String(state) + 
                "\",\"sensor_timestamp\":\"" + String(timestamp) + "\"}";

  int httpCode = http.POST(json);
  Serial.printf("Server response: %d\n", httpCode);
  http.end();
}
```

### 5. Physical End-to-End Workflow
1. **Boot**: The Hub connects to Wi-Fi, syncs NTP time, and begins ESP-NOW beacons.
2. **Discovery**: Clips power on, scan channels, and register with the Hub via `HELLO`.
3. **Pairing**: In the Hub LCD menu (or via Web **Setup** page), select a clip, assign a medicine and dose schedule.
4. **Intake Window**: At scheduled time, the clip sounds a two-tone alarm and flashes its LED.
5. **Sensor Detection**: The user opens the pill box. The IR sensor detects the drop in reflection (`baseline - margin`) and records `TAKEN`.
6. **Cloud Sync**: The Hub sends an HTTP POST payload to `server.py`, which records the event in Cloud Firestore `pill_logs` and marks the dose taken in `intakes`.

---

## Sensor Calibration Bench

To calibrate custom IR reflective or phototransistor sensors before deployment:

```bash
# Flash reflective-IR calibration bench
pio run -e bench-ir -t upload

# Flash phototransistor calibration bench
pio run -e bench-photodiode -t upload
```

Point the sensor at a closed box, note the baseline reading, then open the box. Set thresholds in `lib/clip_adherence/clip_adherence.cpp`:
- `CLIP_SENSOR_REFLECT_MARGIN`: drop below baseline required to trigger "opened" (default: `500`).
- `CLIP_SENSOR_REFLECT_THRESHOLD`: fallback threshold (default: `1500`).

---

## Repository Directory Map

```
Smart-pill-dispencer/
├── web/
│   ├── frontend/             # MedClip HTML/CSS/JavaScript Web Application
│   │   ├── index.html        # Single-page UI (Dashboard, Calendar, Setup, Login)
│   │   ├── styles.css        # Clean responsive styles
│   │   └── app.js            # Client state, API sync, and event handlers
│   ├── server/               # Unified Server (Static host + Cloud Firestore backend)
│   │   ├── server.py         # REST API & Firestore connection logic
│   │   ├── run_dev.ps1       # Windows PowerShell one-click launcher
│   │   ├── run_dev.sh        # Linux / macOS launcher
│   │   └── simulate_clip.py  # Comprehensive test utility for simulated events
│   └── README.md             # Detailed web app and API reference
├── flask_backend/            # Standalone lightweight Flask logging backend
│   ├── server.py             # Flask server writing directly to Firestore
│   ├── test_api.py           # Sample data generator (3 realistic dummy events)
│   └── README.md             # Flask endpoint and payload documentation
├── src/
│   ├── Hub/main.cpp          # ESP32 Hub controller firmware (LCD, menu, Wi-Fi/NTP)
│   ├── Clip/main.cpp         # ESP32-C3 Clip companion firmware (sensor, buzzer, deep sleep)
│   ├── BenchIR/              # Standalone reflective IR calibration firmware
│   └── BenchPhotodiode/      # Standalone photodiode calibration firmware
├── lib/                      # Core embedded C++ libraries (clip_adherence, hub_comms, protocol)
├── include/                  # Header configs (wifi_config.h, etc.)
└── platformio.ini            # PlatformIO build environments and board configurations
```

---

## Cloud Firestore Data Schema (`pill_logs`)

All hardware and simulated events write to the **`pill_logs`** collection in Cloud Firestore:

| Field | Type | Description |
| :--- | :--- | :--- |
| `device_id` | `string` | Hardware ID or clip MAC (e.g., `"ESP_001"`, `"A1F3B2"`) |
| `slot_id` | `number` \| `null` | Compartment / slot index (`1`, `2`, ...) |
| `clip_state` | `string` | Sensor state: `"opened"` or `"closed"` |
| `sensor_timestamp` | `string` | Hardware-recorded ISO timestamp (`"2026-11-20T15:30:00"`) |
| `server_timestamp` | `timestamp` | Firestore server-side timestamp (`firestore.SERVER_TIMESTAMP`) |
| `med_id` | `number` \| `null` | Associated medicine ID if paired |
| `result` | `string` \| `null` | Outcome: `"recorded"`, `"no_scheduled_dose"`, `"not_paired"` |

---

## License & Credits
Developed as part of the Smart Pill Dispenser (MedClip) IoT Project.
