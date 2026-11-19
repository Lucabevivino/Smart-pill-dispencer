# MedClip – smart medicine clip

A clip goes on each medicine box. When the patient takes a tablet the clip
tells the central hub, the hub tells this server, and the web app shows it on
the dashboard and calendar.

```
medclip/
├── frontend/          the web app (served by the server)
│   ├── index.html     pages: login, dashboard, calendar, medicines, setup
│   ├── styles.css
│   └── app.js
├── server/
│   ├── server.py         server: API + database + serves the web app
│   └── simulate_clip.py  pretends to be the hub, for testing without hardware
└── README.md
```

Needs only **Python 3.9 or newer**. Nothing to install.

## 1. Start the server

```bash
cd server
python3 server.py --seed     # --seed adds one example clip with one medicine; leave it out for an empty start
```

Open **http://localhost:8000** and sign in with:

- email `patient@medclip.local`
- password `medclip123`

Other phones and laptops on the same Wi-Fi can open `http://<this-computer's-IP>:8000`.
Everything is stored in `server/medclip.db`. Delete that file to start over.

## 2. Pair clips (Setup page)

1. Switch a clip on and put it on a medicine box. When it joins the Wi-Fi, the
   hub reports it and it appears in Setup within a few seconds, named *Clip 1*,
   *Clip 2*, ... (or whatever name the hub gives it).
2. In **Setup**, pick the clip, type the **medicine name**, the **dose times** and the **days**.
   *More details* (strength, tablets in the box, instructions...) is optional.
3. Click **Pair clip**.

**Unpair** frees the clip again (and deletes that medicine's dose history).
**Forget** removes a free clip that no longer exists.

## 3. What the central hub sends

All messages are `POST` with JSON and the header `X-Hub-Key: change-me`
(change the key by starting the server with `MEDCLIP_HUB_KEY=your-secret python3 server.py`).

### A clip switched on / joined the Wi-Fi

```
POST http://<server-ip>:8000/api/clips/announce
{"clip_id": "A1F3B2"}
```

- `clip_id`: any unique id for the clip (its MAC address, chip id, ...). Case, spaces and colons don't matter.
- Optional `"name": "Clip 3"`. If the hub doesn't send a name, the server picks the next free *Clip N*.
- The hub can also send all clips at once: `{"clips": [{"clip_id": "A1F3B2"}, {"clip_id": "B77C10", "name": "Clip 2"}]}`
- Sending it again is fine (it updates *last seen*), so the hub can repeat it every few minutes as a heartbeat.

Reply: `{"clips": [{"clip_id": "A1F3B2", "name": "Clip 1", "paired_with": null}]}`

### A clip's medicine was taken

```
POST http://<server-ip>:8000/api/clip-events
{"clip_id": "A1F3B2"}
```

- Optional `"taken_at": "2026-11-19T08:05:00"`, only if the hub knows the real time (NTP).
  Without it the server uses the time the message arrives.
- Send it **once per dose**, when the clip decides the tablet was taken. Not a stream of sensor readings.

| Situation | Reply |
|---|---|
| A dose of the paired medicine is due (60 min before to 3 h after) | `200 {"status":"recorded", "clip":"Clip 1", "medicine":"Metformin", "scheduled":"...", "minutesFromSchedule":4}` |
| Paired, but no dose due now (or already taken) | `200 {"status":"no_scheduled_dose"}` |
| Clip not paired yet | `202 {"status":"not_paired"}` (the clip is added to Setup if it was new) |
| Wrong key | `403` |

On time = taken within 30 min of schedule; missed = nothing 3 h after.
(The older `/api/chip-events` with `chip_id` still works.)

### Test without hardware

```bash
cd server
python3 simulate_clip.py on F00D01       # a new clip switches on -> appears in Setup
python3 simulate_clip.py taken A1F3B2    # Clip 1's medicine taken now
python3 simulate_clip.py taken A1F3B2 --at 2026-11-19T08:05
```

### ESP32 example (Arduino IDE)

```cpp
#include <WiFi.h>
#include <HTTPClient.h>

const char* WIFI_SSID  = "your-wifi";
const char* WIFI_PASS  = "your-password";
const char* SERVER     = "http://192.168.1.50:8000";   // computer running server.py
const char* HUB_KEY    = "change-me";

int post(const String& path, const String& json) {
  HTTPClient http;
  http.begin(String(SERVER) + path);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Hub-Key", HUB_KEY);
  int code = http.POST(json);
  Serial.printf("%s -> %d %s\n", path.c_str(), code, http.getString().c_str());
  http.end();
  return code;
}

// call when a clip connects to the hub
void clipOnline(const String& clipId) { post("/api/clips/announce", "{\"clip_id\":\"" + clipId + "\"}"); }

// call when a clip detects its medicine was taken
void medicineTaken(const String& clipId) { post("/api/clip-events", "{\"clip_id\":\"" + clipId + "\"}"); }

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(300);
}

void loop() { /* call clipOnline(...) / medicineTaken(...) from your clip logic */ }
```

## API used by the web app

Need `Authorization: Bearer <token>` from `/api/login` (except `/api/status`).

| Method & path | Purpose |
|---|---|
| `GET /api/status` | server online, last clip signal, number of clips / free clips |
| `POST /api/login` `{email, password}` | returns `{token, user}` |
| `GET /api/clips` | clips the hub reported, with the medicine each is paired with |
| `DELETE /api/clips/<clip_id>` | forget a free clip |
| `GET /api/medicines` | paired medicines (with `clipId`, `clipName`) |
| `POST /api/medicines` / `PUT /api/medicines/<id>` | pair a clip with a medicine / edit |
| `DELETE /api/medicines/<id>` | unpair (clip becomes free, history deleted) |
| `GET /api/intakes?from=YYYY-MM-DD&to=YYYY-MM-DD` | doses recorded by clips or ticked by hand |
| `POST /api/intakes` `{medId, scheduled, takenAt or null}` | patient ticks / unticks a dose |

## Notes

- The server and the browser should be in the same time zone: dose times are local times.
- Sign-ins are kept in memory, so everyone signs in again after the server restarts.
- Opening `frontend/index.html` directly (without the server) runs the app on demo data.
- Student prototype: plain HTTP on the local network. Add HTTPS before using it anywhere public.
- The Flutter app from the earlier version is not included: it still uses typed chip numbers
  and doesn't work with this server version.
