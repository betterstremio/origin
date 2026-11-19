#!/usr/bin/env python3
"""The test server for tools/android/test/run.py (runs in Linux/WSL; the emulator reaches it at 10.0.2.2):
  GET /media/<file>     the test media, with Range (seeking needs it)
  GET /announce         a one-peer BitTorrent tracker: the seeder below, at the address the emulator sees
  GET /releases         a GitHub-style release list of the APKs in <media>/rel/ (the updater test)
  GET /rel/<apk>        those APKs
  a libtorrent session seeding clip.mkv on TCP 6881
Usage: serve.py <media dir> [http port] [peer address seen by the device] [seed upload bytes/s]"""
import hashlib, json, os, re, socket, struct, sys, threading, time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

MEDIA = sys.argv[1]; PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
PEER = sys.argv[3] if len(sys.argv) > 3 else '10.0.2.2'; SEED_PORT = 6881
SEED_RATE = int(sys.argv[4]) if len(sys.argv) > 4 else 0
hits = {}

def bencode(x):
    if isinstance(x, int): return b'i%de' % x
    if isinstance(x, bytes): return b'%d:%s' % (len(x), x)
    if isinstance(x, str): return bencode(x.encode())
    if isinstance(x, list): return b'l' + b''.join(bencode(i) for i in x) + b'e'
    if isinstance(x, dict): return b'd' + b''.join(bencode(k) + bencode(v) for k, v in sorted(x.items())) + b'e'

class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def send_file(self, path, ctype):
        size = os.path.getsize(path); rng = self.headers.get('Range'); start, end = 0, size - 1; code = 200
        m = re.match(r'bytes=(\d*)-(\d*)', rng or '')
        if m:
            if m.group(1): start = int(m.group(1)); end = int(m.group(2)) if m.group(2) else size - 1
            else: start = size - int(m.group(2))
            end = min(end, size - 1); code = 206
        self.send_response(code); self.send_header('Content-Type', ctype); self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Length', str(end - start + 1))
        if code == 206: self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
        self.end_headers()
        if self.command == 'HEAD': return
        with open(path, 'rb') as f:
            f.seek(start); left = end - start + 1
            try:
                while left > 0:
                    b = f.read(min(65536, left))
                    if not b: break
                    self.wfile.write(b); left -= len(b)
            except (BrokenPipeError, ConnectionResetError): pass
    def do_HEAD(self): self.do_GET()
    def do_GET(self):
        u = urlparse(self.path); p = u.path; hits[p] = hits.get(p, 0) + 1
        types = {'.mp4': 'video/mp4', '.mkv': 'video/x-matroska', '.m3u8': 'application/vnd.apple.mpegurl', '.ts': 'video/mp2t', '.apk': 'application/vnd.android.package-archive', '.srt': 'text/plain'}
        if p.startswith('/media/') or p.startswith('/rel/'):
            f = os.path.normpath(os.path.join(MEDIA, p[1:] if p.startswith('/rel/') else p[7:]))
            if not f.startswith(os.path.normpath(MEDIA)) or not os.path.isfile(f): return self.reply(404, b'no')
            return self.send_file(f, types.get(os.path.splitext(f)[1], 'application/octet-stream'))
        if p == '/announce':
            peer = parse_qs(u.query).get('peer', [PEER])[0]   # ?peer=127.0.0.1: a client on this machine
            body = bencode({'interval': 30, 'min interval': 10, 'complete': 1, 'incomplete': 0, 'peers': socket.inet_aton(peer) + struct.pack('>H', SEED_PORT)})
            return self.reply(200, body, 'text/plain')
        if p == '/releases':
            rel = []; d = os.path.join(MEDIA, 'rel'); host = self.headers.get('Host', '%s:%d' % (PEER, PORT))
            for n in sorted(os.listdir(d)) if os.path.isdir(d) else []:
                if not n.endswith('.apk') and not n.endswith('.ipk'): continue
                sha = hashlib.sha256(open(os.path.join(d, n), 'rb').read()).hexdigest()
                rel.append({'draft': False, 'prerelease': False, 'tag_name': n, 'body': '- Test update served by the test suite',
                            'assets': [{'name': n, 'digest': 'sha256:' + sha, 'browser_download_url': 'http://%s/rel/%s' % (host, n)}]})
            return self.reply(200, json.dumps(rel).encode(), 'application/json')
        if p == '/hits': return self.reply(200, json.dumps(hits).encode(), 'application/json')
        self.reply(404, b'no')
    def reply(self, code, body, ctype='text/plain'):
        self.send_response(code); self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)

def seeder():
    try: import libtorrent as lt
    except ImportError: print('no libtorrent: P2P seeding off', flush=True); return
    s = lt.session({'alert_mask': lt.alert.category_t.error_notification | lt.alert.category_t.peer_notification | lt.alert.category_t.connect_notification, 'listen_interfaces': '0.0.0.0:%d' % SEED_PORT, 'enable_dht': False, 'enable_lsd': False, 'enable_upnp': False, 'enable_natpmp': False,
                    'allow_multiple_connections_per_ip': True, 'upload_rate_limit': SEED_RATE})
    ti = lt.torrent_info(os.path.join(MEDIA, 'clip.torrent'))
    h = s.add_torrent({'ti': ti, 'save_path': MEDIA, 'flags': lt.torrent_flags.seed_mode})
    print('seeding %s on %d' % (ti.info_hash(), SEED_PORT), flush=True)
    while True:
        for a in s.pop_alerts(): print('seed alert:', a.message(), flush=True)
        st = h.status()
        if st.num_peers: print('seed: %d peers, up %d kB/s, %d MB sent' % (st.num_peers, st.upload_rate // 1024, st.total_upload >> 20), flush=True)
        time.sleep(2)

threading.Thread(target=seeder, daemon=True).start()
print('serving %s on %d (device sees %s)' % (MEDIA, PORT, PEER), flush=True)
ThreadingHTTPServer(('0.0.0.0', PORT), H).serve_forever()
