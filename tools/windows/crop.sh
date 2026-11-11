#!/bin/bash
# Crop and shrink a screenshot for viewing (WSL's Pillow), from Git Bash:
#   tools/windows/crop.sh IN.png OUT.png [X Y W H] [MAXW]   (no box: whole picture; MAXW default 1920)
set -e
IN=$(cygpath -am "$1"); OUT=$(cygpath -am "$2"); shift 2
BOX="${1:-} ${2:-} ${3:-} ${4:-}"; MAXW="${5:-1920}"
MSYS_NO_PATHCONV=1 wsl -u root python3 -c "
import sys
from PIL import Image
im = Image.open(sys.argv[1]); b = sys.argv[3].split()
if len(b) == 4: x, y, w, h = map(int, b); im = im.crop((x, y, x + w, y + h))
m = int(sys.argv[4])
if im.width > m: im = im.resize((m, im.height * m // im.width), Image.LANCZOS)
im.save(sys.argv[2]); print(sys.argv[2], im.size)
" "/mnt/c${IN#C:}" "/mnt/c${OUT#C:}" "$BOX" "$MAXW"
