#!/bin/bash
# The webOS listing on the website, for the Homebrew Channel and for the app's own update check:
#   <site>/webos/apps.json                                  the Homebrew Channel repository (add https://betterstremio.github.io/webos/apps.json)
#   <site>/webos/api/apps/<appid>/manifest.json             version, IPK link and SHA-256, notes
#   <site>/webos/icon.png
# The app reads the manifest first (GitHub Pages has no hourly limit) and GitHub's release list only when it fails.
# Older installs (2.1.4 and before) read the same manifest when the release list fails.
#   tools/webos-feed.sh X.Y.Z NOTES.md SITE_DIR      writes the files from the release on GitHub (its IPK and digest)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/paths.sh"
V="${1:-}"; NOTES="${2:-}"; SITE="${3:-}"
SLUG="${REPO_SLUG:-betterstremio/origin}"
die() { echo "STOPPED: $*"; exit 1; }
[[ "$V" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] && [ -f "$NOTES" ] && [ -d "$SITE" ] || die "usage: tools/webos-feed.sh X.Y.Z NOTES.md SITE_DIR"
IPK="${APPID}_${V}_arm.ipk"
DG="$(gh api "repos/$SLUG/releases/tags/v$V" --jq ".assets[] | select(.name == \"$IPK\") | .digest")"
[[ "$DG" =~ ^sha256:[0-9a-f]{64}$ ]] || die "no $IPK with a digest in release v$V"
PY="$(command -v python3 || command -v python)" || die "no python"
mkdir -p "$SITE/webos/api/apps/$APPID"
cp "$ROOT/packaging/webos/icon.png" "$SITE/webos/icon.png"
"$PY" - "$V" "$NOTES" "$SITE" "$APPID" "$SLUG" "${DG#sha256:}" <<'EOF'
import json, sys
v, notes, site, appid, slug, sha = sys.argv[1:]
base = "https://betterstremio.github.io/webos"
desc = "Stremio and Nuvio accounts on your LG TV: artwork, trailers, the best stream in one press, subtitles, torrents. Unofficial."
md = open(notes, encoding="utf-8").read()
man = {"id": appid, "version": v, "type": "native", "title": "Origin", "appDescription": desc,
       "iconUri": base + "/icon.png", "sourceUrl": "https://github.com/" + slug, "rootRequired": False,
       "ipkUrl": f"https://github.com/{slug}/releases/download/v{v}/{appid}_{v}_arm.ipk",
       "ipkHash": {"sha256": sha}, "releaseNotes": md}
lst = {"paging": {"page": 1, "count": 1, "maxPage": 1, "itemsTotal": 1},
       "packages": [{"id": appid, "title": "Origin", "iconUri": base + "/icon.png", "pool": "main",
                     "manifestUrl": f"{base}/api/apps/{appid}/manifest.json", "shortDescription": desc,
                     "manifest": man}]}
for p, o in ((f"{site}/webos/api/apps/{appid}/manifest.json", man), (f"{site}/webos/apps.json", lst)):
    with open(p, "w", encoding="utf-8", newline="\n") as f: json.dump(o, f, indent=2, ensure_ascii=False); f.write("\n")
EOF
echo "webOS listing for $V written to $SITE/webos (IPK sha256 ${DG:7:12}...)"
