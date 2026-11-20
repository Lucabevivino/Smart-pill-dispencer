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

Follow this step-by-step guide to configure, calibrate, flash, and run the physical ESP32 Hub and Clip hardware alongside the unified backend and web application.

---

### 1. Hardware Overview & Wiring

| Device | Microcontroller | Primary Sensors & Actuators | Power |
| :--- | :--- | :--- | :--- |
| **Hub** | ESP32 WROOM DevKit | 16×2 HD44780 LCD (4-bit), 3 navigation buttons (UP/DOWN/SELECT) | 5V USB-C / Micro-USB |
| **Clip** | ESP32-C3 (DFRobot Beetle) | Reflective IR box sensor (`GPIO 0`), Buzzer (`GPIO 7`), Alert LED (`GPIO 5`), Battery sense (`GPIO 3`) | 3.7V LiPo or USB-C |

#### Hub Pin Connections (ESP32 WROOM):
- **16×2 Parallel LCD (HD44780 4-bit mode):**
  - `LCD_RS` = **GPIO 23**
  - `LCD_EN` = **GPIO 22**
  - `LCD_D4` = **GPIO 21**
  - `LCD_D5` = **GPIO 19**
  - `LCD_D6` = **GPIO 18**
  - `LCD_D7` = **GPIO 4**
  - > [!IMPORTANT]
    > **LCD Contrast Potentiometer:** LCD Pin 3 ($V_0$) must connect to the wiper of a **10 kΩ potentiometer** (wired between VDD and GND). If the backlight turns on but no text appears (or only solid white/blue blocks appear), **turn the potentiometer screw** until the characters become crisp and visible.
- **Navigation Buttons (Active-Low with internal pull-up):**
  - `UP` = **GPIO 13** (Wire button between GPIO 13 and GND)
  - `DOWN` = **GPIO 14** (Wire button between GPIO 14 and GND)
  - `SELECT` = **GPIO 12** (Wire button between GPIO 12 and GND)

#### Clip Pin Connections (ESP32-C3):
- **Box Sensor (ADC-capable):** `GPIO 0` (TCRT5000 IR sensor or photoresistor signal pin)
- **Alert LED:** `GPIO 5` (Active-high via 220–330 Ω resistor)
- **Passive Buzzer:** `GPIO 7` (Driven via ESP32 LEDC PWM timer)
- **Battery Voltage Sense:** `GPIO 3` (ADC1_CH3 via a 2:1 100 kΩ/100 kΩ resistor divider, or set to `0xFF` to disable)

---

### 2. Hardware Pre-Flight Configuration Checklist

Before compiling or flashing the firmware, check and customize these 5 files:

#### A. Configure Wi-Fi Credentials (`include/wifi_config.h`)
This file is **gitignored** so personal network passwords are never leaked. Create `include/wifi_config.h`:
```cpp
#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

// MUST be a 2.4 GHz Wi-Fi network (ESP32 cannot connect to 5 GHz)
#define WIFI_SSID     "Your-Actual-WiFi-SSID"
#define WIFI_PASSWORD "Your-Actual-WiFi-Password"

#endif
```

#### B. Configure Server IP Address (`flaskServerURL`)
The ESP32 cannot reach `localhost` or `127.0.0.1`. It needs the local IPv4 address of the laptop running `web/server/server.py`:
1. Open PowerShell / Terminal on the laptop and run:
   - **Windows:** `ipconfig` (Look for `IPv4 Address` under your active Wi-Fi adapter, e.g., `192.168.1.50`)
   - **macOS / Linux:** `ifconfig` or `ip a`
2. Point the server URL to your laptop's IP on port **8000**:
   ```cpp
   const char* serverURL = "http://192.168.1.50:8000/log_pill";
   ```
3. > [!WARNING]
   > **Windows Firewall:** Ensure your Windows network profile is set to **Private** and Windows Defender Firewall permits incoming connections on port `8000` (or `5000`), otherwise the ESP32 connection will time out.

#### C. Set Local Timezone ([src/Hub/main.cpp](file:///C:/Users/shash/Smart-pill-dispencer/src/Hub/main.cpp#L9-L11))
By default, the Hub is set to Central European Time (CET/CEST UTC+1/+2). Adjust to match your local timezone:
```cpp
#define TZ_GMT_OFFSET_SEC  3600    // GMT offset in seconds (e.g. UTC+1 = 3600, UTC-5 = -18000)
#define TZ_DST_OFFSET_SEC  3600    // Daylight saving offset (3600 if active, 0 if not used)
```

#### D. Select Sensor Type & Margin ([lib/clip_adherence/clip_adherence.cpp](file:///C:/Users/shash/Smart-pill-dispencer/lib/clip_adherence/clip_adherence.cpp#L12-L40))
Verify `CLIP_SENSOR_TYPE` matches your physical clip sensor:
```cpp
// Choose your active hardware backend:
#define CLIP_SENSOR_TYPE  CLIP_SENSOR_IR_REFLECTIVE   // For TCRT5000 IR reflective module (default)
// #define CLIP_SENSOR_TYPE  CLIP_SENSOR_PHOTOTRANSISTOR // For photoresistor / ambient light sensor

// For IR reflective, "open" means reading drops by this margin below the closed baseline:
#define CLIP_SENSOR_REFLECT_MARGIN  500
```
> [!TIP]
> **Live ADC Calibration:** Set `#define CLIP_SENSOR_DEBUG 1` in `clip_adherence.cpp` to stream live ADC readings over the Serial monitor (115200 baud) when opening/closing the box.

#### E. Bench Testing Flags ([src/Clip/main.cpp](file:///C:/Users/shash/Smart-pill-dispencer/src/Clip/main.cpp#L25-L35))
While prototyping on a breadboard over USB:
1. **Disable Deep Sleep for USB Debugging:**
   ```cpp
   #define DEEP_SLEEP_ON 0   // Set to 0 while testing on breadboard / USB serial
   ```
   *Why?* When `DEEP_SLEEP_ON 1` is active, the ESP32-C3 enters sleep between events, which disconnects the native USB CDC serial port. Setting it to `0` keeps the board awake with continuous serial logging. Switch back to `1` when deploying on battery.
2. **Disable Battery Sensing if Unwired:**
   ```cpp
   #define CLIP_BATTERY_PIN 0xFF  // Disables floating ADC battery alerts if no divider is wired
   ```

---

### 3. Flash the Firmware (PlatformIO)

Connect each board via USB to your computer and run:

```bash
# 1. Flash the ESP32 Hub (Controller + LCD)
pio run -e esp32-wroom-devkitm-1 -t upload

# 2. Flash the ESP32-C3 Clip (Pill Box Sensor)
pio run -e esp32-c3-devkitm-1 -t upload

# 3. Monitor live serial logs (115200 baud)
pio run -e esp32-wroom-devkitm-1 -t monitor
# or Clip serial:
pio run -e esp32-c3-devkitm-1 -t monitor
```

---

### 4. Forwarding Hardware Events to Cloud Firestore

In [src/Hub/main.cpp](file:///C:/Users/shash/Smart-pill-dispencer/src/Hub/main.cpp#L410-L460), the function `handle_hub_events()` catches adherence events received from clips over ESP-NOW (`MSG_TAKEN`, `MSG_MISSED`, `MSG_BOX_OPEN`).

To forward these events directly to the Flask backend and Cloud Firestore, implement an HTTP POST call:

```cpp
#include <WiFi.h>
#include <HTTPClient.h>

// Direct hardware endpoint (zero-config, no auth required):
const char* flaskServerURL = "http://192.168.1.50:8000/log_pill";

void sendPillEvent(const char* deviceId, int slotId, const char* state, const char* timestamp) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] Wi-Fi not connected, cannot send pill event");
    return;
  }
  HTTPClient http;
  http.begin(flaskServerURL);
  http.addHeader("Content-Type", "application/json");

  String json = "{\"device_id\":\"" + String(deviceId) + 
                "\",\"slot_id\":" + String(slotId) + 
                ",\"clip_state\":\"" + String(state) + 
                "\",\"sensor_timestamp\":\"" + String(timestamp) + "\"}";

  int httpCode = http.POST(json);
  Serial.printf("[HTTP] Logged event to backend, code: %d\n", httpCode);
  http.end();
}
```

Call `sendPillEvent()` in `handle_hub_events()` under `case MSG_TAKEN:` and `case MSG_BOX_OPEN:`:
```cpp
case MSG_TAKEN:
    pill = find_pill_by_node(node_id);
    if (pill != NULL) {
        // ... update local pill counts ...
        pill_list_save();
    }
    // Forward to Cloud Firestore via Flask backend
    sendPillEvent("ESP_001", node_id, "opened", "2026-11-20T15:30:00");
    break;
```

---

### 5. Full End-to-End Execution Guide (Hardware + Cloud)

Follow this sequence to run the complete hardware-to-cloud system:

1. **Start the Laptop Backend:**
   ```powershell
   cd web\server
   .\run_dev.ps1
   ```
   Note your laptop's local IP address (e.g., `192.168.1.50`).
2. **Power on the Hub:**
   - The 16×2 LCD displays `Connecting WiFi` and `Please wait`.
   - Once connected, it syncs NTP time and starts broadcasting ESP-NOW beacons.
3. **Power on the Clip:**
   - The Clip performs an alarm self-test (chirp + LED flash).
   - The Clip scans Wi-Fi channels, discovers the Hub, and sends a `HELLO` registration.
4. **Pair the Clip to Medication:**
   - Open **`http://localhost:8000`** in your browser.
   - Go to the **Setup** tab: the new clip appears in the list.
   - Enter medication details (e.g., *Metformin 500mg*), choose scheduled intake times (e.g. `08:00`, `20:00`), and click **Pair Clip**.
5. **Simulate or Trigger an Intake:**
   - At the scheduled dose time, the Clip emits an audible alarm and flashes its LED.
   - Open the pill box lid:
     - The IR reflective sensor detects the lid opening (`baseline - 500`).
     - The Clip stops the alarm and transmits `MSG_TAKEN` to the Hub via ESP-NOW.
     - The Hub displays `Box open! Clip #1` on the LCD and fires an HTTP POST to `http://192.168.1.50:8000/log_pill`.
     - The Flask backend saves the event to Cloud Firestore (`pill_logs` collection) and decrements remaining medication inventory.
     - The Web Dashboard and Calendar update immediately with on-time adherence stats!

---

### 6. Hardware Troubleshooting Matrix

| Problem | Cause | Quick Fix |
| :--- | :--- | :--- |
| **LCD displays blank blue screen or solid white blocks** | Contrast potentiometer not calibrated | Turn the screw on the 10 kΩ potentiometer wired to LCD Pin 3 ($V_0$) until letters appear. |
| **Hub hangs on `Connecting WiFi`** | Wrong credentials or 5 GHz band | Verify `include/wifi_config.h`. Ensure your router broadcasts a 2.4 GHz network (ESP32 cannot connect to 5 GHz). |
| **HTTP POST returns `-1` / connection refused / timed out** | Incorrect IP or Firewall blocked | Run `ipconfig` to re-check your laptop's IP address. In Windows Firewall settings, allow incoming traffic on port `8000`. |
| **Clip disconnects from USB Serial every 30 seconds** | ESP32 deep sleep enabled | Set `#define DEEP_SLEEP_ON 0` in `src/Clip/main.cpp` while debugging on USB. |
| **Clip triggers false `TAKEN` when box is still closed** | IR margin too sensitive | Run `pio run -e bench-ir -t upload` to inspect raw ADC. Increase `CLIP_SENSOR_REFLECT_MARGIN` (e.g., from `500` to `700`) in `lib/clip_adherence/clip_adherence.cpp`. |
| **False low-battery alert on Hub LCD** | Unwired ADC pin floating | Set `#define CLIP_BATTERY_PIN 0xFF` in `src/Clip/main.cpp` if no battery divider is wired on the breadboard. |

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
