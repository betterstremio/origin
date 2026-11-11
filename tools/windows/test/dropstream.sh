#!/bin/bash
# A stream whose connection keeps dropping (a debrid link cutting out): the clip from flaky_server.py, cut every DROP_MB.
# Playback must carry on past each cut to SECONDS, the player never showing an error.
#   tools/windows/test/dropstream.sh CLIP [DROP_MB] [SECONDS]   (Git Bash; build staged first)
set -eo pipefail
CLIP=$(cygpath -w "$1"); DROP=${2:-40}; SECS=${3:-20}; PORT=8798
T=$(cd "$(dirname "$0")" && pwd); STAGE=$(cd "$T/../../../.." && pwd)/windows-run
python -I "$(cygpath -w "$T/flaky_server.py")" "$CLIP" $PORT "$DROP" > "$STAGE/drop-server.log" 2>&1 & SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT; sleep 1
rm -rf "$STAGE/data-drop"
SB_SKIP_LOGIN=1 SB_PLAY_NOSAVE=1 SB_PLAY="${PREFIX:-}http://127.0.0.1:$PORT/clip.mkv" SB_PLAY_ID=tt22022452 SB_PLAY_TITLE="Inside Out 2" SB_PAUSE_AT=$SECS \
  SB_MPV_LOG="$(cygpath -m "$STAGE/mpv-drop.log")" NOTOP=1 TIMEOUT=$((SECS + 25)) bash "$T/../run.sh" drop >/dev/null 2>&1 || true
echo "server: $(grep -c '^cut' "$STAGE/drop-server.log") cuts"
grep -E "^\[video\] (first picture|paused at|state (buffering|error))|^\[player\].*(error|didn)" "$STAGE/drop.log" | head -8 | cut -c1-160
grep -iE "reconnect|Will reconnect|connection reset|stream ended|EOF reached|curl.*error" "$STAGE/mpv-drop.log" | head -6 | cut -c1-160
grep -q "paused at" "$STAGE/drop.log" && echo "PASS: played through the cuts to $SECS s" || echo "FAIL: did not reach $SECS s"
