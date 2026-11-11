#!/bin/bash
# One run of the staged Windows build (tools/windows/stage.sh), from Git Bash on Windows:
#   tools/windows/run.sh NAME [FRAME]   with SB_* development switches passed through the environment
# Data goes to <stage>/data-NAME, the log to <stage>/NAME.log, and with FRAME a screenshot of the app's own drawing to
# <stage>/NAME.png, after which the app quits. The startup sound is always switched off first (the user is nearby), and
# the window stays on top, and videos play muted (SB_SOUND=1 for sound). FRAME counts drawn frames: at 240 Hz, 240 is one second.
set -euo pipefail
STAGE="${STAGE:-$(cd "$(dirname "$0")/../../.." && pwd)/windows-run}"
NAME="${1:?name}"; FRAME="${2:-}"
DATA="$STAGE/data-$NAME"; mkdir -p "$DATA"
python -I - "$DATA/state.json" <<'PY'
import json, os, sys
p = sys.argv[1]
j = json.load(open(p, encoding="utf-8")) if os.path.isfile(p) else {}
j["introSound"] = "false"
wn = json.load(open(os.path.join(os.path.dirname(os.path.dirname(p)), "assets", "whatsnew.json"), encoding="utf-8")).get("version", "")
if not os.environ.get("KEEP_GUIDE"): j["guideSeen"] = "1"; j["whatsNewSeen"] = wn   # scripts start on Home (KEEP_GUIDE=1: the first-start tour)
json.dump(j, open(p, "w", encoding="utf-8"))
PY
W="$(cygpath -m "$STAGE")"
export SB_DATA="$(cygpath -m "$DATA")" SB_TOPMOST=1 SB_WINDOW="${SB_WINDOW:-1280x720}"
[ -n "${SB_SOUND:-}" ] || export SB_MUTE=1   # videos silent unless SB_SOUND=1
if [ -n "$FRAME" ]; then export SB_SHOT="$W/$NAME.png" SB_SHOT_FRAME="$FRAME" SB_SHOT_EXIT=1; fi
# SCREEN=N: also the composed screen (video included) at frame N, to <stage>/NAME-screen.png
if [ -n "${SCREEN:-}" ]; then export SB_SCREEN_SHOT="$W/$NAME-screen.png" SB_SCREEN_FRAME="$SCREEN"; fi
cd "$STAGE"
if [ -n "${DETACHED:-}" ]; then
  # as Explorer starts it: no console and no stderr, so only the kept log (data/applog.log) holds the output
  : > "$DATA/applog.log"
  PID=$(powershell -NoProfile -Command "(Start-Process -PassThru -FilePath '$(cygpath -w "$STAGE/Origin.exe")' -WorkingDirectory '$(cygpath -w "$STAGE")').Id" | tr -d '')
  for i in $(seq 1 "${TIMEOUT:-90}"); do sleep 1; powershell -NoProfile -Command "if (Get-Process -Id $PID -EA SilentlyContinue) { exit 0 } else { exit 1 }" || break; done
  powershell -NoProfile -Command "Stop-Process -Id $PID -Force -EA SilentlyContinue"; rc=0
  cp "$DATA/applog.log" "$NAME.log"
else
set +e; timeout "${TIMEOUT:-90}" ./Origin.exe 2> "$NAME.log"; rc=$?; set -e
fi
echo "exit $rc, log $STAGE/$NAME.log"
grep -E "^\[start\] ready|^\[gl\]|failed|error|\[crash\]" "$NAME.log" | cut -c1-200 | head -12 || true
