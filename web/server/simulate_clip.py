#!/usr/bin/env python3
"""
Pretend to be the ESP32 clip / central hub to test without physical hardware.
Events are sent to the MedClip server and persisted directly into Cloud Firestore.

Usage:
  python simulate_clip.py on F00D01                      # Clip announces online -> appears in Setup
  python simulate_clip.py on F00D01 --name "Clip 7"      # Announce with custom name
  python simulate_clip.py taken A1F3B2                    # Medicine taken now (clip_state=opened)
  python simulate_clip.py taken A1F3B2 --at 2026-11-19T08:05
  python simulate_clip.py taken A1F3B2 --slot 1 --state opened
  python simulate_clip.py taken A1F3B2 --no-key           # Test unauthenticated ESP32 flow (no X-Hub-Key)
  python simulate_clip.py taken A1F3B2 --endpoint log_pill  # Test hardware /log_pill route directly

Options:
  --url http://192.168.1.50:8000   Server URL (default: http://localhost:8000)
  --key my-secret                  Hub secret key (X-Hub-Key; default: 'change-me')
  --no-key                         Omit X-Hub-Key header to test unauthenticated hardware flow
  --endpoint {clip-events,log_pill} Target API route (default: clip-events)
"""
import argparse
from datetime import datetime
import json
import os
import urllib.error
import urllib.request

parser = argparse.ArgumentParser(description="Send simulated ESP clip / hub messages to the MedClip Firestore server")
parser.add_argument("action", choices=["on", "taken", "close"], help="on = clip connected, taken = medicine taken (opened), close = clip closed")
parser.add_argument("clip_id", help="the clip's id or hardware device_id, e.g. A1F3B2 or ESP_001")
parser.add_argument("--name", help="name for the clip (with 'on'); default: server picks 'Clip N'")
parser.add_argument("--slot", type=int, default=1, help="slot ID for the pill box (default: 1)")
parser.add_argument("--state", choices=["opened", "closed"], help="clip state override (default: opened for 'taken', closed for 'close')")
parser.add_argument("--at", help="time recorded by sensor (YYYY-MM-DDTHH:MM or ISO timestamp); default now")
parser.add_argument("--endpoint", choices=["clip-events", "log_pill"], default="clip-events", help="target API route (default: clip-events)")
parser.add_argument("--url", default="http://localhost:8000", help="server address")
parser.add_argument("--key", default=os.environ.get("MEDCLIP_HUB_KEY", "change-me"), help="hub key (X-Hub-Key; default: 'change-me')")
parser.add_argument("--no-key", action="store_true", help="omit X-Hub-Key header to test unauthenticated hardware flow")
args = parser.parse_args()

now_iso = datetime.now().replace(microsecond=0).isoformat()
sensor_ts = args.at if args.at else now_iso

if args.action == "on":
    path = "/api/clips/announce"
    payload = {"clip_id": args.clip_id}
    if args.name:
        payload["name"] = args.name
else:
    # Action is 'taken' or 'close'
    default_state = "closed" if args.action == "close" else "opened"
    state = args.state or default_state
    
    if args.endpoint == "log_pill":
        path = "/log_pill"
        payload = {
            "device_id": args.clip_id,
            "slot_id": args.slot,
            "clip_state": state,
            "sensor_timestamp": sensor_ts
        }
    else:
        path = "/api/clip-events"
        payload = {
            "clip_id": args.clip_id,
            "device_id": args.clip_id,
            "slot_id": args.slot,
            "clip_state": state,
            "taken_at": sensor_ts,
            "sensor_timestamp": sensor_ts
        }

headers = {"Content-Type": "application/json"}
# For /log_pill, default to no X-Hub-Key header unless explicitly provided with non-default key
# For /api/clip-events, send X-Hub-Key unless --no-key is specified
if not args.no_key:
    if args.endpoint == "clip-events" or args.action == "on":
        if args.key:
            headers["X-Hub-Key"] = args.key
    elif args.key and args.key != "change-me":
        headers["X-Hub-Key"] = args.key

url_full = args.url.rstrip("/") + path
print(f"POST {url_full}")
print(f"Headers: {headers}")
print(f"Payload: {json.dumps(payload, indent=2)}")

req = urllib.request.Request(
    url_full,
    data=json.dumps(payload).encode(),
    headers=headers,
    method="POST",
)

try:
    with urllib.request.urlopen(req, timeout=5) as res:
        print(f"Response ({res.status}):", json.dumps(json.loads(res.read()), indent=2))
except urllib.error.HTTPError as e:
    print(f"HTTP Error {e.code}:", e.read().decode())
except urllib.error.URLError as e:
    print("Could not reach the server:", e.reason)
