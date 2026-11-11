#!/bin/bash
# The sound mpv would send to a stereo device, written to a file instead of the speakers (ao=pcm), then measured:
# (the film is held paused near its end: a trailer on Home afterwards would write over the file)
# peak and how many samples go past full scale (clipping, heard as crackle).
#   tools/windows/test/audio.sh CLIP SECONDS NAME="mpv opts" ...   (Git Bash; build staged first)
set -eo pipefail
CLIP=$(cygpath -am "$1"); SECS=$2; shift 2
T=$(cd "$(dirname "$0")" && pwd); STAGE=$(cd "$T/../../../.." && pwd)/windows-run
for v in "$@"; do
  n=${v%%=*}; o=${v#*=}; W="$(cygpath -am "$STAGE")/aud-$n.wav"; rm -f "$W"; rm -rf "$STAGE/data-aud-$n"
  SB_SKIP_LOGIN=1 SB_PLAY_NOSAVE=1 SB_PLAY_ID=${SB_PLAY_ID:-tt22022452} SB_PLAY_TITLE="${SB_PLAY_TITLE:-Inside Out 2}" SB_PLAY="$CLIP" SB_SOUND=1 SB_PAUSE_AT=$((SECS - 1)) \
    SB_MPV_OPTS="ao=pcm;ao-pcm-file=$W;ao-pcm-waveheader=yes;audio-channels=stereo;audio-format=float${o:+;$o}" TIMEOUT=$((SECS + 16)) \
    bash "$T/../run.sh" "aud-$n" >/dev/null 2>&1 || true
  MSYS_NO_PATHCONV=1 wsl -u root python3 - "/mnt/c${W#C:}" "$n" <<'PY'
import sys, struct, array
d = open(sys.argv[1], 'rb').read()
i = d.find(b'data'); a = array.array('f'); a.frombytes(d[i + 8:i + 8 + ((len(d) - i - 8) // 4) * 4])
pk = max(abs(x) for x in a) if a else 0; over = sum(1 for x in a if abs(x) > 1.0)
print(f"{sys.argv[2]:>10}: {len(a) / 2 / 48000:5.1f} s, peak {pk:.3f} ({'%+.1f' % (20 * __import__('math').log10(pk)) if pk else '-'} dBFS), samples past full scale {over}")
PY
done
