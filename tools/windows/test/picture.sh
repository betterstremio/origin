#!/bin/bash
# The picture as the viewer sees it, for one clip under several mpv settings: each run stops the clip at END seconds
# (SB_PAUSE_AT), captures the composed screen, and measures fine noise in a box against a reference frame.
#   tools/windows/test/picture.sh CLIP REF.png END "X Y W H" NAME="opts;opts" ...   (Git Bash; build staged first)
#   CLIP: a local file; REF.png: the same frame decoded in software; the box is in the reference's pixels
set -eo pipefail
CLIP=$(cygpath -am "$1"); REF=$(cygpath -am "$2"); END=$3; BOX=$4; shift 4
T=$(cd "$(dirname "$0")" && pwd); STAGE=$(cd "$T/../../../.." && pwd)/windows-run
for v in "$@"; do
  n=${v%%=*}; o=${v#*=}
  rm -rf "$STAGE/data-pic-$n"
  SB_SKIP_LOGIN=1 SB_PLAY_NOSAVE=1 SB_PLAY_ID=${SB_PLAY_ID:-tt22022452} SB_PLAY_TITLE="${SB_PLAY_TITLE:-Inside Out 2}" SB_PLAY="$CLIP" SB_PAUSE_AT=$END SB_MPV_OPTS="$o" SB_WINDOW=${SB_WINDOW:-3840x2054} SCREEN=${SCREEN:-1900} TIMEOUT=${TIMEOUT:-35} \
    bash "$T/../run.sh" "pic-$n" >/dev/null 2>&1 || true
  grep -E "decoding:|mpv option" "$STAGE/pic-$n.log" | tr '\n' ' ' | cut -c1-160; echo
  MSYS_NO_PATHCONV=1 wsl -u root python3 - "/mnt/c${REF#C:}" "/mnt/c$(cygpath -am "$STAGE/pic-$n-screen.png" | cut -c3-)" "$BOX" "$n" <<'PY'
import sys, statistics
from PIL import Image
r = Image.open(sys.argv[1]).convert('RGB'); a = Image.open(sys.argv[2]).convert('RGB')
x, y, w, h = map(int, sys.argv[3].split())
# the app letterboxes the 16:9 picture into its window: map the reference box into the window
s = min(a.width / r.width, a.height / r.height); ox = (a.width - r.width * s) / 2; oy = (a.height - r.height * s) / 2
ab = (int(ox + x * s), int(oy + y * s), int(ox + (x + w) * s), int(oy + (y + h) * s))
ca = a.crop(ab).resize((w, h), Image.LANCZOS); cr = r.crop((x, y, x + w, y + h))
def hf(im):
    g = im.convert('L'); p = g.load(); W, H = g.size
    return statistics.mean(abs(2 * p[i, j] - p[i - 1, j] - p[i + 1, j]) + abs(2 * p[i, j] - p[i, j - 1] - p[i, j + 1]) for j in range(1, H - 1, 2) for i in range(1, W - 1, 2))
def mean(im): return [round(statistics.mean(list(c.getdata())), 1) for c in im.split()]
na, nr = hf(ca), hf(cr); la, lr = statistics.mean(ca.convert('L').getdata()), statistics.mean(cr.convert('L').getdata())
print(f"{sys.argv[4]:>10}: noise {na:5.2f} (ref {nr:5.2f}), per brightness {100 * na / la:5.2f} (ref {100 * nr / lr:5.2f})  mean RGB {mean(ca)} (ref {mean(cr)})")
o = Image.new('RGB', (w * 2, h)); o.paste(cr, (0, 0)); o.paste(ca, (w, 0)); o.save(sys.argv[2].replace('-screen.png', '-cmp.png'))
PY
done
