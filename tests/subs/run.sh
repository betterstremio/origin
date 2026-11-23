#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${OUT:-$ROOT/build}"
mkdir -p "$OUT"
"${CC:-cc}" -std=c11 -Wall -Wextra -I"$ROOT/src" -I"$ROOT/third_party/cjson" \
  "$ROOT/tests/subs/lang_test.c" "$ROOT/src/core/subs_lang.c" -o "$OUT/subs_lang_test"
"$OUT/subs_lang_test"
