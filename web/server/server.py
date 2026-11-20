#!/usr/bin/env python3
"""
MedClip central hub server (Unified Firebase Firestore backend).

Runs on Python 3.9+ with firebase-admin. It:
  * serves the web app in ../frontend
  * keeps the list of clips the hub has seen, and gives each one a name
  * stores medicines (each paired with one clip) and doses in Cloud Firestore
  * receives a signal each time a clip says its medicine was taken, matches it to
    the right scheduled dose, and records telemetry in Firestore ('pill_logs')
  * provides backward-compatible support for /log_pill from the ESP32 hardware

Run:   python3 server.py            (then open http://localhost:8000)
       python3 server.py --seed     (adds one example clip with one medicine)
"""
import argparse
import glob
import hashlib
import hmac
import json
import os
import re
import secrets
import sys
import threading
from datetime import date, datetime, timedelta
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

import firebase_admin
from firebase_admin import credentials, firestore

# ----------------------------------------------------------------- settings
HERE = os.path.dirname(os.path.abspath(__file__))
FRONTEND_DIR = os.path.normpath(os.path.join(HERE, "..", "frontend"))
PORT = int(os.environ.get("MEDCLIP_PORT", "8000"))
HUB_KEY = os.environ.get("MEDCLIP_HUB_KEY", "change-me")   # shared secret for hub messages
DEFAULT_EMAIL = os.environ.get("MEDCLIP_EMAIL", "patient@medclip.local")
DEFAULT_PASSWORD = os.environ.get("MEDCLIP_PASSWORD", "medclip123")

# A "taken" signal counts for a dose scheduled up to 60 min later or 3 h earlier.
MATCH_EARLY_MIN = 60
MATCH_LATE_MIN = 180

DB_LOCK = threading.Lock()
SESSIONS = {}  # token -> email (cleared when the server restarts)
ID_RE = re.compile(r"[^0-9A-Z]")
db = None


def norm_id(value):
    """Clip ids: upper case, letters and digits only ("a1:f3 b2" -> "A1F3B2")."""
    return ID_RE.sub("", str(value or "").upper())


def now_local():
    return datetime.now().replace(microsecond=0)


def to_local_naive(text):
    """Parse an ISO time from the browser or the hub; return local time without tz."""
    if not text:
        return None
    text = str(text).strip().replace("Z", "+00:00")
    dt = datetime.fromisoformat(text)
    if dt.tzinfo is not None:
        dt = dt.astimezone().replace(tzinfo=None)
    return dt.replace(microsecond=0)


def get_candidate_key_dirs():
    """Return all candidate directories where the Firebase key might be located."""
    cwd = os.getcwd()
    dirs = [
        os.path.normpath(os.path.join(HERE, "..", "..", "flask_backend")),
        os.path.normpath(os.path.join(HERE, "..", "..")),
        os.path.normpath(os.path.join(cwd, "flask_backend")),
        os.path.normpath(cwd),
        os.path.normpath(os.path.join(HERE, "..")),
        HERE,
        os.path.normpath(os.path.expanduser("~/OneDrive/Desktop/flask_backend")),
        os.path.normpath(os.path.expanduser("~/Desktop/flask_backend")),
    ]
    seen = set()
    deduped = []
    for d in dirs:
        if d not in seen:
            seen.add(d)
            deduped.append(d)
    return deduped


def get_firebase_key_path():
    """Dynamically search for a Firebase service account JSON key."""
    if os.environ.get("GOOGLE_APPLICATION_CREDENTIALS") and os.path.isfile(os.environ["GOOGLE_APPLICATION_CREDENTIALS"]):
        return os.environ["GOOGLE_APPLICATION_CREDENTIALS"]

    search_dirs = get_candidate_key_dirs()
    for s_dir in search_dirs:
        if not os.path.isdir(s_dir):
            continue
        keys = [f for f in glob.glob(os.path.join(s_dir, "*.json"))
                if "firebase" in os.path.basename(f).lower() or "smart-pill-dispenser" in os.path.basename(f).lower()]
        if keys:
            return keys[0]
        for f in glob.glob(os.path.join(s_dir, "*.json")):
            bname = os.path.basename(f).lower()
            if bname not in ("package.json", "tsconfig.json", "settings.json", "launch.json", "c_cpp_properties.json", "extensions.json"):
                return f
    return None


def init_firebase():
    global db
    if firebase_admin._apps:
        db = firestore.client()
        return db

    key_path = get_firebase_key_path()
    if not key_path:
        search_dirs = [d for d in get_candidate_key_dirs() if os.path.isdir(d)]
        sys.exit(
            "\n" + "=" * 70 + "\n"
            "ERROR: Firebase Service Account JSON key not found!\n"
            "=" * 70 + "\n\n"
            "The MedClip server requires a Firebase Admin SDK private key (.json)\n"
            "to connect to Cloud Firestore.\n\n"
            "To resolve this:\n"
            "1. Download your service account key from the Firebase Console:\n"
            "   Project Settings -> Service Accounts -> 'Generate new private key'\n"
            "2. Place the downloaded .json file in:\n"
            "   - 'flask_backend/' (recommended), or\n"
            "   - the repository root directory\n"
            "   (Note: *.json files are gitignored and will never be committed to Git)\n"
            "3. Or set the environment variable:\n"
            "   $env:GOOGLE_APPLICATION_CREDENTIALS = 'C:\\path\\to\\your-key.json'\n\n"
            "Checked locations:\n  - " + "\n  - ".join(search_dirs) + "\n"
            + "=" * 70 + "\n"
        )
    print(f"Connected to Firebase using credentials: {key_path}")
    cred = credentials.Certificate(key_path)
    firebase_admin.initialize_app(cred)
    db = firestore.client()
    return db


def hash_pw(password, salt):
    return hashlib.pbkdf2_hmac("sha256", password.encode(), bytes.fromhex(salt), 200_000).hex()


def init_db():
    init_firebase()
    user_ref = db.collection("users").document(DEFAULT_EMAIL.lower())
    if not user_ref.get().exists:
        salt = secrets.token_hex(16)
        user_ref.set({
            "email": DEFAULT_EMAIL.lower(),
            "name": "Patient",
            "salt": salt,
            "pw_hash": hash_pw(DEFAULT_PASSWORD, salt)
        })
    print(f"Sign in with: {DEFAULT_EMAIL} / {DEFAULT_PASSWORD}  (unless you changed it)")


def next_clip_name():
    names = {doc.to_dict().get("name") for doc in db.collection("clips").stream()}
    n = 1
    while f"Clip {n}" in names:
        n += 1
    return f"Clip {n}"


def register_clip(clip_id, name=None, seen=None):
    """Add a clip the hub reports, or update its name / last-seen time. Returns its name."""
    seen_str = (seen or now_local()).isoformat()
    doc_ref = db.collection("clips").document(clip_id)
    doc = doc_ref.get()
    name = str(name).strip()[:40] if name else None
    if doc.exists:
        data = doc.to_dict()
        assigned_name = name or data.get("name") or clip_id
        doc_ref.update({
            "last_seen": seen_str,
            "name": assigned_name
        })
        return assigned_name
    assigned_name = name or next_clip_name()
    doc_ref.set({
        "clip_id": clip_id,
        "name": assigned_name,
        "first_seen": seen_str,
        "last_seen": seen_str
    })
    return assigned_name


def next_medicine_id():
    max_id = 0
    for doc in db.collection("medicines").stream():
        d = doc.to_dict()
        mid = d.get("id")
        if isinstance(mid, int) and mid > max_id:
            max_id = mid
        elif str(doc.id).isdigit() and int(doc.id) > max_id:
            max_id = int(doc.id)
    return max_id + 1


def med_to_json(data, clip_name=None, last_signal=None):
    times = data.get("times") or []
    if isinstance(times, str):
        try:
            times = json.loads(times)
        except Exception:
            times = []

    days = data.get("days") or []
    if isinstance(days, str):
        try:
            days = json.loads(days)
        except Exception:
            days = []

    side_effects = data.get("side_effects") or []
    if isinstance(side_effects, str):
        try:
            side_effects = json.loads(side_effects)
        except Exception:
            side_effects = []

    clip_id = data.get("clip_id") or ""
    c_name = clip_name or data.get("clip_name") or clip_id

    return {
        "id": data.get("id"),
        "clipId": clip_id,
        "clipName": c_name,
        "name": data.get("name") or "",
        "strength": data.get("strength") or "",
        "form": data.get("form") or "Tablet",
        "purpose": data.get("purpose") or "",
        "color": data.get("color") or 1,
        "times": times,
        "days": days,
        "startDate": data.get("start_date") or "",
        "stock": data.get("stock", 0),
        "stockMax": data.get("stock_max", 0),
        "refillAt": data.get("refill_at", 0),
        "instructions": data.get("instructions") or "",
        "sideEffects": side_effects,
        "prescriber": data.get("prescriber") or "",
        "notes": data.get("notes") or "",
        "createdAt": data.get("created_at"),
        "lastSignal": last_signal,
    }


class ApiError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status
        self.message = message


def clean_medicine(body):
    """Validate a medicine sent by the Setup form."""
    clip = norm_id(body.get("clipId"))
    name = str(body.get("name") or "").strip()
    times = sorted({t for t in body.get("times") or [] if re.fullmatch(r"([01]\d|2[0-3]):[0-5]\d", str(t))})
    days = sorted({int(d) for d in body.get("days") or [] if str(d).isdigit() and 0 <= int(d) <= 6})
    if not clip:
        raise ApiError(400, "pick a clip")
    if not name:
        raise ApiError(400, "medicine name is required")
    if not times:
        raise ApiError(400, "set at least one dose time (HH:MM)")
    if not days:
        raise ApiError(400, "pick at least one day")

    def as_int(v):
        try:
            return max(0, int(v))
        except (TypeError, ValueError):
            return 0

    stock = as_int(body.get("stock"))
    return {
        "clip_id": clip,
        "name": name[:80],
        "strength": str(body.get("strength") or "")[:40],
        "form": str(body.get("form") or "Tablet")[:20],
        "purpose": str(body.get("purpose") or "")[:120],
        "color": as_int(body.get("color")) or 1,
        "times": times,
        "days": days,
        "stock": stock,
        "stock_max": max(as_int(body.get("stockMax")), stock, 1),
        "refill_at": as_int(body.get("refillAt")),
        "instructions": str(body.get("instructions") or "")[:500],
        "side_effects": [str(s)[:60] for s in (body.get("sideEffects") or [])][:20],
        "prescriber": str(body.get("prescriber") or "")[:80],
        "notes": str(body.get("notes") or "")[:500],
    }


def check_clip_free(clip_id, med_id=None):
    clip_doc = db.collection("clips").document(clip_id).get()
    if not clip_doc.exists:
        raise ApiError(400, "that clip isn't connected to the hub. Switch it on and wait for it to appear")
    clip_name = clip_doc.to_dict().get("name") or clip_id
    meds = db.collection("medicines").where("clip_id", "==", clip_id).stream()
    for m in meds:
        m_data = m.to_dict()
        if med_id is None or m_data.get("id") != med_id:
            raise ApiError(409, f"{clip_name} is already paired with {m_data.get('name')}")


def match_dose(med, when):
    """Find the scheduled dose a 'taken' signal at `when` belongs to (or None)."""
    times = med.get("times") or []
    if isinstance(times, str):
        times = json.loads(times)
    days = med.get("days") or []
    if isinstance(days, str):
        days = json.loads(days)
    start_str = med.get("start_date")
    start = date.fromisoformat(start_str) if start_str else when.date()

    intakes_stream = db.collection("intakes").where("med_id", "==", med["id"]).stream()
    taken = {
        d.to_dict().get("scheduled")
        for d in intakes_stream
        if d.to_dict().get("taken_at")
    }

    best = None
    for offset in (-1, 0, 1):
        day = when.date() + timedelta(days=offset)
        if day < start or (day.isoweekday() % 7) not in days:   # isoweekday: Mon=1..Sun=7 -> Sun=0
            continue
        for t in times:
            sched = datetime.combine(day, datetime.strptime(t, "%H:%M").time())
            diff = (when - sched).total_seconds() / 60
            key = f"{day.isoformat()}T{t}"
            if key in taken or diff < -MATCH_EARLY_MIN or diff > MATCH_LATE_MIN:
                continue
            if best is None or abs(diff) < abs(best[1]):
                best = (key, diff)
    return best[0] if best else None


# ----------------------------------------------------------------- HTTP handler
class Handler(SimpleHTTPRequestHandler):
    server_version = "MedClipHub/2.0-Firestore"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=FRONTEND_DIR, **kwargs)

    def log_message(self, fmt, *args):
        if self.path.startswith("/api") or self.path == "/log_pill":
            sys.stderr.write("%s  %s\n" % (self.log_date_time_string(), fmt % args))

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Hub-Key")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
        if not self.path.startswith("/api"):
            self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.end_headers()

    def do_GET(self):
        if self.path.startswith("/api/"):
            return self.api("GET")
        return super().do_GET()

    def do_POST(self):
        self.api("POST")

    def do_PUT(self):
        self.api("PUT")

    def do_DELETE(self):
        self.api("DELETE")

    # ---- helpers
    def send_json(self, status, payload):
        data = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if length > 100_000:
            raise ApiError(413, "request too large")
        if not length:
            return {}
        try:
            data = json.loads(self.rfile.read(length))
        except json.JSONDecodeError:
            raise ApiError(400, "body must be JSON")
        if not isinstance(data, dict):
            raise ApiError(400, "body must be a JSON object")
        return data

    def require_user(self):
        auth = self.headers.get("Authorization", "")
        token = auth[7:] if auth.startswith("Bearer ") else ""
        if token not in SESSIONS:
            raise ApiError(401, "sign in first")
        return SESSIONS[token]

    def require_hub(self, allow_omitted=True):
        """
        Validate X-Hub-Key header.
        - If X-Hub-Key is omitted: permitted during local/hardware testing or when allow_omitted=True.
        - If X-Hub-Key is provided: validated against HUB_KEY (rejects with 403 on mismatch).
        """
        key = self.headers.get("X-Hub-Key")
        if key is None or key.strip() == "":
            if allow_omitted or HUB_KEY == "change-me":
                return
            raise ApiError(403, "missing X-Hub-Key")

        if not hmac.compare_digest(key, HUB_KEY):
            raise ApiError(403, "wrong X-Hub-Key")

    # ---- routing
    def api(self, method):
        url = urlparse(self.path)
        path_clean = url.path.rstrip("/")
        if path_clean in ("/log_pill", "/api/log_pill") and method == "POST":
            try:
                status, payload = self.r_log_pill()
                self.send_json(status, payload)
            except ApiError as e:
                self.send_json(e.status, {"error": e.message})
            except Exception as e:
                self.log_error("error: %r", e)
                self.send_json(500, {"error": "server error, see hub log"})
            return

        parts = [unquote(p) for p in url.path.split("/")[2:] if p]   # after /api/
        query = parse_qs(url.query)
        try:
            first = parts[0] if parts else ""
            if first == "chip-events":           # old name, still accepted
                first = "clip-events"
            if first == "clips" and len(parts) > 1 and parts[1] == "announce":
                first, parts = "clips-announce", parts[1:]
            handler = {
                ("GET", "status"): self.r_status,
                ("POST", "login"): self.r_login,
                ("GET", "clips"): self.r_list_clips,
                ("DELETE", "clips"): self.r_forget_clip,
                ("POST", "clips-announce"): self.r_announce,
                ("GET", "medicines"): self.r_list_meds,
                ("POST", "medicines"): self.r_create_med,
                ("PUT", "medicines"): self.r_update_med,
                ("DELETE", "medicines"): self.r_delete_med,
                ("GET", "intakes"): self.r_list_intakes,
                ("POST", "intakes"): self.r_mark_intake,
                ("POST", "clip-events"): self.r_clip_event,
                ("POST", "log_pill"): self.r_log_pill,
            }.get((method, first))
            if not handler:
                raise ApiError(404, "unknown endpoint")
            status, payload = handler(parts[1:], query)
            self.send_json(status, payload)
        except ApiError as e:
            self.send_json(e.status, {"error": e.message})
        except Exception as e:  # keep the hub running whatever happens
            self.log_error("error: %r", e)
            self.send_json(500, {"error": "server error, see hub log"})

    # ---- endpoints for the app
    def r_status(self, _parts, _q):
        with DB_LOCK:
            last = None
            events = list(db.collection("clip_events").order_by("received_at", direction=firestore.Query.DESCENDING).limit(1).stream())
            if events:
                last = events[0].to_dict().get("received_at")
            else:
                logs = list(db.collection("pill_logs").order_by("sensor_timestamp", direction=firestore.Query.DESCENDING).limit(1).stream())
                if logs:
                    last = logs[0].to_dict().get("sensor_timestamp")

            clips_docs = list(db.collection("clips").stream())
            clips_count = len(clips_docs)
            med_clips = {m.to_dict().get("clip_id") for m in db.collection("medicines").stream()}
            free_count = sum(1 for c in clips_docs if c.to_dict().get("clip_id") not in med_clips)

        return 200, {
            "ok": True,
            "lastEvent": last,
            "clips": clips_count,
            "freeClips": free_count,
            "time": now_local().isoformat()
        }

    def r_login(self, _parts, _q):
        body = self.body()
        email = str(body.get("email") or "").strip().lower()
        password = str(body.get("password") or "")
        with DB_LOCK:
            user_doc = db.collection("users").document(email).get()
            if not user_doc.exists:
                raise ApiError(401, "wrong email or password")
            user = user_doc.to_dict()
            if not hmac.compare_digest(hash_pw(password, user["salt"]), user["pw_hash"]):
                raise ApiError(401, "wrong email or password")
        token = secrets.token_urlsafe(24)
        SESSIONS[token] = user["email"]
        return 200, {"token": token, "user": {"name": user["name"], "email": user["email"]}}

    def r_list_clips(self, _parts, _q):
        self.require_user()
        with DB_LOCK:
            clips_docs = list(db.collection("clips").stream())
            meds_docs = list(db.collection("medicines").stream())

            med_by_clip = {}
            for m in meds_docs:
                d = m.to_dict()
                cid = d.get("clip_id")
                if cid:
                    med_by_clip[cid] = d

            result = []
            for c in clips_docs:
                cd = c.to_dict()
                cid = cd.get("clip_id")
                m = med_by_clip.get(cid)
                med_id = m.get("id") if m else None
                med_name = (f"{m.get('name', '')} {m.get('strength') or ''}".strip() if m else None)

                result.append({
                    "clipId": cid,
                    "name": cd.get("name") or cid,
                    "firstSeen": cd.get("first_seen"),
                    "lastSeen": cd.get("last_seen"),
                    "medicineId": med_id,
                    "medicineName": med_name,
                })

            def sort_key(item):
                name = item.get("name") or ""
                if name.startswith("Clip "):
                    try:
                        return (0, int(name[5:]))
                    except ValueError:
                        pass
                return (1, name)

            result.sort(key=sort_key)
        return 200, result

    def r_forget_clip(self, parts, _q):
        self.require_user()
        clip = norm_id(parts[0] if parts else "")
        with DB_LOCK:
            meds = list(db.collection("medicines").where("clip_id", "==", clip).limit(1).stream())
            if meds:
                raise ApiError(409, "unpair the medicine from this clip first")
            db.collection("clips").document(clip).delete()
        return 200, {"ok": True}

    def r_list_meds(self, _parts, _q):
        self.require_user()
        with DB_LOCK:
            last = {}
            for ev in db.collection("clip_events").stream():
                d = ev.to_dict()
                mid = d.get("med_id")
                t = d.get("received_at")
                if mid is not None and t:
                    if mid not in last or t > last[mid]:
                        last[mid] = t

            clips_map = {c.id: c.to_dict().get("name") for c in db.collection("clips").stream()}
            meds = []
            for m in db.collection("medicines").stream():
                d = m.to_dict()
                if "id" not in d:
                    d["id"] = int(m.id) if m.id.isdigit() else m.id
                clip_id = d.get("clip_id")
                c_name = clips_map.get(clip_id) or clip_id
                meds.append(med_to_json(d, clip_name=c_name, last_signal=last.get(d["id"])))

            meds.sort(key=lambda x: str(x.get("id") or 0))
        return 200, meds

    def r_create_med(self, _parts, _q):
        self.require_user()
        m = clean_medicine(self.body())
        m["start_date"] = date.today().isoformat()
        m["created_at"] = now_local().isoformat()
        with DB_LOCK:
            check_clip_free(m["clip_id"])
            new_id = next_medicine_id()
            m["id"] = new_id
            db.collection("medicines").document(str(new_id)).set(m)
            clip_doc = db.collection("clips").document(m["clip_id"]).get()
            c_name = clip_doc.to_dict().get("name") if clip_doc.exists else m["clip_id"]
        return 201, med_to_json(m, clip_name=c_name)

    def r_update_med(self, parts, _q):
        self.require_user()
        if not parts or not parts[0].isdigit():
            raise ApiError(400, "medicine id missing")
        med_id = int(parts[0])
        m = clean_medicine(self.body())
        m["id"] = med_id
        with DB_LOCK:
            doc_ref = db.collection("medicines").document(str(med_id))
            doc = doc_ref.get()
            if not doc.exists:
                raise ApiError(404, "medicine not found")
            check_clip_free(m["clip_id"], med_id)
            existing = doc.to_dict()
            m["start_date"] = existing.get("start_date", date.today().isoformat())
            m["created_at"] = existing.get("created_at", now_local().isoformat())
            doc_ref.set(m)
            clip_doc = db.collection("clips").document(m["clip_id"]).get()
            c_name = clip_doc.to_dict().get("name") if clip_doc.exists else m["clip_id"]
        return 200, med_to_json(m, clip_name=c_name)

    def r_delete_med(self, parts, _q):
        """Unpair: removes the medicine and its history; the clip becomes free again."""
        self.require_user()
        if not parts or not parts[0].isdigit():
            raise ApiError(400, "medicine id missing")
        med_id = int(parts[0])
        with DB_LOCK:
            db.collection("medicines").document(str(med_id)).delete()
            for i_doc in db.collection("intakes").where("med_id", "==", med_id).stream():
                i_doc.reference.delete()
        return 200, {"ok": True}

    def r_list_intakes(self, _parts, q):
        self.require_user()
        start = (q.get("from") or ["0000-01-01"])[0]
        end = (q.get("to") or ["9999-12-31"])[0]
        with DB_LOCK:
            intakes_query = db.collection("intakes") \
                .where("scheduled", ">=", start) \
                .where("scheduled", "<=", end + "T23:59:59") \
                .stream()
            rows = [d.to_dict() for d in intakes_query]
            rows.sort(key=lambda r: r.get("scheduled") or "")
        return 200, [{
            "medId": r.get("med_id"),
            "scheduled": r.get("scheduled"),
            "takenAt": r.get("taken_at"),
            "source": r.get("source")
        } for r in rows]

    def r_mark_intake(self, _parts, _q):
        """Patient ticks (takenAt = time) or unticks (takenAt = null) a dose."""
        self.require_user()
        body = self.body()
        try:
            med_id = int(body.get("medId") or 0)
        except (TypeError, ValueError):
            raise ApiError(400, "medId must be a number")
        scheduled = str(body.get("scheduled") or "")
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}", scheduled):
            raise ApiError(400, "scheduled must look like YYYY-MM-DDTHH:MM")
        try:
            taken = to_local_naive(body.get("takenAt"))
        except ValueError:
            raise ApiError(400, "takenAt is not a valid time")

        with DB_LOCK:
            med_doc = db.collection("medicines").document(str(med_id)).get()
            if not med_doc.exists:
                raise ApiError(404, "medicine not found")
            med = med_doc.to_dict()

            intake_ref = db.collection("intakes").document(f"{med_id}_{scheduled}")
            prev = intake_ref.get()
            was_taken = bool(prev.exists and prev.to_dict().get("taken_at"))
            is_taken = bool(taken)

            intake_ref.set({
                "med_id": med_id,
                "scheduled": scheduled,
                "taken_at": taken.isoformat() if taken else None,
                "source": "manual"
            })

            current_stock = med.get("stock", 0)
            if is_taken and not was_taken:
                db.collection("medicines").document(str(med_id)).update({
                    "stock": max(current_stock - 1, 0)
                })
            elif was_taken and not is_taken:
                db.collection("medicines").document(str(med_id)).update({
                    "stock": current_stock + 1
                })
        return 200, {"ok": True}

    # ---- endpoints for the central hub & hardware
    def r_announce(self, _parts, _q):
        """The hub reports clips that are switched on and connected."""
        self.require_hub()
        body = self.body()
        items = body.get("clips") if isinstance(body.get("clips"), list) else [body]
        out = []
        with DB_LOCK:
            for item in items:
                if not isinstance(item, dict):
                    continue
                clip = norm_id(item.get("clip_id") or item.get("clipId") or item.get("chip_id"))
                if len(clip) < 2:
                    continue
                name = register_clip(clip, item.get("name"))
                paired_med = None
                meds = list(db.collection("medicines").where("clip_id", "==", clip).limit(1).stream())
                if meds:
                    paired_med = meds[0].to_dict().get("name")
                out.append({"clip_id": clip, "name": name, "paired_with": paired_med})
        if not out:
            raise ApiError(400, "clip_id missing")
        return 200, {"clips": out}

    def _process_event_ingestion(self, body, is_log_pill=False):
        """Shared logic for /api/clip-events and /log_pill storing into pill_logs & medicines."""
        device_id = str(body.get("device_id") or body.get("clip_id") or body.get("clipId") or body.get("chip_id") or "unknown")
        clip = norm_id(device_id)
        if len(clip) < 2 and device_id != "unknown":
            clip = device_id

        slot_id = body.get("slot_id")
        clip_state = body.get("clip_state")
        if not clip_state:
            clip_state = "opened"

        raw_ts = body.get("sensor_timestamp") or body.get("taken_at")
        try:
            when = to_local_naive(raw_ts) or now_local()
        except ValueError:
            raise ApiError(400, "sensor_timestamp or taken_at is not a valid time")
        sensor_timestamp = when.isoformat()

        # Telemetry document for 'pill_logs' collection in Firestore
        pill_log_data = {
            "device_id": device_id,
            "slot_id": slot_id,
            "clip_state": clip_state,
            "sensor_timestamp": sensor_timestamp,
            "server_timestamp": firestore.SERVER_TIMESTAMP,
        }

        # Return payload (excluding server_timestamp sentinel for JSON safety)
        response_payload = {
            "device_id": device_id,
            "slot_id": slot_id,
            "clip_state": clip_state,
            "sensor_timestamp": sensor_timestamp,
        }

        with DB_LOCK:
            # If the sensor reports closed, record telemetry and exit
            if str(clip_state).lower() == "closed":
                db.collection("pill_logs").add(dict(pill_log_data))
                if is_log_pill:
                    return 200, {
                        "status": "success",
                        "data": response_payload,
                        "saved_payload": response_payload
                    }
                return 200, {"status": "recorded", "message": "Clip closed event logged"}

            clip_name = register_clip(clip, None)

            # Match paired medicine by clip_id or slot_id
            med_docs = list(db.collection("medicines").where("clip_id", "==", clip).stream())
            if not med_docs and slot_id is not None:
                slot_doc = db.collection("medicines").document(str(slot_id)).get()
                if slot_doc.exists:
                    med_docs = [slot_doc]

            if not med_docs:
                db.collection("clip_events").add({
                    "clip_id": clip,
                    "received_at": sensor_timestamp,
                    "result": "not_paired"
                })
                pill_log_data["result"] = "not_paired"
                db.collection("pill_logs").add(dict(pill_log_data))

                if is_log_pill:
                    return 200, {
                        "status": "success",
                        "data": response_payload,
                        "saved_payload": response_payload,
                        "result": "not_paired",
                        "message": f"{clip_name} isn't paired with a medicine yet; pair it in Setup"
                    }
                return 202, {
                    "status": "not_paired",
                    "clip": clip_name,
                    "message": f"{clip_name} isn't paired with a medicine yet; pair it in Setup"
                }

            med = med_docs[0].to_dict()
            if "id" not in med:
                med["id"] = int(med_docs[0].id) if med_docs[0].id.isdigit() else med_docs[0].id

            scheduled = match_dose(med, when)
            if scheduled:
                db.collection("intakes").document(f"{med['id']}_{scheduled}").set({
                    "med_id": med["id"],
                    "scheduled": scheduled,
                    "taken_at": sensor_timestamp,
                    "source": "clip"
                })
                current_stock = med.get("stock", 0)
                db.collection("medicines").document(str(med["id"])).update({
                    "stock": max(current_stock - 1, 0)
                })
                result = "recorded"
            else:
                result = "no_scheduled_dose"

            db.collection("clip_events").add({
                "clip_id": clip,
                "received_at": sensor_timestamp,
                "med_id": med["id"],
                "scheduled": scheduled,
                "result": result
            })

            pill_log_data["med_id"] = med["id"]
            pill_log_data["scheduled"] = scheduled
            pill_log_data["result"] = result
            db.collection("pill_logs").add(dict(pill_log_data))

        if is_log_pill:
            return 200, {
                "status": "success",
                "data": response_payload,
                "saved_payload": response_payload,
                "result": result,
                "clip": clip_name,
                "medicine": med["name"],
                "scheduled": scheduled
            }

        if not scheduled:
            return 200, {
                "status": result,
                "clip": clip_name,
                "medicine": med["name"],
                "message": "signal saved, but no dose of this medicine is due around this time"
            }
        late_min = round((when - datetime.fromisoformat(scheduled)).total_seconds() / 60)
        return 200, {
            "status": result,
            "clip": clip_name,
            "medicine": med["name"],
            "scheduled": scheduled,
            "minutesFromSchedule": late_min
        }

    def r_clip_event(self, _parts, _q):
        """The hub reports that a clip's medicine was taken."""
        self.require_hub()
        return self._process_event_ingestion(self.body(), is_log_pill=False)

    def r_log_pill(self, _parts=None, _q=None):
        """Hardware endpoint for ESP32 clip events (backward compatible with flask_backend)."""
        return self._process_event_ingestion(self.body(), is_log_pill=True)


# ----------------------------------------------------------------- example data
def seed():
    """One clip paired with one medicine, to get started."""
    init_firebase()
    with DB_LOCK:
        name = register_clip("A1F3B2")
        med_ref = db.collection("medicines").document("1")
        if not med_ref.get().exists:
            med_ref.set({
                "id": 1,
                "clip_id": "A1F3B2",
                "name": "Metformin",
                "strength": "500 mg",
                "form": "Tablet",
                "purpose": "Blood sugar control",
                "color": 1,
                "times": ["08:00", "20:00"],
                "days": [0, 1, 2, 3, 4, 5, 6],
                "start_date": date.today().isoformat(),
                "stock": 60,
                "stock_max": 60,
                "refill_at": 10,
                "instructions": "Take with breakfast and with dinner.",
                "side_effects": [],
                "prescriber": "",
                "notes": "",
                "created_at": now_local().isoformat()
            })
    print(f"Added one example: {name} (id A1F3B2) paired with Metformin 500 mg, 08:00 and 20:00 in Firestore.")


def main():
    parser = argparse.ArgumentParser(description="MedClip central hub server (Firestore)")
    parser.add_argument("--seed", action="store_true", help="add one example clip with one medicine")
    parser.add_argument("--port", type=int, default=PORT)
    args = parser.parse_args()
    if not os.path.isdir(FRONTEND_DIR):
        sys.exit(f"Frontend folder not found: {FRONTEND_DIR}")
    init_db()
    if args.seed:
        seed()
    if HUB_KEY == "change-me":
        print("Note: the hub key is the default 'change-me'. Set MEDCLIP_HUB_KEY to change it.")
    server = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print(f"MedClip hub running with Firestore: open http://localhost:{args.port} (or this computer's IP on your network)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
