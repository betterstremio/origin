#!/bin/bash
# One run of the staged Windows build (tools/windows/stage.sh), from Git Bash on Windows:
#   tools/windows/run.sh NAME [FRAME]   with SB_* development switches passed through the environment
# Data goes to <stage>/data-NAME, the log to <stage>/NAME.log, and with FRAME a screenshot of the app's own drawing to
# <stage>/NAME.png, after which the app quits. The startup sound is always switched off first (the user is nearby), and
# the window stays on top. FRAME counts drawn frames: at 240 Hz, 240 is one second.
set -euo pipefail
STAGE="${STAGE:-$(cd "$(dirname "$0")/../../.." && pwd)/windows-run}"
NAME="${1:?name}"; FRAME="${2:-}"
DATA="$STAGE/data-$NAME"; mkdir -p "$DATA"
python -I - "$DATA/state.json" <<'PY'
import json, os, sys
p = sys.argv[1]
j = json.load(open(p, encoding="utf-8")) if os.path.isfile(p) else {}
j["introSound"] = "false"
json.dump(j, open(p, "w", encoding="utf-8"))
PY
W="$(cygpath -m "$STAGE")"
export SB_DATA="$(cygpath -m "$DATA")" SB_TOPMOST=1 SB_WINDOW="${SB_WINDOW:-1280x720}"
if [ -n "$FRAME" ]; then export SB_SHOT="$W/$NAME.png" SB_SHOT_FRAME="$FRAME" SB_SHOT_EXIT=1; fi
cd "$STAGE"
set +e; timeout "${TIMEOUT:-90}" ./Origin.exe 2> "$NAME.log"; rc=$?; set -e
echo "exit $rc, log $STAGE/$NAME.log"
grep -E "^\[start\] ready|^\[gl\]|failed|error|\[crash\]" "$NAME.log" | cut -c1-200 | head -12 || true
