#!/usr/bin/env python3
"""
Pretend to be the central hub, to test without the real clips.

  python3 simulate_clip.py on F00D01            a clip switched on -> appears in Setup
  python3 simulate_clip.py on F00D01 --name "Clip 7"
  python3 simulate_clip.py taken A1F3B2          that clip's medicine was taken now
  python3 simulate_clip.py taken A1F3B2 --at 2026-11-19T08:05
  add --url http://192.168.1.50:8000 --key my-secret to reach another hub
"""
import argparse
import json
import os
import urllib.error
import urllib.request

parser = argparse.ArgumentParser(description="Send a fake hub message to the MedClip server")
parser.add_argument("action", choices=["on", "taken"], help="on = clip connected, taken = medicine taken")
parser.add_argument("clip_id", help="the clip's id, e.g. A1F3B2")
parser.add_argument("--name", help="name for the clip (with 'on'); default: the server picks 'Clip N'")
parser.add_argument("--at", help="time the medicine was taken (YYYY-MM-DDTHH:MM); default now")
parser.add_argument("--url", default="http://localhost:8000", help="hub server address")
parser.add_argument("--key", default=os.environ.get("MEDCLIP_HUB_KEY", "change-me"), help="hub key (X-Hub-Key)")
args = parser.parse_args()

if args.action == "on":
    path, payload = "/api/clips/announce", {"clip_id": args.clip_id}
    if args.name:
        payload["name"] = args.name
else:
    path, payload = "/api/clip-events", {"clip_id": args.clip_id}
    if args.at:
        payload["taken_at"] = args.at

req = urllib.request.Request(
    args.url.rstrip("/") + path,
    data=json.dumps(payload).encode(),
    headers={"Content-Type": "application/json", "X-Hub-Key": args.key},
    method="POST",
)
try:
    with urllib.request.urlopen(req, timeout=5) as res:
        print(res.status, json.loads(res.read()))
except urllib.error.HTTPError as e:
    print(e.code, e.read().decode())
except urllib.error.URLError as e:
    print("Could not reach the server:", e.reason)
