# MedClip – Smart Medicine Clip (Firestore Backend)

A clip goes on each medicine box. When the patient takes a tablet, the clip reports to the central hub, the hub synchronizes with Google Cloud Firebase Firestore (`pill_logs`, `medicines`, `clips`, `intakes`), and the web app displays adherence on the dashboard and calendar.

```
web/
├── frontend/          the web app (served by the server)
│   ├── index.html     pages: login, dashboard, calendar, medicines, setup
│   ├── styles.css
│   └── app.js
├── server/
│   ├── server.py         server: REST API + Firestore database + serves frontend
│   ├── run_dev.ps1       Windows PowerShell dev launcher
│   ├── run_dev.sh        macOS/Linux dev launcher
│   └── simulate_clip.py  tests clip/hub events without physical hardware
└── README.md
```

## 1. Prerequisites & Setup

Needs **Python 3.9 or newer** and `firebase-admin`:

```bash
pip install firebase-admin
```

### Firebase Credentials Discovery
The server dynamically searches for your private Firebase Admin SDK `.json` key in any of the following locations:
1. `flask_backend/` (recommended)
2. Repository root (`Smart-pill-dispencer/`)
3. `web/server/` or `web/`
4. The path specified in `$env:GOOGLE_APPLICATION_CREDENTIALS` (Windows) or `export GOOGLE_APPLICATION_CREDENTIALS` (macOS/Linux).

> **Security Notice:** The `.json` credentials file contains private keys and must **never** be committed to GitHub. It is protected by `.gitignore`.

---

## 2. Quick Start: Local Development

The server is a unified host: it serves the web app frontend (`index.html`, `styles.css`, `app.js`) and handles all API routes (`/api/*`, `/log_pill`) on the **same port (8000)**. This prevents all cross-origin (CORS) and routing discrepancies.

### Terminal 1: Start the Unified Server

**Using PowerShell (Windows):**
```powershell
# From web/server folder:
.\run_dev.ps1

# Or with custom port:
.\run_dev.ps1 -Port 8000
```

**Or directly with Python:**
```bash
cd web/server
python server.py --seed
```

**Using Bash (macOS / Linux):**
```bash
./run_dev.sh
```

Open **http://localhost:8000** in your browser and sign in with:
- **Email:** `patient@medclip.local`
- **Password:** `medclip123`

---

### Terminal 2: Test & Simulate Events

Keep the server running in Terminal 1, then open a second terminal to simulate clip events:

```bash
cd web/server

# Announce a new clip online -> immediately visible in Setup
python simulate_clip.py on F00D01

# Simulate dose taken now (opened clip on slot 1)
python simulate_clip.py taken A1F3B2

# Simulate specific timestamp matching schedule (e.g. 08:00 or 20:00)
python simulate_clip.py taken A1F3B2 --at 2026-11-20T20:00:00

# Test unauthenticated ESP32 hardware flow on /api/clip-events
python simulate_clip.py taken A1F3B2 --no-key

# Test direct hardware /log_pill endpoint
python simulate_clip.py taken ESP_001 --slot 1 --endpoint log_pill
```

---

## 3. Pair Clips (Setup Page)

1. Switch a clip on and attach it to a pill box. As soon as it announces to the hub, it appears in **Setup** (named *Clip 1*, *Clip 2*, ...).
2. Pick the clip, enter the **medicine name**, **dose times**, and **schedule days**.
3. Click **Pair clip**.

- **Unpair:** Frees the clip and clears that medicine's intake history.
- **Forget:** Removes an unassigned clip that no longer exists.

---

## 4. Hardware & Hub Communication

The server supports two ingestion endpoints. Both endpoints automatically record telemetry in Cloud Firestore in the **`pill_logs`** collection using our agreed hardware schema:
- `device_id` (string, default: `"unknown"`)
- `slot_id` (integer / null)
- `clip_state` (string: `"opened"` or `"closed"`)
- `sensor_timestamp` (ISO string from hardware)
- `server_timestamp` (`firestore.SERVER_TIMESTAMP`)

### Authentication & Header Policy

| Endpoint | `X-Hub-Key` Header | Unauthenticated ESP32 Behavior |
| :--- | :--- | :--- |
| **`POST /log_pill`** | Not required | **Succeeds (200 OK)**. Designed for direct ESP32 hardware without auth overhead. |
| **`POST /api/clip-events`** | Optional during testing | **Succeeds (200 OK / 202)** if omitted. Validated against `MEDCLIP_HUB_KEY` if provided (403 only on mismatched key). |
| **`POST /api/clips/announce`**| Optional during testing | **Succeeds (200 OK)** if omitted. Validated if provided. |

> **Production note:** To secure hub endpoints in production, start the server with `MEDCLIP_HUB_KEY=your-secret python server.py`.

---

### Endpoint: `POST /log_pill` (ESP32 Hardware Direct)
Header: `Content-Type: application/json` (no `X-Hub-Key` required)
```json
{
  "device_id": "ESP_001",
  "slot_id": 1,
  "clip_state": "opened",
  "sensor_timestamp": "2026-11-20T15:30:00"
}
```
Reply (`200 OK`):
```json
{
  "status": "success",
  "data": {
    "device_id": "ESP_001",
    "slot_id": 1,
    "clip_state": "opened",
    "sensor_timestamp": "2026-11-20T15:30:00"
  },
  "saved_payload": { ... },
  "result": "recorded"
}
```

---

### Endpoint: `POST /api/clip-events` (Central Hub / ESP32)
Headers: `Content-Type: application/json` (`X-Hub-Key: change-me` optional)
```json
{
  "clip_id": "A1F3B2",
  "device_id": "A1F3B2",
  "slot_id": 1,
  "clip_state": "opened",
  "taken_at": "2026-11-20T15:30:00"
}
```

---

### ESP32 C++ Code Example (Arduino IDE / PlatformIO)
```cpp
#include <WiFi.h>
#include <HTTPClient.h>

const char* WIFI_SSID  = "your-wifi";
const char* WIFI_PASS  = "your-password";
// Both endpoints work seamlessly on the same port:
const char* SERVER_URL = "http://192.168.1.50:8000/log_pill"; 
// const char* SERVER_URL = "http://192.168.1.50:8000/api/clip-events";

void sendPillEvent(const char* deviceId, int slotId, const char* state, const char* timestamp) {
  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");

  String json = "{\"device_id\":\"" + String(deviceId) + 
                "\",\"slot_id\":" + String(slotId) + 
                ",\"clip_state\":\"" + String(state) + 
                "\",\"sensor_timestamp\":\"" + String(timestamp) + "\"}";

  int code = http.POST(json);
  Serial.printf("Response code: %d, body: %s\n", code, http.getString().c_str());
  http.end();
}
```

> [!TIP]
> **Complete Hardware Setup Guide:** For physical board deployment, refer to [Part 2 of the Root README](../../README.md#part-2-using-the-physical-hardware-esp32-hub--clip). It contains the complete pre-flight checklist: creating `include/wifi_config.h`, LCD contrast pot calibration, disabling deep sleep during USB bench testing (`DEEP_SLEEP_ON 0`), IR sensor margin calibration, and the hardware troubleshooting matrix.

---

## 5. Frontend REST API Reference

All user requests require `Authorization: Bearer <token>` from `/api/login` (except `/api/status`).

| Method & Path | Description |
|---|---|
| `GET /api/status` | Server health, last clip signal, total and free clips |
| `POST /api/login` | `{email, password}` -> returns `{token, user}` |
| `GET /api/clips` | List clips and paired medicine names |
| `DELETE /api/clips/<clip_id>` | Remove an unassigned clip |
| `GET /api/medicines` | List all medicines with schedule, stock, and last signal |
| `POST /api/medicines` | Create a new paired medicine |
| `PUT /api/medicines/<id>` | Update an existing medicine's schedule or details |
| `DELETE /api/medicines/<id>` | Unpair medicine and remove its intake history |
| `GET /api/intakes?from=YYYY-MM-DD&to=YYYY-MM-DD` | Get recorded doses in date range |
| `POST /api/intakes` | `{medId, scheduled, takenAt}` -> Manual patient tick/untick |
