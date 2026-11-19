# Smart Pill Dispenser - Flask Backend

Lightweight Flask backend connecting the ESP32 hardware (Smart Pill Dispenser) to the shared Firebase Firestore database (`pill_logs` collection).

---

## 1. Quick Start

### Prerequisites
Install Python dependencies:
```bash
pip install flask firebase-admin requests
```

### Credentials
Place your Firebase Admin SDK service account key (`*.json`) directly in this directory (`flask_backend/`).
> **Important:** Never commit or upload this `.json` key to GitHub or any public repository. It is already ignored by `.gitignore`.

### Start the Server
```bash
python server.py
```
By default, the server listens on all network interfaces (`0.0.0.0`) on port `5000`. When started, note your machine's local IP address (e.g., `192.168.1.50`).

### Test the Server
In another terminal, run:
```bash
python test_api.py
```
This sends 3 realistic dummy requests to populate Firestore with sample data.

---

## 2. API Endpoint Specification

### `POST /log_pill`
Receives pill box clip sensor events and persists them to Cloud Firestore.

- **URL (Local test):** `http://127.0.0.1:5000/log_pill`
- **URL (LAN / Hardware):** `http://<LAPTOP_LOCAL_IP>:5000/log_pill` (e.g., `http://192.168.1.50:5000/log_pill`)
- **HTTP Method:** `POST`
- **Headers:** `Content-Type: application/json`

### Request Body (JSON)

| Field | Type | Required | Description | Example |
| :--- | :--- | :--- | :--- | :--- |
| `device_id` | string | Optional | Hardware identifier of the ESP device. Defaults to `"unknown"` if omitted. | `"ESP_001"` |
| `slot_id` | integer | Optional | Pill box slot / compartment index. | `1` |
| `clip_state` | string | Optional | State of the clip sensor (`"opened"`, `"closed"`). | `"opened"` |
| `sensor_timestamp` | string | Optional | ISO 8601 formatted timestamp recorded by hardware/sensor. | `"2026-11-19T15:30:00"` |

#### Example Request Payload
```json
{
  "device_id": "ESP_001",
  "slot_id": 1,
  "clip_state": "opened",
  "sensor_timestamp": "2026-11-19T15:30:00"
}
```

### Response

#### Success (`200 OK`)
Returns status code `200` with the saved payload for easy debugging:
```json
{
  "status": "success",
  "data": {
    "device_id": "ESP_001",
    "slot_id": 1,
    "clip_state": "opened",
    "sensor_timestamp": "2026-11-19T15:30:00"
  },
  "saved_payload": {
    "device_id": "ESP_001",
    "slot_id": 1,
    "clip_state": "opened",
    "sensor_timestamp": "2026-11-19T15:30:00"
  }
}
```

---

## 3. Firestore Database Schema

Data is saved to the **`pill_logs`** collection in Firebase Firestore. Each document contains:

| Field | Firestore Type | Source | Description |
| :--- | :--- | :--- | :--- |
| `device_id` | `string` | ESP32 | Identifier of the device (or `"unknown"`). |
| `slot_id` | `number` \| `null` | ESP32 | Target slot / compartment number. |
| `clip_state` | `string` \| `null` | ESP32 | Event type (`"opened"` or `"closed"`). |
| `sensor_timestamp` | `string` \| `null` | ESP32 | Hardware-generated ISO 8601 timestamp. |
| `server_timestamp` | `timestamp` | Server | Exact server timestamp generated via `firestore.SERVER_TIMESTAMP`. |

---

## 4. Teammate Reference Notes

### For the Hardware Teammate (ESP32 / Arduino / PlatformIO)
1. Ensure the ESP32 is connected to the same Wi-Fi network as the laptop running the Flask backend.
2. Define the server URL matching your laptop's current local IP:
   ```cpp
   const char* flaskServerURL = "http://192.168.x.x:5000/log_pill";
   ```
3. When the clip opens or closes, make an HTTP POST request:
   ```cpp
   #include <HTTPClient.h>

   HTTPClient http;
   http.begin(flaskServerURL);
   http.addHeader("Content-Type", "application/json");

   String jsonPayload = "{\"device_id\":\"ESP_001\",\"slot_id\":1,\"clip_state\":\"opened\",\"sensor_timestamp\":\"2026-11-19T15:30:00\"}";
   int httpResponseCode = http.POST(jsonPayload);

   http.end();
   ```

### For the Frontend Teammate (React / Flutter / Web)
1. Connect to the Firestore collection `pill_logs`.
2. Order logs by `server_timestamp` descending to show real-time adherence activity:
   ```javascript
   import { collection, query, orderBy, onSnapshot } from "firebase/firestore";

   const q = query(collection(db, "pill_logs"), orderBy("server_timestamp", "desc"));
   const unsubscribe = onSnapshot(q, (snapshot) => {
     snapshot.docChanges().forEach((change) => {
       if (change.type === "added") {
         console.log("New pill log: ", change.doc.data());
       }
     });
   });
   ```
3. Use `clip_state` (`"opened"` vs `"closed"`) and `sensor_timestamp` to determine intake adherence and duration.
