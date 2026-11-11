#!/bin/bash
# After a change to shared code: the other targets still build, and the shared tests pass (WSL, as root):
#   tools/windows/test/shared.sh   (from Git Bash it hands itself to WSL)
HERE=$(cd "$(dirname "$0")/../../.." && pwd)
if [ -z "${WSL_DISTRO_NAME:-}" ]; then exec env MSYS_NO_PATHCONV=1 wsl -u root bash "/mnt/c${HERE#/c}/tools/windows/test/shared.sh"; fi
cd "$HERE"; source tools/paths.sh
for t in webos "android arm64-v8a" desktop; do printf "%s: " "$t"; bash tools/build.sh $t 2>&1 | grep -E " error|COMPILE FAILED|BUILD_OK" | head -3; done
SB_ASAN=1 bash tools/build.sh desktop 2>&1 | grep -E " error|COMPILE FAILED|BUILD_OK"
for t in collections recs nuvio; do echo "== tests/$t"; bash tests/$t/run.sh 2>&1 | tail -1; done
echo "== logs-test"; [ -x ./build/logs-test ] && ./build/logs-test vectors 2>&1 | tail -1
echo "== app_test (ASan)"; bash tests/nuvio/app_test.sh "$BUILD_ROOT/desktop-asan/stremioorigin" "$HERE" /root/wapptest 2>&1 | tail -3
