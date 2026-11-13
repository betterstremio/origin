#!/bin/bash
# Formats the project's C code with clang-format, using the .clang-format at the repo root. third_party/ is left alone.
#   tools/format.sh              every tracked .c/.h file under src/, tests/ and tools/
#   tools/format.sh FILE...      only these files
#   tools/format.sh --check      change nothing; list the files that need formatting and exit 1 if there are any
# Needs clang-format 19.1.7, the version the tree was formatted with: pip install clang-format==19.1.7
# (CLANG_FORMAT=/path/to/clang-format picks a binary by hand.)
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
WANT=19.1.7

CF="${CLANG_FORMAT:-}"
if [ -z "$CF" ]; then
  if command -v clang-format >/dev/null 2>&1; then CF=clang-format
  else   # the pip package keeps its binary next to the module, which is not always on PATH
    for py in python3 python; do
      d=$("$py" -c 'import clang_format, os; print(os.path.join(os.path.dirname(clang_format.__file__), "data", "bin"))' 2>/dev/null) || continue
      for b in "$d/clang-format" "$d/clang-format.exe"; do [ -x "$b" ] && CF="$b" && break 2; done
    done
  fi
fi
[ -n "$CF" ] || { echo "clang-format not found: pip install clang-format==$WANT"; exit 1; }
have=$("$CF" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
[ "$have" = "$WANT" ] || echo "warning: clang-format $have, the tree uses $WANT; a few lines may come out different"

CHECK=0; FILES=()
for a in "$@"; do case "$a" in --check) CHECK=1;; *) FILES+=("$a");; esac; done
if [ ${#FILES[@]} = 0 ]; then
  mapfile -t FILES < <(git ls-files 'src/*.c' 'src/*.h' 'tests/*.c' 'tests/*.h' 'tools/*.c' 'tools/*.h')
fi

if [ "$CHECK" = 1 ]; then
  bad=0
  for f in "${FILES[@]}"; do
    "$CF" --style=file --dry-run --Werror "$f" >/dev/null 2>&1 || { echo "$f"; bad=1; }
  done
  exit "$bad"
fi
printf '%s\0' "${FILES[@]}" | xargs -0 "$CF" -i --style=file
