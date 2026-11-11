#!/bin/bash
# Build and stage the Windows app in one step. From Git Bash it hands itself to WSL (as root), where the build runs:
#   tools/windows/rebuild.sh   -> so/windows-run; stops on a failed build and shows the errors
set -o pipefail
HERE=$(cd "$(dirname "$0")/../.." && pwd)
if [ -z "${WSL_DISTRO_NAME:-}" ]; then
  exec env MSYS_NO_PATHCONV=1 wsl -u root bash "/mnt/c${HERE#/c}/tools/windows/rebuild.sh"
fi
cd "$HERE"
tools/build.sh windows > /tmp/origin-winbuild.log 2>&1; r=$?
grep -E ' error |COMPILE FAILED|Origin.exe ' /tmp/origin-winbuild.log | head -20
[ "$r" = 0 ] || { echo "BUILD FAILED (full log /tmp/origin-winbuild.log in WSL)"; exit "$r"; }
tools/windows/stage.sh | tail -1
