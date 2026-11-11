#!/bin/bash
# The whole Windows update, end to end, on this PC (Git Bash; WSL for the build):
#   install this version silently -> the installed app finds a newer one on a local release list (SB_UPD_GH), installs
#   it by itself (SB_UPD_AUTOINSTALL): download, SHA-256 check, close, silent setup, reopen -> the new version runs ->
#   uninstall silently, and nothing of it is left. Its data goes to a test folder, never the real one.
# Needs packaging/windows/pack.sh first (OUT=/root/wbuild/dist). Usage: tools/windows/test/update.sh
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; REPO="$(cd "$HERE/../../.." && pwd)"
V="$(tr -d '[:space:]' < "$REPO/VERSION")"; IFS=. read -r a b c <<<"$V"; NEXT="$a.$b.$((c + 1))"
APPID=tv.stremioorigin.app; T="$(cd "$REPO/.." && pwd)/windows-run/update-test"; PORT=8799
INST="$LOCALAPPDATA/Programs/Origin"; KEY='HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\tv.stremioorigin.app'
fails=0; ok() { echo "ok   $1"; }; bad() { echo "FAIL $1"; fails=$((fails + 1)); }
[ ! -e "$INST" ] || { echo "Origin is already installed in $INST: this test needs a PC without it"; exit 1; }
# the real data folder (the user's own sign-in and settings) must come out of this untouched
real_print() { [ -d "$LOCALAPPDATA/$APPID" ] && (cd "$LOCALAPPDATA/$APPID" && find . -maxdepth 1 -type f | sort | xargs -r md5sum | md5sum) || echo none; }
REAL0="$(real_print)"
rm -rf "$T"; mkdir -p "$T/srv" "$T/data"
# 1. this version's setup, and the next one (the same files, appinfo.json saying NEXT)
wsl_path() { echo "$1" | sed -E 's#^/([a-z])/#/mnt/\1/#'; }
cat > "$T/next.sh" <<EOF
set -e; P=/root/obuild/windows/windows/pkg; D=/root/wbuild/dist
rm -rf \$P/next; cp -r \$P/Origin \$P/next; sed -i 's/"version": "$V"/"version": "$NEXT"/' \$P/next/appinfo.json
makensis -V1 -DVERSION=$NEXT -DSRC=\$P/next -DICON=/root/obuild/windows/windows/build/origin.ico -DOUT=$(wsl_path "$T/srv")/${APPID}_${NEXT}_windows_setup.exe $(wsl_path "$REPO")/packaging/windows/installer.nsi
cp \$D/${APPID}_${V}_windows_setup.exe $(wsl_path "$T")/
EOF
MSYS_NO_PATHCONV=1 wsl -u root bash "$(wsl_path "$T/next.sh")"
NEWSETUP="${APPID}_${NEXT}_windows_setup.exe"; SHA="$(sha256sum "$T/srv/$NEWSETUP" | cut -c1-64)"
printf '[{"tag_name":"v%s","draft":false,"prerelease":false,"body":"Test release","assets":[{"name":"%s","digest":"sha256:%s","browser_download_url":"http://127.0.0.1:%d/%s"}]}]' \
  "$NEXT" "$NEWSETUP" "$SHA" "$PORT" "$NEWSETUP" > "$T/srv/releases.json"
python -I -m http.server "$PORT" --bind 127.0.0.1 --directory "$(cygpath -w "$T/srv")" > "$T/http.log" 2>&1 & SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT; sleep 1
# 2. install this version, silently
MSYS_NO_PATHCONV=1 "$T/${APPID}_${V}_windows_setup.exe" /S </dev/null >/dev/null 2>&1; sleep 2   # (no pipe of this script handed on: a child holding it kept the caller waiting)   # (MSYS_NO_PATHCONV: Git Bash would turn /S into a path)
[ -f "$INST/Origin.exe" ] && [ -f "$INST/libsbengine.dll" ] && [ -f "$INST/assets/icon.png" ] && [ -f "$INST/licenses/README.txt" ] && ok "installed into $INST" || bad "installed files"
grep -q "\"version\": \"$V\"" "$INST/appinfo.json" && ok "installed version $V" || bad "installed version"
reg query "$KEY" //v DisplayVersion 2>/dev/null | grep -q "$V" && ok "listed in Installed apps" || bad "uninstall entry"
[ -f "$APPDATA/Microsoft/Windows/Start Menu/Programs/Origin.lnk" ] && ok "Start menu shortcut" || bad "Start menu shortcut"
# 3. the installed app updates itself (the environment reaches the reopened app through the installer)
python -I - "$T/data/state.json" "$REPO/assets/whatsnew.json" <<'PY'
import json, sys
wn = json.load(open(sys.argv[2], encoding="utf-8")).get("version", "")
json.dump({"introSound": "false", "guideSeen": "1", "whatsNewSeen": wn}, open(sys.argv[1], "w", encoding="utf-8"))
PY
export SB_DATA="$(cygpath -m "$T/data")" SB_UPD_GH="http://127.0.0.1:$PORT/releases.json" SB_UPD_AUTOINSTALL=1 SB_SKIP_LOGIN=1 SB_TOPMOST=1 SB_WINDOW=1280x720 SB_MUTE=1 SB_NO_MPV=1
"$INST/Origin.exe" </dev/null >/dev/null 2> "$T/first.log" &
for i in $(seq 1 60); do sleep 1; grep -q "\"version\": \"$NEXT\"" "$INST/appinfo.json" 2>/dev/null && break; done
grep -q "handed to the installer" "$T/first.log" && ok "downloaded, checksum matched, installer started" || bad "update hand-over ($(grep '\[update\]' "$T/first.log" | tail -1))"
grep -q "\"version\": \"$NEXT\"" "$INST/appinfo.json" && ok "version $NEXT installed over it" || bad "new version installed"
sleep 6; RUNNING="$(powershell -NoProfile -Command "(Get-Process Origin -ErrorAction SilentlyContinue | Where-Object { \$_.Path -eq '$(cygpath -w "$INST/Origin.exe")' }).Id")"
[ -n "$RUNNING" ] && ok "Origin opened again by the installer" || bad "reopened"
grep -c "^.*\[start\] Windows" "$T/data/applog.log" | grep -q "^[2-9]" && ok "the kept log shows two starts" || bad "second start in the log"
[ -n "$RUNNING" ] && powershell -NoProfile -Command "Stop-Process -Id $RUNNING -Force" >/dev/null 2>&1; sleep 2
# 4. uninstall, silently: nothing of it left (the test data folder is the test's)
MSYS_NO_PATHCONV=1 "$INST/Uninstall.exe" /S </dev/null >/dev/null 2>&1   # (it copies itself to %TEMP% and returns at once: the copy does the work)
for i in $(seq 1 60); do sleep 1; [ -e "$INST" ] || break; done
[ ! -e "$INST" ] && ok "uninstalled: the folder is gone" || bad "folder left: $(ls "$INST" 2>/dev/null | tr '\n' ' ')"
reg query "$KEY" >/dev/null 2>&1 && bad "uninstall entry left" || ok "uninstall entry removed"
[ ! -f "$APPDATA/Microsoft/Windows/Start Menu/Programs/Origin.lnk" ] && ok "shortcut removed" || bad "shortcut left"
[ "$(real_print)" = "$REAL0" ] && ok "the real data folder untouched" || bad "the real data folder changed: $LOCALAPPDATA/$APPID"
echo "fails: $fails"; [ $fails = 0 ]
