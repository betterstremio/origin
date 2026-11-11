#!/bin/bash
# The player on a PC with no Vulkan loader of its own (a VM, Remote Desktop, the basic display driver): a copy of the
# staged build whose libmpv-2.dll asks for "vulkan-9.dll" (a name no Windows has, same length) and whose fallback\
# holds the bundled loader under that name. The first load must fail, the retry must load it, and a clip must play.
#   tools/windows/test/novulkan.sh CLIP   (Git Bash; build staged first)
set -eo pipefail
CLIP=$(cygpath -am "$1")
T=$(cd "$(dirname "$0")" && pwd); SO=$(cd "$T/../../../.." && pwd); SRC=$SO/windows-run; N=$SO/windows-run-novk
rm -rf "$N"; mkdir -p "$N/fallback"; cp "$SRC"/*.exe "$SRC"/*.dll "$SRC/appinfo.json" "$N/"; cp -r "$SRC/assets" "$N/assets"
cp "$SRC/fallback/vulkan-1.dll" "$N/fallback/vulkan-9.dll"
python -I - "$N/libmpv-2.dll" <<'PY'
import sys
p = sys.argv[1]; b = open(p, 'rb').read()
n = b.count(b'vulkan-1.dll'); assert n >= 1, 'no vulkan-1.dll import'
open(p, 'wb').write(b.replace(b'vulkan-1.dll', b'vulkan-9.dll')); print('libmpv patched:', n, 'name(s)')
PY
STAGE="$N" SB_SKIP_LOGIN=1 SB_PLAY_NOSAVE=1 SB_PLAY="$CLIP" SB_PLAY_ID=tt22022452 SB_PLAY_TITLE="Inside Out 2" SB_PAUSE_AT=4 TIMEOUT=25 NOTOP=1 \
  bash "$T/../run.sh" novk >/dev/null 2>&1 || true
grep -E "^\[video\] (libmpv|decoding|first picture|state playing|paused)" "$N/novk.log" | head -6 | cut -c1-160
