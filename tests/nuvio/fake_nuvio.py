#!/usr/bin/env python3
"""A small stand-in for Nuvio's server, for tests/nuvio/app_test.sh. Everything sent to it goes to LOG, one JSON line each.
Control: GET /ctl?dead=1 (the third addon's manifest dies), ?dead=0, ?fail=N (the next N calls answer 500),
?expire=1 (the next user call answers 401), ?refuse=1 (refreshing the session is refused).
Avatars: Sam's avatar_id "av1" is in get_avatar_catalog, its picture at /storage/v1/object/public/avatars/animals/av1.png
(an orange disc with a yellow band, on white)."""
import json, sys, time, threading, struct, zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs
PORT, LOG = int(sys.argv[1]), sys.argv[2]
S = {"dead": 0, "fail": 0, "expire": 0, "refuse": 0, "polls": 0}
LK = threading.Lock()
BASE = [
 {"profile_index": 1, "name": "Sam", "avatar_color_hex": "#1E88E5", "uses_primary_addons": False, "is_primary": True, "avatar_id": "av1", "avatar_url": None, "profile_background_id": "bg1", "profile_background_url": "https://example.test/b.png", "uses_primary_plugins": True},
 {"profile_index": 2, "name": "Kids", "avatar_color_hex": "#E53935", "uses_primary_addons": False, "is_primary": False},
 {"profile_index": 3, "name": "Locked", "avatar_color_hex": "#43A047", "uses_primary_addons": True, "is_primary": False}]
PROFILES = list(BASE)
LIB = {1: [{"content_id": "tt0111161", "content_type": "movie", "name": "Shawshank", "poster": "", "added_at": 1700000000000}],
       2: [{"content_id": "tt0317219", "content_type": "movie", "name": "Cars", "poster": "", "added_at": 1700000000000}], 3: []}
PROG = {1: [{"content_id": "tt0468569", "content_type": "movie", "video_id": "tt0468569", "position": 600000, "duration": 9000000, "last_watched": 1795000000000, "progress_key": "tt0468569"}], 2: [], 3: []}
WAT = {1: [], 2: [{"content_id": "tt0114709", "content_type": "movie", "title": "Toy Story", "watched_at": 1795000000000}], 3: []}
def avatar_png(n=128):
    rows = b""
    for y in range(n):
        rows += bytes(1)
        for x in range(n):
            d = ((x - n / 2) ** 2 + (y - n / 2) ** 2) ** 0.5
            rows += bytes((250, 250, 248) if d > n * 0.46 else (255, 214, 0) if y > n * 0.62 else (245, 121, 0))
    ch = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return bytes.fromhex("89504e470d0a1a0a") + ch(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 2, 0, 0, 0)) + ch(b"IDAT", zlib.compress(rows)) + ch(b"IEND", b"")
AVATAR = avatar_png()
def tokens(): return {"access_token": "user-token", "refresh_token": "r1", "expires_in": 3600, "user": {"id": "owner-123", "email": "fake@example.test"}}
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def out(self, code, obj=None, raw=None):
        b = raw if raw is not None else json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def note(self, kind, body):
        with LK:
            with open(LOG, "a") as f: f.write(json.dumps({"k": kind, "b": body, "t": time.time()}) + "\n")
    def gate(self, user):
        with LK:
            if S["fail"] > 0: S["fail"] -= 1; return 500
            if user and S["expire"]: S["expire"] = 0; return 401
        return 0
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query)
        if u.path == "/ctl":
            with LK:
                for k, v in q.items(): S[k] = int(v[0])
                if "list" in q: PROFILES[:] = BASE[:int(q["list"][0])]
            return self.out(200, S)
        if u.path == "/storage/v1/object/public/avatars/animals/av1.png":
            self.note("avatar", 1); self.send_response(200); self.send_header("Content-Type", "image/png"); self.send_header("Content-Length", str(len(AVATAR))); self.end_headers(); return self.wfile.write(AVATAR)
        if u.path.endswith("/manifest.json"):
            name = u.path.split("/")[1]; self.note("manifest", name)
            if name == "dead" and S["dead"]: return self.out(500, {})
            return self.out(200, {"id": "fake." + name, "name": "Fake " + name, "version": "1.0.0", "resources": ["stream"], "types": ["movie"], "catalogs": []})
        if u.path == "/rest/v1/addons":
            g = self.gate(True)
            if g: return self.out(g, {"message": "x"})
            prof = int(q["profile_id"][0].replace("eq.", "")); self.note("addons", prof); base = "http://%s/" % self.headers.get("Host")
            names = ["good1", "good2", "dead"] if prof != 2 else ["good1"]
            return self.out(200, [{"url": base + n + "/manifest.json", "sort_order": i, "enabled": True} for i, n in enumerate(names)])
        self.out(404, {})
    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0); raw = self.rfile.read(n) if n else b"{}"
        try: b = json.loads(raw or b"{}")
        except Exception: b = {}
        p = urlparse(self.path).path
        user = self.headers.get("Authorization", "").endswith("user-token")
        if p.startswith("/rest/v1/rpc/") and user:
            g = self.gate(True)
            if g: self.note("gate", g); return self.out(g, {"message": "x"})
        if p == "/auth/v1/signup": return self.out(200, {"access_token": "anon"})
        if p == "/auth/v1/logout": self.note("logout", 1); return self.out(204, raw=b"")
        if p == "/auth/v1/token":
            if "refresh_token" in self.path:
                self.note("refresh", 1)
                if S["refuse"]: return self.out(400, {"error": "invalid_grant"})
                return self.out(200, tokens())
            if b.get("password") == "good": return self.out(200, tokens())
            return self.out(400, {"error_description": "bad"})
        if p == "/functions/v1/tv-logins-exchange": return self.out(200, tokens())
        fn = p.rsplit("/", 1)[-1]; prof = int(b.get("p_profile_id") or 1)
        if fn == "start_device_login_session": self.note("qr", 1); return self.out(200, [{"device_code": "dc", "user_code": "ABC123", "verification_uri_complete": "https://example.test/link?c=ABC123", "poll_interval_seconds": 2}])
        if fn == "poll_tv_login_session":
            with LK: S["polls"] += 1; st = "approved" if S["polls"] >= 2 else "pending"
            return self.out(200, [{"status": st, "poll_interval_seconds": 2}])
        if fn == "get_sync_owner": return self.out(200, "owner-123")
        if fn == "sync_pull_profiles": return self.out(200, PROFILES)
        if fn == "get_avatar_catalog": self.note("avatars", 1); return self.out(200, [{"id": "av1", "display_name": "Bao", "storage_path": "animals/av1.png", "category": "animals", "sort_order": 1, "is_active": True, "bg_color": "#FF7900"}])
        if fn == "sync_pull_profile_locks": return self.out(200, [{"profile_index": 3, "pin_enabled": True}])
        if fn == "verify_profile_pin": self.note("pin", b.get("p_pin")); return self.out(200, b.get("p_pin") == "1234")
        if fn == "sync_pull_library": self.note("pull_lib", prof); return self.out(200, LIB.get(prof, []) if not b.get("p_offset") else [])
        if fn == "sync_pull_watch_progress": self.note("pull_prog", prof); return self.out(200, PROG.get(prof, []))
        if fn == "sync_pull_watched_items": self.note("pull_wat", prof); return self.out(200, WAT.get(prof, []))
        if fn == "sync_push_profiles": self.note(fn, b); PROFILES[:] = b["p_profiles"]; return self.out(200, [])
        if fn.startswith("sync_push") or fn.startswith("sync_delete"): self.note(fn, b); return self.out(200, [])
        self.note("unknown", fn); self.out(404, {"message": "no " + fn})
ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
