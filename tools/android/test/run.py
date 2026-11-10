#!/usr/bin/env python3
"""The Android build's automated tests: builds the dev APK, starts the emulator and the test server if needed, and
runs every check against the real app, reading its log (logcat) and the screen (screencap).

  python tools/android/test/run.py              all tests on the running emulator (or the one it starts)
  python tools/android/test/run.py --build      build first (WSL: tools/android/build-apk.sh --dev, x86)
  python tools/android/test/run.py boot p2p     only some tests (names below)
  python tools/android/test/run.py --serial <id> a phone over USB instead (adb devices); tests that need the
                                                emulator's address for the test server (10.0.2.2) use adb reverse there

Needs: Windows with the Android SDK (emulator, adb), WSL with the build tools (tools/android/*.sh) and
tools/android/test/media.sh run once. The signed-in account test reads a session saved by --save-signin
(kept outside the repo, in --work). Results: <work>/report.json and screenshots in <work>/shots."""
import argparse, json, os, shutil, struct, subprocess, sys, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__)); REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
SDK = os.environ.get('ANDROID_HOME') or os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk')
ADB = os.path.join(SDK, 'platform-tools', 'adb.exe' if os.name == 'nt' else 'adb')
EMU = os.path.join(SDK, 'emulator', 'emulator.exe' if os.name == 'nt' else 'emulator')
PKG = 'tv.stremioorigin.app'; ACT = PKG + '/.MainActivity'
AVD = os.environ.get('SB_AVD', 'sbtv')
WSL_REPO = '/mnt/' + REPO[0].lower() + REPO[2:].replace('\\', '/') if os.name == 'nt' else REPO
APKS = ('/root/obuild/%s/android/apk' % os.path.basename(REPO)) if os.name == 'nt' else REPO + '/build/android/apk'   # tools/paths.sh's BUILD_ROOT
HOST = '10.0.2.2'; PORT = 8765

ap = argparse.ArgumentParser(); ap.add_argument('tests', nargs='*'); ap.add_argument('--build', action='store_true')
ap.add_argument('--serial'); ap.add_argument('--work', default=os.path.join(os.path.dirname(REPO), 'android-tools'))
ap.add_argument('--save-signin', action='store_true', help='keep the session now on the device for the account tests')
ap.add_argument('--abis', default='x86')
A = ap.parse_args()
try: sys.stdout.reconfigure(encoding='utf-8')
except Exception: pass
WORK = A.work; SHOTS = os.path.join(WORK, 'shots', 'test'); os.makedirs(SHOTS, exist_ok=True)
SIGNIN = os.path.join(WORK, '.signin', 'state.json')

def adb(*a, check=False, timeout=120, binary=False):
    cmd = [ADB] + (['-s', A.serial] if A.serial else []) + list(a)
    r = subprocess.run(cmd, capture_output=True, timeout=timeout)
    if check and r.returncode: raise RuntimeError('adb %s: %s' % (' '.join(a), r.stderr.decode(errors='replace')))
    return r.stdout if binary else r.stdout.decode(errors='replace')
def sh(c, **k): return adb('shell', c, **k)
def wsl(c, timeout=900):
    r = subprocess.run(['wsl', '-d', 'Ubuntu', '-u', 'root', '--', 'bash', '-c', c], capture_output=True, timeout=timeout, env=dict(os.environ, MSYS_NO_PATHCONV='1'))
    return r.returncode, r.stdout.decode(errors='replace') + r.stderr.decode(errors='replace')
def wait(sec): time.sleep(sec)

# ---- the device ----
def emulator_up():
    if A.serial: return
    if 'device' in adb('get-state'): return
    print('starting the emulator (%s)' % AVD)
    # with its window (minimized by hand if wanted): started with no window at all, the emulator never came up on the host GPU
    # software GL (SwiftShader): the host-GPU renderer crashed on the letterbox test's display resize, and a crash
    # leaves a consent dialog that holds every later start, so old crash reports go first
    tmp = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Temp', 'AndroidEmulator')
    for f in (os.listdir(tmp) if os.path.isdir(tmp) else []):
        if f.startswith('emu-crash'): shutil.rmtree(os.path.join(tmp, f), ignore_errors=True)
    subprocess.Popen([EMU, '-avd', AVD, '-no-snapshot', '-no-boot-anim', '-gpu', 'swiftshader_indirect'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    adb('wait-for-device', timeout=300)
    for _ in range(150):
        if sh('getprop sys.boot_completed').strip() == '1': return
        wait(2)
    raise RuntimeError('the emulator did not boot')
def quiet_device():   # the emulator only: Google TV's own "Set up Google TV" prompt comes up over the app and pauses it
    if A.serial: return
    for c in ('settings put secure user_setup_complete 1', 'settings put global device_provisioned 1', 'settings put secure tv_user_setup_complete 1',
              'pm disable-user --user 0 com.google.android.tungsten.setupwraith', 'input keyevent KEYCODE_BACK'): sh(c)
def server_up():
    try:
        urllib.request.urlopen('http://127.0.0.1:%d/hits' % PORT, timeout=3).read(); return
    except Exception: pass
    print('starting the test server (WSL)')
    wsl('fuser -k %d/tcp 6881/tcp >/dev/null 2>&1; mkdir -p /root/abuild/run; sed "s/\\r$//" %s/tools/android/test/serve.py > /root/abuild/run/serve.py' % (PORT, WSL_REPO))
    log = open(os.path.join(WORK, 'serve.log'), 'w')
    subprocess.Popen(['wsl', '-d', 'Ubuntu', '-u', 'root', '--', 'bash', '-c', 'cd /root/abuild && exec python3 -u run/serve.py /root/abuild/testmedia %d %s' % (PORT, HOST)],
                     stdout=log, stderr=subprocess.STDOUT, creationflags=0x08000000 if os.name == 'nt' else 0)
    for _ in range(30):
        try: urllib.request.urlopen('http://127.0.0.1:%d/hits' % PORT, timeout=2).read(); return
        except Exception: wait(1)
    raise RuntimeError('the test server did not start (see %s)' % log.name)
def device_reaches_host():
    if A.serial:   # a phone: the test server through adb reverse, at 127.0.0.1 on the phone
        global HOST; HOST = '127.0.0.1'; adb('reverse', 'tcp:%d' % PORT, 'tcp:%d' % PORT); adb('reverse', 'tcp:6881', 'tcp:6881')

def build():
    print('building the dev APK (%s)' % A.abis)
    rc, out = wsl('ABIS="%s" ROOT=%s bash %s/tools/android/build-apk.sh --dev 2>&1 | tail -5' % (A.abis, WSL_REPO, WSL_REPO), timeout=1800)
    if 'APK_OK' not in out: print(out); raise RuntimeError('build failed')
def apk_path(ver=None):
    ver = ver or open(os.path.join(REPO, 'VERSION')).read().strip()
    out = os.path.join(WORK, 'out'); os.makedirs(out, exist_ok=True)
    name = '%s_%s_android_dev.apk' % (PKG, ver)
    wsl('cp %s/%s %s/' % (APKS, name, '/mnt/' + out[0].lower() + out[2:].replace('\\', '/')))
    return os.path.join(out, name)

# ---- the app ----
def launch(env=None, clear_log=True):
    sh('am force-stop ' + PKG)
    if clear_log: adb('logcat', '-c')
    args = ' '.join("--es %s '%s'" % (k, v) for k, v in (env or {}).items())
    sh('am start -n %s %s' % (ACT, args))
def log():
    return '\n'.join(l for l in adb('logcat', '-d', '-v', 'brief', timeout=60).splitlines() if 'StremioOrigin' in l or 'AndroidRuntime' in l or 'DEBUG' in l or 'libc' in l)
def wait_log(pattern, sec):
    t0 = time.time()
    while time.time() - t0 < sec:
        L = log()
        if pattern in L: return L
        wait(1)
    return None
def crashed(L):
    for bad in ('FATAL EXCEPTION', 'Fatal signal', '[crash]', 'backtrace:'):
        if bad in L: return bad
    return None
def screen(name):
    raw = adb('exec-out', 'screencap', binary=True, timeout=60)
    png = adb('exec-out', 'screencap', '-p', binary=True, timeout=60); open(os.path.join(SHOTS, name + '.png'), 'wb').write(png)
    w, h = struct.unpack('<II', raw[:8]); hdr = len(raw) - w * h * 4
    return w, h, raw[hdr:]
def px(img, x, y):
    w, h, d = img; i = (y * w + x) * 4; return d[i], d[i + 1], d[i + 2]
def variety(img, step=40):
    w, h, d = img; seen = set()
    for y in range(0, h, step):
        for x in range(0, w, step): r, g, b = px(img, x, y); seen.add((r >> 4, g >> 4, b >> 4))
    return len(seen)
def fps(L):
    v = [int(l.split(' drawn, ')[1].split(' fps')[0]) for l in L.splitlines() if ' drawn, ' in l and ' fps' in l]
    return v[-1] if v else 0

def restore_signin():
    if not os.path.isfile(SIGNIN): return False
    sh('am force-stop ' + PKG); adb('push', SIGNIN, '/data/local/tmp/sb-state.json')
    sh("run-as %s sh -c 'mkdir -p files/data && cat /data/local/tmp/sb-state.json > files/data/state.json'" % PKG); sh('rm /data/local/tmp/sb-state.json')
    return True
def signed_out():
    sh('am force-stop ' + PKG); sh("run-as %s rm -f files/data/state.json" % PKG)

# ---- the tests: each returns (ok, detail) ----
R = []
def test(name):
    def deco(f): f.tname = name; R.append(f); return f
    return deco
SKIP_INTRO = {'SB_PLAY_ID': 'tt0000001'}   # an id no skip-intro source knows: the clip plays from the start

@test('boot')
def t_boot():
    signed_out(); launch()
    L = wait_log('[loop] start', 30)
    if not L: return False, 'no "[loop] start" in 30 s'
    wait(8); L = log(); img = screen('boot-signin')
    if 'signed in: 0' not in L: return False, 'expected the sign-in screen'
    v = variety(img); f = fps(L)
    if v < 8: return False, 'the screen looks blank (%d colours)' % v
    if f < 40: return False, 'only %d fps' % f
    return True, '%d fps, %d colours, sign-in code from Stremio: %s' % (f, v, 'yes' if '[login]' in L or 'link' in L.lower() or v > 20 else 'unknown')

@test('keys')
def t_keys():
    signed_out(); launch({'SB_EVLOG': '1'}); wait(6)
    a = screen('keys-before')
    for k in ('KEYCODE_DPAD_DOWN', 'KEYCODE_DPAD_DOWN', 'KEYCODE_DPAD_RIGHT'): sh('input keyevent ' + k); wait(0.5)
    wait(1.5); b = screen('keys-after'); L = log()
    diff = sum(1 for y in range(0, a[1], 30) for x in range(0, a[0], 30) if px(a, x, y) != px(b, x, y))
    evs = L.count('[ev] type=768')   # SDL_KEYDOWN
    sh('input keyevent KEYCODE_BACK'); wait(2); alive = sh('pidof ' + PKG).strip() != ''
    sh('input keyevent KEYCODE_PROG_RED'); wait(1); L = log()
    if evs < 3: return False, 'the app saw %d key presses of 3' % evs
    if diff < 5: return False, 'the highlight did not move'
    if not alive: return False, 'Back closed the app (the app should take it)'
    return True, '%d key presses reached the app, the highlight moved (%d points changed), Back stays in the app' % (evs, diff)

@test('nuvio-signin')
def t_nuvio():   # the Stremio | Nuvio switch and the profiles, against tests/nuvio/fake_nuvio.py on this PC
    import subprocess, tempfile
    log_f = os.path.join(tempfile.gettempdir(), 'fake_nuvio.log'); open(log_f, 'w').close()
    srv = subprocess.Popen([sys.executable, '-I', os.path.join(REPO, 'tests', 'nuvio', 'fake_nuvio.py'), '18765', log_f])
    try:
        wait(1); signed_out(); u = 'http://%s:18765' % HOST; launch({'SB_NV_URL': u, 'SB_NV_URL2': u, 'SB_NV_KEY': 'k'}); wait(8)
        a = screen('nuvio-stremio-side'); sx = lambda v, s: int(v * s)
        at = (sx(0.59, a[0]), sx(0.2, a[1])); before = px(a, *at)
        sh('input keyevent KEYCODE_DPAD_UP'); wait(0.7); sh('input keyevent KEYCODE_DPAD_RIGHT'); wait(1.5)
        b = screen('nuvio-nuvio-side'); after = px(b, *at)
        if not (sum(after[:3]) > 500 and sum(before[:3]) < 400): return False, 'the switch did not move (%s to %s)' % (before, after)
        wait(9); c = screen('nuvio-profiles')   # the fake phone approves on the second check
        sh('input keyevent KEYCODE_DPAD_RIGHT'); wait(0.7); sh('input keyevent KEYCODE_DPAD_CENTER'); wait(9)
        st = sh("run-as %s cat files/data/state.json" % PKG); L = log()
        if crashed(L): return False, 'crashed'
        if '"nvProfile":"2"' not in st: return False, 'the Kids profile was not chosen with the remote'
        if '[nuvio] addons: 0 -> 1' not in L: return False, 'its addons did not come down'
        return True, 'the switch moved, the code was approved, Kids chosen with the remote, addons and library came down'
    finally:
        srv.kill(); signed_out()

@test('home')
def t_home():
    if not restore_signin(): return None, 'no saved session (run with --save-signin once after signing in)'
    launch(); L = wait_log('signed in: 1', 30)
    if not L: return False, 'the saved session was not taken'
    wait(20); L = log(); img = screen('home-account')
    pics = [l for l in L.splitlines() if 'MB of pictures' in l]
    mb = int(pics[-1].split(' fps, ')[1].split(' MB')[0]) if pics else 0
    if mb < 5: return False, 'only %d MB of posters loaded' % mb
    return True, 'signed in, %d MB of posters, %d fps' % (mb, fps(L))

def play_test(name, url, extra=None, shot=None, need_tracks=None, wait_s=25):
    env = dict(SKIP_INTRO); env['SB_PLAY'] = url; env.update(extra or {}); signed_out(); launch(env)   # signed out: the test plays leave nothing in the account's Continue Watching
    L = wait_log('[video] state playing', wait_s + 20)
    if not L: L = log(); return False, 'never played: ' + ' | '.join(l.split(': ', 1)[-1] for l in L.splitlines() if '[player]' in l or 'error' in l.lower())[-300:]
    wait(6); L = log(); img = screen(shot or name)
    st = [l for l in L.splitlines() if '[video] state' in l]
    if 'picture running' not in L: return False, 'no picture ("picture running" missing)'
    # the test clip's colour bars: red on the far left, blue in the middle-right (testsrc2), seen through the app's surface
    w, h, _ = img; pts = [px(img, int(w * fx), int(h * fy)) for fx in (0.03, 0.08, 0.13) for fy in (0.3, 0.4, 0.5, 0.6)]   # inside the red bar
    red = sum(1 for r, g, b in pts if r > 150 and g < 100 and b < 100)
    if red < len(pts) // 2: return False, 'the picture is not on screen (%d of %d points red where the red bar is: %s)' % (red, len(pts), pts[:3])
    tr = [l for l in L.splitlines() if '[video] tracks:' in l]
    if need_tracks and not any(need_tracks in l for l in tr): return False, 'tracks: %s (wanted %s)' % (tr[-1].split('tracks: ')[-1] if tr else 'none', need_tracks)
    return True, '%s; %s' % (st[-1].split('[video] ')[-1], tr[-1].split('[video] ')[-1] if tr else '')

@test('play-mp4')
def t_mp4(): return play_test('play-mp4', 'http://%s:%d/media/clip.mp4' % (HOST, PORT), need_tracks='1 audio')
@test('play-mkv')
def t_mkv():
    ok, d = play_test('play-mkv', 'http://%s:%d/media/clip.mkv' % (HOST, PORT), need_tracks='2 audio, 1 text')
    if ok and 'Subtitle line' not in log(): return False, d + '; the subtitles never showed'
    return ok, d + ('; subtitles drawn' if ok else '')
@test('play-hls')
def t_hls(): return play_test('play-hls', 'http://%s:%d/media/hls/master.m3u8' % (HOST, PORT))

@test('audio-fallback')
def t_audio():   # a sound the device can't decode: the picture plays on without it (the emulator has no E-AC-3 decoder)
    caps = log(); launch_caps = [l for l in caps.splitlines() if 'device caps' in l]
    ok, d = play_test('audio-fallback', 'http://%s:%d/media/clip-eac3.mkv' % (HOST, PORT))
    L = log()
    if 'eac3=1' in L: return None, 'this device decodes E-AC-3: nothing to fall back from'
    if not ok: return False, d
    # either ExoPlayer leaves the track out (no decoder listed) or its decoder fails and the bridge turns the sound off:
    # both end in the app's own "can't play this sound" notice with the picture on
    how = 'decoder failed, sound switched off' if "the sound can't be played here" in L else 'track left out'
    if "can’t play this video’s sound" not in L and "can't play this video's sound" not in L: return False, 'played, but the app was never told the sound is missing: ' + d
    return True, 'E-AC-3 only, no decoder (%s): the picture plays on and the app says so; %s' % (how, d)

@test('seek-pause')
def t_seek():
    ok, d = play_test('seek-before', 'http://%s:%d/media/clip.mp4' % (HOST, PORT))
    if not ok: return ok, d
    sh('input keyevent KEYCODE_MEDIA_PLAY_PAUSE'); wait(2); L = log()
    paused = 'state paused' in L
    sh('input keyevent KEYCODE_MEDIA_PLAY_PAUSE'); wait(2)
    sh('input keyevent KEYCODE_MEDIA_FAST_FORWARD'); wait(1); sh('input keyevent KEYCODE_DPAD_CENTER'); wait(4); L = log()
    seeks = [l for l in L.splitlines() if '[video] seek to' in l]
    screen('seek-after')
    if not paused: return False, 'Play/Pause did not pause'
    if not seeks: return False, 'no seek reached the player'
    return True, 'paused and resumed; %s' % seeks[-1].split('[video] ')[-1]

@test('p2p')
def t_p2p():
    ih = wsl('cat /root/abuild/testmedia/clip.ih')[1].strip()
    if len(ih) != 40: return None, 'no test torrent (run tools/android/test/media.sh)'
    sh("run-as %s rm -rf files/data/engine files/data/cache/engine-state" % PKG)
    ok, d = play_test('p2p', 'p2p://%s/0?tr=http%%3A%%2F%%2F%s%%3A%d%%2Fannounce' % (ih, HOST, PORT), wait_s=40)
    L = log()
    eng = [l for l in L.splitlines() if '[p2p] engine ' in l]
    ready = [l for l in L.splitlines() if 'stream ready' in l]
    if not eng: return False, 'the engine library did not load'
    if not ok: return False, d
    return True, '%s; %s; %s' % (eng[0].split('[p2p] ')[-1], ready[0].split('[p2p] ')[-1].split(':')[0] if ready else '', d)

@test('lifecycle')
def t_life():
    signed_out(); launch(); wait_log('[loop] start', 30); wait(5)
    sh('input keyevent KEYCODE_HOME'); wait(3); sh('am start -n ' + ACT); wait(5); L = log()
    c = crashed(L)
    if c: return False, 'crash: ' + c
    if '[life] to the back' not in L or '[life] back in front' not in L: return False, 'the app did not see itself go to the back and return'
    img = screen('lifecycle-back');
    if variety(img) < 8: return False, 'blank screen after coming back'
    return True, 'to the back and back in front, drawing again'

@test('letterbox')
def t_letterbox():
    sh('wm size 2400x1080'); wait(2)
    try:
        signed_out(); launch(); wait_log('[loop] start', 30); wait(6); img = screen('letterbox-phone')
    finally: sh('wm size reset'); wait(2)
    w, h, _ = img; bar = (w - h * 16 // 9) // 2
    if bar < 20: return False, 'the screen did not get wider (%dx%d)' % (w, h)
    edge = [px(img, x, int(h * fy)) for x in (3, bar - 6, w - bar + 6, w - 4) for fy in (0.1, 0.5, 0.9)]
    inner = [px(img, x, int(h * fy)) for x in (bar + 20, w - bar - 20) for fy in (0.1, 0.5, 0.9)]
    if any(max(c) > 20 for c in edge): return False, 'expected black bars %d px wide on a %dx%d screen, found %s' % (bar, w, h, edge[:2])
    if all(max(c) <= 20 for c in inner): return False, 'the 16:9 picture is black too'
    return True, '%dx%d screen (2400x1080 scaled): 16:9 picture between %d px black bars' % (w, h, bar)

@test('update')
def t_update():
    base = open(os.path.join(REPO, 'VERSION')).read().strip()
    p = [int(x) for x in base.split('.')]; newer = '%d.%d.%d' % (p[0], p[1], p[2] + 1)
    kind = {'arm64-v8a': 'arm64', 'armeabi-v7a': 'armv7'}.get(A.abis.split()[0], A.abis.split()[0])   # the file this build's updater looks for
    rc, out = wsl('ls /root/abuild/testmedia/rel/%s_%s_android-%s.apk' % (PKG, newer, kind))
    if rc:
        print('  building %s for the update test' % newer)
        rc, out = wsl('''SB_VERSION=%s ABIS="%s" ROOT=%s bash %s/tools/android/build-apk.sh --dev 2>&1 | tail -2 && rm -rf /root/abuild/testmedia/rel && mkdir -p /root/abuild/testmedia/rel && cp %s/%s_%s_android_dev.apk /root/abuild/testmedia/rel/%s_%s_android-%s.apk''' % (newer, A.abis, WSL_REPO, WSL_REPO, APKS, PKG, newer, PKG, newer, kind), timeout=1800)
        if 'APK_OK' not in out: return False, 'could not build %s: %s' % (newer, out[-300:])
    adb('install', '-r', '-d', apk_path(base), timeout=180)
    if ('versionName=' + base) not in sh('dumpsys package ' + PKG): return False, 'could not put %s back' % base
    sh('appops set %s REQUEST_INSTALL_PACKAGES allow' % PKG)
    launch({'SB_UPD_GH': 'http://%s:%d/releases' % (HOST, PORT), 'SB_UPD_AUTOINSTALL': '1'})
    t0 = time.time(); ver = base
    while time.time() - t0 < 90:
        d = sh('dumpsys package ' + PKG)
        if ('versionName=' + newer) in d: ver = newer; break
        if 'confirm' in log().lower():   # the system asks: press its Install/Update button
            ui = sh('uiautomator dump /sdcard/ui.xml >/dev/null && cat /sdcard/ui.xml')
            import re
            m = re.search(r'text="(Update|Install|UPDATE|INSTALL)"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', ui)
            if m: x = (int(m.group(2)) + int(m.group(4))) // 2; y = (int(m.group(3)) + int(m.group(5))) // 2; sh('input tap %d %d' % (x, y))
        wait(2)
    L = log()
    steps = [l.split('[update] ')[-1] for l in L.splitlines() if '[update]' in l and 'Downloading' not in l]
    adb('install', '-r', '-d', apk_path(base), timeout=180)   # back to the version under test
    if ver != newer: return False, 'still %s after 90 s: %s' % (base, ' | '.join(steps)[-400:])
    return True, '%s -> %s through the app: %s' % (base, newer, ' | '.join(s for s in steps if 'checksum' in s or 'available' in s or 'Install' in s))

@test('no-crash')
def t_nocrash():
    L = adb('logcat', '-d', '-b', 'crash', timeout=60)
    bad = [l for l in L.splitlines() if PKG in l or 'libmain' in l or 'libsbengine' in l]
    return (not bad), ('no crash in the crash log' if not bad else bad[0][:200])

# ---- run ----
def main():
    emulator_up(); quiet_device(); device_reaches_host(); server_up()
    if A.build: build(); wsl('rm -rf /root/abuild/testmedia/rel')   # the update test's newer APK is made again from this build
    if A.save_signin:
        os.makedirs(os.path.dirname(SIGNIN), exist_ok=True)
        data = adb('exec-out', 'run-as', PKG, 'cat', 'files/data/state.json', binary=True)
        if b'authKey' not in data: sys.exit('no signed-in session on the device')
        open(SIGNIN, 'wb').write(data); print('session saved in', SIGNIN); return
    apk = apk_path(); print('installing', os.path.basename(apk))
    out = adb('install', '-r', '-d', apk, timeout=180) + ''
    if 'Success' not in out:   # signed with another key (a new release key): a clean install, the session put back after
        adb('uninstall', PKG, timeout=60); adb('install', apk, check=True, timeout=180); restore_signin()
    adb('logcat', '-b', 'crash', '-c')
    want = A.tests or [f.tname for f in R]; results = []; t00 = time.time()
    for f in R:
        if f.tname not in want: continue
        t0 = time.time(); print('%-11s ' % f.tname, end='', flush=True)
        try: ok, detail = f()
        except Exception as e: ok, detail = False, 'error: %r' % e
        L = log() if ok else ''
        if ok and crashed(L): ok, detail = False, 'crash: ' + crashed(L)
        mark = 'SKIP' if ok is None else 'PASS' if ok else 'FAIL'
        print('%s  %4.0fs  %s' % (mark, time.time() - t0, detail))
        results.append({'test': f.tname, 'result': mark, 'seconds': round(time.time() - t0), 'detail': detail})
        if not ok and ok is not None: open(os.path.join(SHOTS, f.tname + '.log'), 'w', encoding='utf-8').write(log())
    sh('am force-stop ' + PKG); restore_signin()   # the device is left signed in, as it was
    n = {k: sum(1 for r in results if r['result'] == k) for k in ('PASS', 'FAIL', 'SKIP')}
    dev = sh('getprop ro.product.model').strip() + ', Android ' + sh('getprop ro.build.version.release').strip() + ', ' + sh('getprop ro.product.cpu.abi').strip()
    json.dump({'device': dev, 'when': time.strftime('%Y-%m-%d %H:%M'), 'results': results, 'summary': n}, open(os.path.join(WORK, 'report.json'), 'w'), indent=1)
    print('\n%d passed, %d failed, %d skipped in %.0f s on %s. Screenshots: %s' % (n['PASS'], n['FAIL'], n['SKIP'], time.time() - t00, dev, SHOTS))
    sys.exit(1 if n['FAIL'] else 0)

main()
