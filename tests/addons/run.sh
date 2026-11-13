#!/bin/bash
# tests/addons/run.sh : builds and runs the addon address test; SB_ASAN=1 for a sanitizer build
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; OUT="${OUT:-$ROOT/build}"; mkdir -p "$OUT"
cc -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -O1 -g ${SB_ASAN:+-fsanitize=address,undefined} -I"$ROOT/src" -I"$ROOT/third_party/cjson" \
  "$ROOT/tests/addons/addons_test.c" "$ROOT/src/core/addons.c" "$ROOT/third_party/cjson/cJSON.c" -lm -o "$OUT/addons_test"
"$OUT/addons_test" "$@"
