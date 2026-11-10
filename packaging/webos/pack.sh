#!/bin/bash
# packaging/webos/pack.sh [--clean]: the webOS app compiled, then its IPK and .sha256 in OUT (default dist/).
# Needs $ORIGIN_BUILD/official: the official IPK unpacked (data/, ctl/control, data.tar.gz, debian-binary), which holds the
# P2P engine and DTS converter libraries. Use --clean for a release.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../../tools/paths.sh"
bash "$ROOT/tools/build.sh" webos "$@"
python3 "$ROOT/packaging/webos/pack.py" "$ROOT" "$BUILD_ROOT/webos/stremioorigin" "$ORIGIN_BUILD/official" "${OUT:-$ROOT/dist}"
