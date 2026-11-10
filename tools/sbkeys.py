#!/usr/bin/env python3
# sbkeys.py KEYS_FILE OUT_HEADER: the app's own keys, from a private file outside the repo (NAME=value lines), as a header
# with each value scrambled, so they are not plain text in the binary. Without the file the build still works: each
# service then needs the user's own key. The header is rewritten only when it changes, so a rebuild stays incremental.
import hashlib, os, sys

NAMES = ('TRAKT_CLIENT_ID', 'TMDB_API_KEY', 'NUVIO_SUPABASE_URL', 'NUVIO_SUPABASE_FALLBACK_URL', 'NUVIO_SUPABASE_ANON_KEY', 'DEVICE_LOGIN_WEB_BASE_URL')

src, out = sys.argv[1], sys.argv[2]
kv = {}
if os.path.isfile(src):
    for ln in open(src):
        ln = ln.strip()
        if ln and not ln.startswith('#') and '=' in ln:
            k, v = ln.split('=', 1)
            kv[k.strip()] = v.strip().strip('"\'')

def arr(name):
    v = kv.get(name, '').encode()
    pad = hashlib.sha256(b'sb:' + name.encode()).digest()
    return 'static const unsigned char SBK_%s[] = { %s0 }, SBK_%s_PAD[32] = { %s };\n#define SBK_%s_LEN %d\n' % (
        name, ''.join('%d,' % (c ^ pad[i % 32]) for i, c in enumerate(v)), name, ','.join(str(x) for x in pad), name, len(v))

body = '/* made by tools/sbkeys.py from the private keys file: never commit */\n' + ''.join(arr(n) for n in NAMES)
if not os.path.isfile(out) or open(out).read() != body:
    open(out, 'w').write(body)
print('== keys built in: %s' % (', '.join(n for n in NAMES[:3] if kv.get(n)) or 'none (%s)' % src))
