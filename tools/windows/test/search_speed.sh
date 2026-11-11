#!/bin/bash
# Search speed, gently: ONE app start per round (a start loads all of Home, ~500 requests: many starts in a row got us
# rate limited), every query typed in that one session one key every ~150 ms as a person types, 10 s apart, then the
# first query typed again (Backspace and retyping are what the in-memory searches are for). The app's [sstat] lines
# (SB_SLOWLOG) give milliseconds from the LAST key to: IMDb asked, IMDb's answer, the first results, the first poster,
# the catalogs asked, every catalog answered.
#   tools/windows/test/search_speed.sh [ROUNDS] [QUERY...]   (Git Bash; build staged first, STAGE= the staged folder)
#   PAUSE=60: seconds between rounds. Data: $STAGE/data-real, else so/windows-run/data-real (signed in), else none.
set -eo pipefail
ROUNDS=${1:-2}; shift || true; [ $# -gt 0 ] || set -- inception "the office" dune
T=$(cd "$(dirname "$0")" && pwd); SO=$(cd "$T/../../../.." && pwd)
STAGE=${STAGE:-$SO/windows-run}; REAL=$STAGE/data-real; [ -d "$REAL/acct" ] || REAL=$SO/windows-run/data-real
OUT=$STAGE/search-speed; mkdir -p "$OUT"; : > "$OUT/rows.txt"
F=${FPK:-18}   # frames per key (120 fps: 150 ms); Backspace between queries goes faster (5 frames), so it asks nothing
for r in $(seq 1 "$ROUNDS"); do
  [ "$r" = 1 ] || sleep "${PAUSE:-60}"
  rm -rf "$STAGE/data-ss"; mkdir -p "$STAGE/data-ss"
  if [ -d "$REAL/acct" ]; then cp -r "$REAL/state.json" "$REAL/acct" "$STAGE/data-ss/"; [ -f "$REAL/tmdb.key" ] && cp "$REAL"/tmdb.key* "$STAGE/data-ss/"; else export SB_SKIP_LOGIN=1; fi
  python -I -c "import json,sys; p=sys.argv[1]; j=json.load(open(p,encoding='utf-8')); j['askProfile']='false'; json.dump(j,open(p,'w',encoding='utf-8'))" "$STAGE/data-ss/state.json" 2>/dev/null || true   # (no "Who's watching?")
  f=480; post=""; prev=""   # (4 s: start-up done even on a slow network)
  for q in "$@" "$1"; do
    for ((i = 0; i < ${#prev}; i++)); do post="$post,$f:key BACKSPACE"; f=$((f + 5)); done; f=$((f + 60))
    for ((i = 0; i < ${#q}; i++)); do post="${post:+$post,}$f:type ${q:i:1}"; f=$((f + F)); done
    f=$((f + 1200)); prev=$q   # 10 s for its answers
  done
  STAGE="$STAGE" NOTOP=1 SB_SLOWLOG=1 SB_POST="$post" TIMEOUT=$((f / 120 + 25)) bash "$T/../run.sh" ss "$f" > /dev/null 2>&1 || true
  cp "$STAGE/ss.log" "$OUT/round-$r.log"
  # each query's lines, from its last "asked imdb" (or "from memory") to the next query's; the repeat counts as "<q> again"
  python -I - "$STAGE/ss.log" "$@" >> "$OUT/rows.txt" <<'PY'
import re, sys
log, qs = sys.argv[1], sys.argv[2:]
blocks, seen, cur = {}, {}, None
for ln in open(log, encoding="utf-8", errors="replace"):
    if not ln.startswith("[sstat]"): continue
    m = re.search(r'(asked imdb for|from memory for) "(.*)"', ln)
    if m:
        q = m.group(2).strip()
        if q in qs and (cur is None or cur.split("#")[0] != q):
            seen[q] = seen.get(q, 0) + 1
        cur = q + "#" + str(seen.get(q, 1)); blocks[cur] = []
    if cur: blocks[cur].append(ln)
def ms(b, pat):
    for ln in b:
        if re.search(pat, ln): return re.match(r"\[sstat\] \+ *(\d+)", ln).group(1)
    return "-"
for key in [q + "#1" for q in qs] + [qs[0] + "#2"]:
    b = blocks.get(key, [])
    name = key.replace("#1", "").replace("#2", " again")
    vals = [ms(b, "asked imdb|from memory"), ms(b, "imdb answered|from memory"), ms(b, "first results|from memory"), ms(b, "first poster"), ms(b, r"asked \d+ catalog"), ms(b, "all answered|from memory")]
    print(name.replace(" ", "_"), *vals)
PY
  echo "round $r done"
done
echo "medians (ms after the last key): query, imdb asked, imdb answered, first results, first poster, catalogs asked, all answered"
python -I - "$OUT/rows.txt" <<'PY'
import sys, statistics
rows = {}
for ln in open(sys.argv[1]):
    p = ln.split(); rows.setdefault(p[0], []).append(p[1:])
for q, rs in rows.items():
    med = []
    for k in range(6):
        v = [int(r[k]) for r in rs if r[k].isdigit()]
        med.append(str(int(statistics.median(v))) if v else "-")
    print(f"  {q.replace('_', ' '):<16}", "  ".join(f"{m:>5}" for m in med), f"  ({len(rs)} rounds)")
PY
