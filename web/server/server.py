#!/usr/bin/env python3
"""
MedClip central hub server.

Runs on the hub computer (laptop, Raspberry Pi, ...) with plain Python 3.9+,
no packages to install. It:
  * serves the web app in ../frontend
  * keeps the list of clips the hub has seen, and gives each one a name
  * stores medicines (each paired with one clip) and doses in SQLite
  * receives a signal each time a clip says its medicine was taken, and
    matches it to the right scheduled dose

Run:   python3 server.py            (then open http://localhost:8000)
       python3 server.py --seed     (adds one example clip with one medicine)

Messages from the central hub (see README.md):
  POST /api/clips/announce   a clip switched on / joined the Wi-Fi
       {"clip_id": "A1F3B2"}  or  {"clip_id": "A1F3B2", "name": "Clip 3"}
       or a whole list: {"clips": [{"clip_id": "A1F3B2"}, ...]}
  POST /api/clip-events      a clip says its medicine was taken
       {"clip_id": "A1F3B2"}  (+ optional "taken_at": "2026-11-19T08:05:00")
  Both need the header  X-Hub-Key: <MEDCLIP_HUB_KEY>
"""
import argparse
import hashlib
import hmac
import json
import os
import re
import secrets
import sqlite3
import sys
import threading
from contextlib import contextmanager
from datetime import date, datetime, timedelta
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

# ----------------------------------------------------------------- settings
HERE = os.path.dirname(os.path.abspath(__file__))
FRONTEND_DIR = os.path.normpath(os.path.join(HERE, "..", "frontend"))
DB_PATH = os.environ.get("MEDCLIP_DB", os.path.join(HERE, "medclip.db"))
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


# ----------------------------------------------------------------- database
SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
  email TEXT PRIMARY KEY, name TEXT NOT NULL, salt TEXT NOT NULL, pw_hash TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS clips (
  clip_id TEXT PRIMARY KEY,     -- id the hub uses for the clip
  name TEXT NOT NULL,           -- "Clip 3", given by the hub or by this server
  first_seen TEXT NOT NULL, last_seen TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS medicines (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  clip_id TEXT NOT NULL UNIQUE,
  name TEXT NOT NULL, strength TEXT, form TEXT, purpose TEXT, color INTEGER,
  times TEXT NOT NULL,          -- JSON list of "HH:MM"
  days TEXT NOT NULL,           -- JSON list of weekday numbers, 0 = Sunday
  start_date TEXT NOT NULL,     -- YYYY-MM-DD
  stock INTEGER DEFAULT 0, stock_max INTEGER DEFAULT 0, refill_at INTEGER DEFAULT 0,
  instructions TEXT, side_effects TEXT, prescriber TEXT, notes TEXT,
  created_at TEXT              -- when it was paired; earlier doses don't count
);
CREATE TABLE IF NOT EXISTS intakes (
  med_id INTEGER NOT NULL REFERENCES medicines(id) ON DELETE CASCADE,
  scheduled TEXT NOT NULL,      -- YYYY-MM-DDTHH:MM (local)
  taken_at TEXT,                -- local ISO time, NULL = marked as not taken
  source TEXT NOT NULL,         -- 'clip' or 'manual'
  PRIMARY KEY (med_id, scheduled)
);
CREATE TABLE IF NOT EXISTS clip_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  clip_id TEXT NOT NULL, received_at TEXT NOT NULL,
  med_id INTEGER, scheduled TEXT, result TEXT NOT NULL
);
"""


@contextmanager
def db():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    try:
        yield conn
        conn.commit()
    except BaseException:
        conn.rollback()
        raise
    finally:
        conn.close()


def hash_pw(password, salt):
    return hashlib.pbkdf2_hmac("sha256", password.encode(), bytes.fromhex(salt), 200_000).hex()


def init_db():
    if os.path.exists(DB_PATH):
        with db() as conn:
            cols = [r["name"] for r in conn.execute("PRAGMA table_info(medicines)")]
        if cols and "clip_id" not in cols:
            sys.exit(f"{DB_PATH} was made by the older MedBox version. Delete it and start again.")
    with DB_LOCK, db() as conn:
        conn.executescript(SCHEMA)
        if "created_at" not in [r["name"] for r in conn.execute("PRAGMA table_info(medicines)")]:
            conn.execute("ALTER TABLE medicines ADD COLUMN created_at TEXT")
        if not conn.execute("SELECT 1 FROM users").fetchone():
            salt = secrets.token_hex(16)
            conn.execute("INSERT INTO users VALUES (?,?,?,?)",
                         (DEFAULT_EMAIL, "Patient", salt, hash_pw(DEFAULT_PASSWORD, salt)))
    print(f"Sign in with: {DEFAULT_EMAIL} / {DEFAULT_PASSWORD}  (unless you changed it)")


def next_clip_name(conn):
    names = {r["name"] for r in conn.execute("SELECT name FROM clips")}
    n = 1
    while f"Clip {n}" in names:
        n += 1
    return f"Clip {n}"


def register_clip(conn, clip_id, name=None, seen=None):
    """Add a clip the hub reports, or update its name / last-seen time. Returns its name."""
    seen = (seen or now_local()).isoformat()
    row = conn.execute("SELECT name FROM clips WHERE clip_id=?", (clip_id,)).fetchone()
    name = str(name).strip()[:40] if name else None
    if row:
        conn.execute("UPDATE clips SET last_seen=?, name=COALESCE(?, name) WHERE clip_id=?", (seen, name, clip_id))
        return name or row["name"]
    name = name or next_clip_name(conn)
    conn.execute("INSERT INTO clips VALUES (?,?,?,?)", (clip_id, name, seen, seen))
    return name


def med_to_json(row, last_signal=None):
    return {
        "id": row["id"], "clipId": row["clip_id"], "clipName": row["clip_name"] or row["clip_id"],
        "name": row["name"], "strength": row["strength"] or "", "form": row["form"] or "Tablet",
        "purpose": row["purpose"] or "", "color": row["color"] or 1,
        "times": json.loads(row["times"]), "days": json.loads(row["days"]),
        "startDate": row["start_date"], "stock": row["stock"], "stockMax": row["stock_max"],
        "refillAt": row["refill_at"], "instructions": row["instructions"] or "",
        "sideEffects": json.loads(row["side_effects"] or "[]"),
        "prescriber": row["prescriber"] or "", "notes": row["notes"] or "",
        "createdAt": row["created_at"], "lastSignal": last_signal,
    }


MED_SELECT = "SELECT m.*, c.name AS clip_name FROM medicines m LEFT JOIN clips c ON c.clip_id = m.clip_id"


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
        "clip_id": clip, "name": name[:80],
        "strength": str(body.get("strength") or "")[:40], "form": str(body.get("form") or "Tablet")[:20],
        "purpose": str(body.get("purpose") or "")[:120], "color": as_int(body.get("color")) or 1,
        "times": json.dumps(times), "days": json.dumps(days),
        "stock": stock, "stock_max": max(as_int(body.get("stockMax")), stock, 1),
        "refill_at": as_int(body.get("refillAt")),
        "instructions": str(body.get("instructions") or "")[:500],
        "side_effects": json.dumps([str(s)[:60] for s in (body.get("sideEffects") or [])][:20]),
        "prescriber": str(body.get("prescriber") or "")[:80], "notes": str(body.get("notes") or "")[:500],
    }


def check_clip_free(conn, clip_id, med_id=None):
    clip = conn.execute("SELECT name FROM clips WHERE clip_id=?", (clip_id,)).fetchone()
    if not clip:
        raise ApiError(400, "that clip isn't connected to the hub. Switch it on and wait for it to appear")
    other = conn.execute("SELECT name FROM medicines WHERE clip_id=? AND id IS NOT ?", (clip_id, med_id)).fetchone()
    if other:
        raise ApiError(409, f"{clip['name']} is already paired with {other['name']}")


def match_dose(conn, med, when):
    """Find the scheduled dose a 'taken' signal at `when` belongs to (or None)."""
    times = json.loads(med["times"])
    days = json.loads(med["days"])
    start = date.fromisoformat(med["start_date"])
    taken = {r["scheduled"] for r in conn.execute(
        "SELECT scheduled FROM intakes WHERE med_id=? AND taken_at IS NOT NULL", (med["id"],))}
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
    server_version = "MedClipHub/2.0"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=FRONTEND_DIR, **kwargs)

    def log_message(self, fmt, *args):
        if self.path.startswith("/api"):
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

    def require_hub(self):
        if not hmac.compare_digest(self.headers.get("X-Hub-Key", ""), HUB_KEY):
            raise ApiError(403, "wrong or missing X-Hub-Key")

    # ---- routing
    def api(self, method):
        url = urlparse(self.path)
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
        with DB_LOCK, db() as conn:
            last = conn.execute("SELECT MAX(received_at) FROM clip_events").fetchone()[0]
            clips = conn.execute("SELECT COUNT(*) FROM clips").fetchone()[0]
            free = conn.execute(
                "SELECT COUNT(*) FROM clips WHERE clip_id NOT IN (SELECT clip_id FROM medicines)").fetchone()[0]
        return 200, {"ok": True, "lastEvent": last, "clips": clips, "freeClips": free, "time": now_local().isoformat()}

    def r_login(self, _parts, _q):
        body = self.body()
        email = str(body.get("email") or "").strip().lower()
        password = str(body.get("password") or "")
        with DB_LOCK, db() as conn:
            user = conn.execute("SELECT * FROM users WHERE lower(email)=?", (email,)).fetchone()
        if not user or not hmac.compare_digest(hash_pw(password, user["salt"]), user["pw_hash"]):
            raise ApiError(401, "wrong email or password")
        token = secrets.token_urlsafe(24)
        SESSIONS[token] = user["email"]
        return 200, {"token": token, "user": {"name": user["name"], "email": user["email"]}}

    def r_list_clips(self, _parts, _q):
        self.require_user()
        with DB_LOCK, db() as conn:
            rows = conn.execute("""
                SELECT c.*, m.id AS med_id, m.name AS med_name, m.strength AS med_strength
                FROM clips c LEFT JOIN medicines m ON m.clip_id = c.clip_id
                ORDER BY CAST(SUBSTR(c.name, 6) AS INTEGER), c.name""").fetchall()
        return 200, [{
            "clipId": r["clip_id"], "name": r["name"], "firstSeen": r["first_seen"], "lastSeen": r["last_seen"],
            "medicineId": r["med_id"], "medicineName": (f"{r['med_name']} {r['med_strength'] or ''}".strip()
                                                         if r["med_id"] else None),
        } for r in rows]

    def r_forget_clip(self, parts, _q):
        self.require_user()
        clip = norm_id(parts[0] if parts else "")
        with DB_LOCK, db() as conn:
            if conn.execute("SELECT 1 FROM medicines WHERE clip_id=?", (clip,)).fetchone():
                raise ApiError(409, "unpair the medicine from this clip first")
            conn.execute("DELETE FROM clips WHERE clip_id=?", (clip,))
        return 200, {"ok": True}

    def r_list_meds(self, _parts, _q):
        self.require_user()
        with DB_LOCK, db() as conn:
            last = {r["med_id"]: r["t"] for r in conn.execute(
                "SELECT med_id, MAX(received_at) AS t FROM clip_events WHERE med_id IS NOT NULL GROUP BY med_id")}
            rows = conn.execute(MED_SELECT + " ORDER BY m.id").fetchall()
        return 200, [med_to_json(r, last.get(r["id"])) for r in rows]

    def r_create_med(self, _parts, _q):
        self.require_user()
        m = clean_medicine(self.body())
        m["start_date"] = date.today().isoformat()
        m["created_at"] = now_local().isoformat()
        with DB_LOCK, db() as conn:
            check_clip_free(conn, m["clip_id"])
            cur = conn.execute(f"INSERT INTO medicines ({', '.join(m)}) VALUES ({', '.join('?' * len(m))})",
                               list(m.values()))
            row = conn.execute(MED_SELECT + " WHERE m.id=?", (cur.lastrowid,)).fetchone()
        return 201, med_to_json(row)

    def r_update_med(self, parts, _q):
        self.require_user()
        if not parts or not parts[0].isdigit():
            raise ApiError(400, "medicine id missing")
        med_id = int(parts[0])
        m = clean_medicine(self.body())
        with DB_LOCK, db() as conn:
            if not conn.execute("SELECT 1 FROM medicines WHERE id=?", (med_id,)).fetchone():
                raise ApiError(404, "medicine not found")
            check_clip_free(conn, m["clip_id"], med_id)
            conn.execute(f"UPDATE medicines SET {', '.join(k + '=?' for k in m)} WHERE id=?", [*m.values(), med_id])
            row = conn.execute(MED_SELECT + " WHERE m.id=?", (med_id,)).fetchone()
        return 200, med_to_json(row)

    def r_delete_med(self, parts, _q):
        """Unpair: removes the medicine and its history; the clip becomes free again."""
        self.require_user()
        if not parts or not parts[0].isdigit():
            raise ApiError(400, "medicine id missing")
        with DB_LOCK, db() as conn:
            conn.execute("DELETE FROM medicines WHERE id=?", (int(parts[0]),))
        return 200, {"ok": True}

    def r_list_intakes(self, _parts, q):
        self.require_user()
        start = (q.get("from") or ["0000-01-01"])[0]
        end = (q.get("to") or ["9999-12-31"])[0]
        with DB_LOCK, db() as conn:
            rows = conn.execute(
                "SELECT * FROM intakes WHERE substr(scheduled,1,10) BETWEEN ? AND ? ORDER BY scheduled",
                (start, end)).fetchall()
        return 200, [{"medId": r["med_id"], "scheduled": r["scheduled"], "takenAt": r["taken_at"],
                      "source": r["source"]} for r in rows]

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
        with DB_LOCK, db() as conn:
            if not conn.execute("SELECT 1 FROM medicines WHERE id=?", (med_id,)).fetchone():
                raise ApiError(404, "medicine not found")
            prev = conn.execute("SELECT taken_at FROM intakes WHERE med_id=? AND scheduled=?", (med_id, scheduled)).fetchone()
            was_taken = bool(prev and prev["taken_at"])
            conn.execute("INSERT OR REPLACE INTO intakes VALUES (?,?,?, 'manual')",
                         (med_id, scheduled, taken.isoformat() if taken else None))
            if taken and not was_taken:
                conn.execute("UPDATE medicines SET stock = MAX(stock - 1, 0) WHERE id=?", (med_id,))
            elif was_taken and not taken:
                conn.execute("UPDATE medicines SET stock = stock + 1 WHERE id=?", (med_id,))
        return 200, {"ok": True}

    # ---- endpoints for the central hub
    def r_announce(self, _parts, _q):
        """The hub reports clips that are switched on and connected."""
        self.require_hub()
        body = self.body()
        items = body.get("clips") if isinstance(body.get("clips"), list) else [body]
        out = []
        with DB_LOCK, db() as conn:
            for item in items:
                if not isinstance(item, dict):
                    continue
                clip = norm_id(item.get("clip_id") or item.get("clipId") or item.get("chip_id"))
                if len(clip) < 2:
                    continue
                name = register_clip(conn, clip, item.get("name"))
                paired = conn.execute("SELECT name FROM medicines WHERE clip_id=?", (clip,)).fetchone()
                out.append({"clip_id": clip, "name": name, "paired_with": paired["name"] if paired else None})
        if not out:
            raise ApiError(400, "clip_id missing")
        return 200, {"clips": out}

    def r_clip_event(self, _parts, _q):
        """The hub reports that a clip's medicine was taken."""
        self.require_hub()
        body = self.body()
        clip = norm_id(body.get("clip_id") or body.get("clipId") or body.get("chip_id"))
        if len(clip) < 2:
            raise ApiError(400, "clip_id missing")
        try:
            when = to_local_naive(body.get("taken_at")) or now_local()
        except ValueError:
            raise ApiError(400, "taken_at is not a valid time")
        stamp = when.isoformat()
        with DB_LOCK, db() as conn:
            clip_name = register_clip(conn, clip, None)
            med = conn.execute("SELECT * FROM medicines WHERE clip_id=?", (clip,)).fetchone()
            if not med:
                conn.execute("INSERT INTO clip_events (clip_id, received_at, result) VALUES (?,?, 'not_paired')",
                             (clip, stamp))
                return 202, {"status": "not_paired", "clip": clip_name,
                             "message": f"{clip_name} isn't paired with a medicine yet; pair it in Setup"}
            scheduled = match_dose(conn, med, when)
            if scheduled:
                conn.execute("INSERT OR REPLACE INTO intakes VALUES (?,?,?, 'clip')", (med["id"], scheduled, stamp))
                conn.execute("UPDATE medicines SET stock = MAX(stock - 1, 0) WHERE id=?", (med["id"],))
            result = "recorded" if scheduled else "no_scheduled_dose"
            conn.execute("INSERT INTO clip_events (clip_id, received_at, med_id, scheduled, result) VALUES (?,?,?,?,?)",
                         (clip, stamp, med["id"], scheduled, result))
        if not scheduled:
            return 200, {"status": result, "clip": clip_name, "medicine": med["name"],
                         "message": "signal saved, but no dose of this medicine is due around this time"}
        late_min = round((when - datetime.fromisoformat(scheduled)).total_seconds() / 60)
        return 200, {"status": result, "clip": clip_name, "medicine": med["name"], "scheduled": scheduled,
                     "minutesFromSchedule": late_min}


# ----------------------------------------------------------------- example data
def seed():
    """One clip paired with one medicine, to get started."""
    with DB_LOCK, db() as conn:
        name = register_clip(conn, "A1F3B2")
        conn.execute("""INSERT OR IGNORE INTO medicines (clip_id, name, strength, form, purpose, color, times, days,
                        start_date, stock, stock_max, refill_at, instructions, side_effects, prescriber, notes, created_at)
                        VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
                     ("A1F3B2", "Metformin", "500 mg", "Tablet", "Blood sugar control", 1, json.dumps(["08:00", "20:00"]),
                      json.dumps([0, 1, 2, 3, 4, 5, 6]), date.today().isoformat(), 60, 60, 10,
                      "Take with breakfast and with dinner.", json.dumps([]), "", "", now_local().isoformat()))
    print(f"Added one example: {name} (id A1F3B2) paired with Metformin 500 mg, 08:00 and 20:00.")


def main():
    parser = argparse.ArgumentParser(description="MedClip central hub server")
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
    print(f"MedClip hub running: open http://localhost:{args.port} (or this computer's IP on your network)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
