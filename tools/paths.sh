# Where things are, for every build and packaging script. Sourced, not run. Each one can be set from outside.
#   ROOT          this checkout
#   ORIGIN_BUILD  private folder outside the repo: toolchains, SDK, keys, the official IPK's unpacked files
#   KEYS          the private keys file (see tools/sbkeys.py)
#   AB            what every Android build shares: dependencies, P2P engine, Gradle, test media
#   WB            the same for the Windows build
#   BUILD_ROOT    objects and outputs of this checkout. A checkout on a Windows drive (WSL) builds on the Linux side, one
#                 folder per checkout, so two worktrees never link each other's objects
ROOT="${ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
ORIGIN_BUILD="${ORIGIN_BUILD:-$HOME/origin-build}"
KEYS="${KEYS:-$ORIGIN_BUILD/keys.env}"
AB="${AB:-$HOME/abuild}"
WB="${WB:-$HOME/wbuild}"   # the Windows build's dependencies (tools/windows/build-deps.sh)
case "$ROOT" in /mnt/*) BUILD_ROOT="${BUILD_ROOT:-$HOME/obuild/$(basename "$ROOT")}";; *) BUILD_ROOT="${BUILD_ROOT:-$ROOT/build}";; esac
APPID="${APPID:-tv.stremioorigin.app}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
TP="$ROOT/third_party"
