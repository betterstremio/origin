#!/bin/bash
# Memory over a long session: Home's card previews one after another (arrow keys posted to the app's window every
# STEP seconds, row by row), then the private memory at each preview's first picture and the app's own [mem] lines.
#   tools/windows/test/memory.sh [PREVIEWS] [STEP]   (Git Bash; build staged first)
# Runs from a copy of the staged build (so/windows-run-mem), so rebuilding meanwhile does not touch it. Data: a copy of
# so/windows-run/data-real (a signed-in account) if there is one, else no account. Muted, and not kept on top.
set -eo pipefail
N=${1:-60}; STEP=${2:-3}
T=$(cd "$(dirname "$0")" && pwd); SO=$(cd "$T/../../../.." && pwd); SRC=$SO/windows-run; M=$SO/windows-run-mem
mkdir -p "$M"; cp -u "$SRC"/*.exe "$SRC"/*.dll "$SRC/appinfo.json" "$M/"; rm -rf "$M/assets"; cp -r "$SRC/assets" "$M/assets"
rm -rf "$M/data-mem"; if [ -d "$SRC/data-real/acct" ]; then mkdir -p "$M/data-mem"; cp -r "$SRC/data-real/state.json" "$SRC/data-real/acct" "$M/data-mem/"; [ -f "$SRC/data-real/tmdb.key" ] && cp "$SRC/data-real"/tmdb.key* "$M/data-mem/"; ACCT=1; fi
F=120; f=300; post="250:click 470 340,$f:key DOWN"   # (the profile picker, then from the billboard into the rows)
row=0; dir=RIGHT
for i in $(seq 1 "$N"); do f=$((f + STEP * F)); if [ $((i % 12)) = 0 ]; then post="$post,$f:key DOWN"; [ $dir = RIGHT ] && dir=LEFT || dir=RIGHT; else post="$post,$f:key $dir"; fi; done
secs=$(( f / F + 20 ))
echo "$N previews, a step every $STEP s, about $secs s${ACCT:+, signed in}"
[ -n "${ACCT:-}" ] || export SB_SKIP_LOGIN=1
EXE=$(cygpath -w "$M/Origin.exe"); : > "$M/mem-samples.txt"
( sleep 20; while sleep 20; do powershell -NoProfile -Command "\$p = Get-Process Origin -EA SilentlyContinue | Where-Object { \$_.Path -eq '$EXE' } | Select-Object -First 1; if (\$p) { [int](\$p.PrivateMemorySize64 / 1MB) } else { exit 1 }" >> "$M/mem-samples.txt" || break; done ) &
STAGE="$M" NOTOP=1 SB_POST="$post" TIMEOUT=$secs bash "$T/../run.sh" mem >/dev/null 2>&1 || true; wait
echo "private MB every 20 s: $(tr -d '\r' < "$M/mem-samples.txt" | tr '\n' ' ')"
L="$M/mem.log"
echo "previews started: $(grep -c 'first picture' "$L")"
grep -o 'first picture [0-9]* ms after the load (app [0-9]* MB, [0-9]* MB private)' "$L" | awk '{print NR": "$9" MB working set, "$11" MB private ("$3" ms)"}' | awk 'NR<=3 || NR%10==0 {print} {last=$0} END {print "last " last}'
grep '^\[mem\] app' "$L" | awk 'NR == 1 || NR % 4 == 0 {print} {l = $0} END {print l}' | cut -c1-150   # (the app's own count: working set, pictures, textures)
grep -E '\[crash\]|ERROR|failed' "$L" | head -5
