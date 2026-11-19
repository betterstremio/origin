#!/usr/bin/env python3
# pack.py ROOT BINARY OFFICIAL OUT: the webOS IPK, in the official package's exact form: its ar and tar layout and member
# order, its torrent and DTS libraries (OFFICIAL holds them, unpacked), with this checkout's binary, assets, icons and version.
import gzip, hashlib, io, json, os, shutil, sys, tarfile, tempfile, time

ROOT, BIN, O, OUT = sys.argv[1:5]
ID = os.environ.get('APPID') or 'tv.stremioorigin.app'
OLD = os.listdir(O + '/data/usr/palm/applications')[0]   # the official package's id: another APPID renames it while staging
S = tempfile.mkdtemp(prefix='ipkstage-', dir=os.path.expanduser('~'))
try:
    shutil.rmtree(S); shutil.copytree(O + '/data', S + '/data', symlinks=True)
    if ID != OLD:
        for d in ('applications', 'packages'):
            os.rename(f'{S}/data/usr/palm/{d}/{OLD}', f'{S}/data/usr/palm/{d}/{ID}')
    def renamed(n): return n.replace(f'palm/applications/{OLD}', f'palm/applications/{ID}').replace(f'palm/packages/{OLD}', f'palm/packages/{ID}')
    A = f'{S}/data/usr/palm/applications/{ID}'
    VER = open(ROOT + '/VERSION').read().strip()

    pi = f'{S}/data/usr/palm/packages/{ID}/packageinfo.json'
    pj = json.load(open(pi)); pj['version'] = VER
    if ID != OLD: pj['id'] = pj['app'] = ID
    open(pi, 'w').write(json.dumps(pj, indent=2) + '\n')

    for sub in ('i18n', 'fonts', 'sounds', 'guide'):
        shutil.rmtree(f'{A}/assets/{sub}', ignore_errors=True)
        shutil.copytree(f'{ROOT}/assets/{sub}', f'{A}/assets/{sub}')
    for f in os.listdir(ROOT + '/assets'):
        if f in ('icon.png', 'logo.png', 'splashicon.png', 'cacert.pem', 'whatsnew.json') or (f.startswith('icons') and f.endswith('.json')):
            shutil.copy(f'{ROOT}/assets/{f}', f'{A}/assets/{f}')
    shutil.rmtree(A + '/licenses')
    shutil.copytree(ROOT + '/third_party/licenses', A + '/licenses')
    for f in ('icon.png', 'largeIcon.png'):
        shutil.copy(f'{ROOT}/packaging/webos/{f}', f'{A}/{f}')
    info = open(ROOT + '/packaging/webos/appinfo.json', encoding='utf-8').read()
    if ID != 'tv.stremioorigin.app': info = info.replace('"id": "tv.stremioorigin.app"', '"id": "%s"' % ID)
    open(A + '/appinfo.json', 'w', encoding='utf-8', newline='\n').write(info.replace('"version": "0.0.0"', '"version": "%s"' % VER))
    shutil.copy(BIN, A + '/stremioorigin')

    # the P2P engine: ENGINE, else this checkout's own build (tools/build-engine.sh webos), else the official 1.0.0 one
    eng = os.environ.get('ENGINE') or f'{ROOT}/build/webos/libsbengine.so'
    if not os.path.exists(eng):
        print('warning: packing the OFFICIAL 1.0.0 engine (libsbengine.so): build it with tools/build-engine.sh webos')
        eng = None
    engs = [os.path.join(dp, f) for dp, _, fs in os.walk(S + '/data') for f in fs if f == 'libsbengine.so']
    if len(engs) != 1: sys.exit('expected one libsbengine.so in the official package, found %d' % len(engs))
    if eng: shutil.copy(eng, engs[0])
    esha = hashlib.sha256(open(engs[0], 'rb').read()).hexdigest()[:12]

    for dp, _, fs in os.walk(S + '/data'):
        for f in fs:
            if f.rsplit('.', 1)[-1] in ('json', 'pem', 'txt', 'md') and b'\r\n' in open(os.path.join(dp, f), 'rb').read():
                sys.exit('CRLF in ' + f + ': the checkout must have LF line ends (.gitattributes)')

    def tar_gz(root, order):
        buf = io.BytesIO(); now = int(time.time())
        with gzip.GzipFile(fileobj=buf, mode='wb', mtime=now) as z, tarfile.open(fileobj=z, mode='w', format=tarfile.GNU_FORMAT) as t:
            names = [n for n in order if os.path.lexists(os.path.join(root, n.rstrip('/')))]
            extra = []
            for dp, ds, fs in os.walk(root):
                for n in sorted(ds) + sorted(fs):
                    rel = os.path.relpath(os.path.join(dp, n), root)
                    extra.append(rel + '/' if os.path.isdir(os.path.join(root, rel)) else rel)
            seen = set()
            for n in names + extra:
                k = n.rstrip('/')
                if k in seen: continue
                seen.add(k); p = os.path.join(root, k); isd = os.path.isdir(p)
                ti = tarfile.TarInfo(k + '/' if isd else k); ti.uid = ti.gid = 0; ti.uname = ti.gname = 'root'; ti.mtime = now   # nothing about the build machine
                if isd:
                    ti.type = tarfile.DIRTYPE; ti.mode = 0o777; t.addfile(ti)
                else:
                    ti.size = os.path.getsize(p); ti.mode = 0o755 if k.endswith(('/stremioorigin', '.so')) else 0o644
                    with open(p, 'rb') as h: t.addfile(ti, h)
        return buf.getvalue()

    size = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(S + '/data') for f in fs)
    os.makedirs(S + '/ctl')
    ctl = open(O + '/ctl/control', encoding='utf-8').read().split('\n')
    open(S + '/ctl/control', 'w', encoding='utf-8', newline='').write('\n'.join(
        f'Installed-Size: {size}' if l.startswith('Installed-Size:') else f'Version: {VER}' if l.startswith('Version:') else f'Package: {ID}' if l.startswith('Package:') and ID != OLD else l for l in ctl))
    official_order = [renamed(m.name) for m in tarfile.open(O + '/data.tar.gz').getmembers()]
    dtar = tar_gz(S + '/data', official_order); ctar = tar_gz(S + '/ctl', ['control']); now = int(time.time())

    out = bytearray(b'!<arch>\n')
    for name, body in (('debian-binary', open(O + '/debian-binary', 'rb').read()), ('control.tar.gz', ctar), ('data.tar.gz', dtar)):
        out += (name.ljust(16) + str(now).ljust(12) + '0'.ljust(6) + '0'.ljust(6) + '100644'.ljust(8) + str(len(body)).ljust(10) + '`\n').encode() + body + (b'\n' if len(body) & 1 else b'')
    os.makedirs(OUT, exist_ok=True)
    I = f'{OUT}/{ID}_{VER}_arm.ipk'; open(I, 'wb').write(out)
    sha = hashlib.sha256(out).hexdigest(); open(I + '.sha256', 'w').write(sha + '\n')
    print(I, len(out), 'bytes, sha256', sha, 'engine', esha)

    # checked again from what was written, against the official package
    o = tarfile.open(O + '/data.tar.gz'); n = tarfile.open(fileobj=io.BytesIO(dtar))
    on = [renamed(m.name).rstrip('/') for m in o.getmembers()]; nn = [m.name.rstrip('/') for m in n.getmembers()]
    missing = [x for x in on if x not in nn]
    if missing: print('missing from the official list:', missing[:8])
finally:
    shutil.rmtree(S, ignore_errors=True)
