"""A file server that behaves like a debrid link that drops: byte ranges as the real ones answer them, but every
connection is cut (no goodbye) after DROP_MB megabytes. Development only.
    python -I flaky_server.py FILE PORT DROP_MB        (serves FILE at http://127.0.0.1:PORT/clip.mkv)"""
import os, re, socket, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PATH, PORT, DROP = sys.argv[1], int(sys.argv[2]), int(float(sys.argv[3]) * 1024 * 1024)
SIZE = os.path.getsize(PATH)
lock = threading.Lock(); stats = {"connections": 0, "cut": 0}

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def do_HEAD(self): self.answer(body=False)
    def do_GET(self): self.answer(body=True)
    def answer(self, body):
        start, end = 0, SIZE - 1
        m = re.match(r"bytes=(\d*)-(\d*)", self.headers.get("Range", ""))
        if m and m.group(1): start = int(m.group(1)); end = int(m.group(2)) if m.group(2) else SIZE - 1
        if start >= SIZE: self.send_response(416); self.send_header("Content-Range", f"bytes */{SIZE}"); self.end_headers(); return
        n = end - start + 1
        self.send_response(206 if m else 200); self.send_header("Accept-Ranges", "bytes"); self.send_header("Content-Length", str(n))
        self.send_header("Content-Type", "video/x-matroska")
        if m: self.send_header("Content-Range", f"bytes {start}-{end}/{SIZE}")
        self.end_headers()
        if not body: return
        with lock: stats["connections"] += 1
        sent = 0
        with open(PATH, "rb") as f:
            f.seek(start)
            while sent < n:
                if sent >= DROP:  # the link drops: no more bytes, the socket reset
                    with lock: stats["cut"] += 1
                    print(f"cut after {sent >> 20} MB (from byte {start})", flush=True)
                    self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
                    self.connection.close(); return
                b = f.read(min(262144, n - sent, DROP - sent))
                if not b: break
                try: self.wfile.write(b)
                except OSError: return
                sent += len(b)

print(f"serving {SIZE >> 20} MB, cut every {DROP >> 20} MB", flush=True)
ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
