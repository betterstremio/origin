#!/bin/bash
# scroll_test.sh: the strict fast-scroll test, Windows first (Git Bash). Builds and stages this tree, drives the app with
# the in-app wall-clock key timeline (SB_SCROLL=<profile>, src/core/scrollin.c), collects the "[fill] phase=" lines
# (src/core/fillstat.c) and checks them against perf/budget.json with perf/perf_check.py (exit 1 fail, 2 noisy, 3 no data).
#
#   ./scroll_test.sh [-p "CTRL T250 T150 H-V H-R B5 RT"] [-n RUNS] [-c CPUS] [-m MEMMB] [-d DEVICE] [-t TAG]
#                    [-S STAGE_DIR] [-o OUT_DIR] [-e "SB_DEV_SLOWDEC=150 ..."] [-C] [-B] [-N ROWS]
#     -p  profiles (default: all)      -n  measured runs per profile (default 5)
#     -c  SB_CPUS (core count)         -m  SB_MEMMB (fake RAM, 1024 = a Firestick)
#     -d  budget device: C1 | emulator | windows | windows-2cpu (default: windows-2cpu when -c 2, else windows)
#     -t  build label for the history file (default: git short hash)
#     -S  stage folder (default ../scroll-test-run)   -o  where logs go (default <stage>/out/<tag>-<cpus>-<mem>)
#     -e  extra environment for the app: the slow-build switches SB_DEV_SLOWDEC=ms, SB_DEV_BUDGET_PCT=50, SB_DEV_NOAHEAD=1
#     -C  cold: the picture cache is emptied before every run (default warm: one unmeasured run fills the cache first)
#     -B  skip the build (use what is staged)   -N  rows a tap profile walks (SB_SCROLL_N, default 8)
# Needs the signed-in data template ../win-mouse-run/data-tmpl (TMPL=...). One app at a time: it takes the screen.
# Other platforms: the same SB_SCROLL / SB_INPUT environment goes in through webOS launch params and an Android intent
# extra (not wired yet, see TODO.md); perf_check.py is platform independent (--device C1 / emulator).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PROFILES="CTRL T250 T150 H-V H-R B5 RT"; RUNS=5; CPUS=""; MEMMB=""; DEV=""; TAG=""; STAGE=""; OUT=""; EXTRA=""; COLD=""; NOBUILD=""; ROWS=8
while getopts "p:n:c:m:d:t:S:o:e:CBN:" o; do case $o in
  p) PROFILES=$OPTARG;; n) RUNS=$OPTARG;; c) CPUS=$OPTARG;; m) MEMMB=$OPTARG;; d) DEV=$OPTARG;; t) TAG=$OPTARG;;
  S) STAGE=$OPTARG;; o) OUT=$OPTARG;; e) EXTRA=$OPTARG;; C) COLD=1;; B) NOBUILD=1;; N) ROWS=$OPTARG;; *) sed -n 2,20p "$0"; exit 64;; esac; done
STAGE=${STAGE:-$(cd "$HERE/.." && pwd)/scroll-test-run}
TMPL=${TMPL:-$HERE/../win-mouse-run/data-tmpl}
[ -n "$TAG" ] || TAG=$(git -C "$HERE" rev-parse --short HEAD)
[ -n "$DEV" ] || { [ "$CPUS" = 2 ] && DEV=windows-2cpu || DEV=windows; }
CACHE=warm; [ -z "$COLD" ] || CACHE=cold
OUT=${OUT:-$STAGE/out/$TAG-${CPUS:-all}c-${MEMMB:-all}m${EXTRA:+-$(echo "$EXTRA" | tr -c 'A-Za-z0-9\n' _)}-$CACHE}
mkdir -p "$OUT"
if [ -z "$NOBUILD" ]; then
  WSTAGE=$(echo "$STAGE" | sed 's|^/\([a-z]\)/|/mnt/\1/|')
  "$HERE/tools/windows/rebuild.sh" "$WSTAGE" || { echo "build failed"; exit 1; }
fi
[ -d "$TMPL" ] || { echo "no data template $TMPL"; exit 1; }
one() { # one PROFILE LOGNAME: a fresh app start on the profile's data folder
  local p=$1 name=w-$1
  ( export SB_SCROLL=$p SB_SCROLL_N=$ROWS SB_WINDOW=1920x1080 STAGE TIMEOUT=${TIMEOUT:-200}
    [ -z "$CPUS" ] || export SB_CPUS=$CPUS
    [ -z "$MEMMB" ] || export SB_MEMMB=$MEMMB
    for kv in $EXTRA; do export "$kv"; done
    "$HERE/tools/windows/run.sh" "$name" >/dev/null 2>&1 )
  cp "$STAGE/$name.log" "$2"
}
for p in $PROFILES; do
  name=w-$p; rm -rf "$STAGE/data-$name"; cp -r "$TMPL" "$STAGE/data-$name"
  if [ -z "$COLD" ]; then one "$p" "$OUT/$p-prime.log"; echo "[$p] warm-up run done"; fi
  for i in $(seq 1 "$RUNS"); do
    [ -z "$COLD" ] || rm -rf "$STAGE/data-$name/cache"
    one "$p" "$OUT/$p-$i.log"
    echo "[$p run $i] $(grep -c '^\[fill\] phase=' "$OUT/$p-$i.log") phases"
  done
done
echo "logs in $OUT"
grep -h '^\[fill\] phase=' "$OUT"/*-[0-9]*.log | cut -c1-330 | head -${SHOWLINES:-0}
python -I "$HERE/perf/perf_check.py" --device "$DEV" --cache "$CACHE" --tag "$TAG${EXTRA:+ $EXTRA}${CPUS:+ cpus=$CPUS}${MEMMB:+ mem=$MEMMB}" \
  --history "${HISTORY:-$HERE/perf/history.jsonl}" $(ls "$OUT"/*-[0-9]*.log)
