#!/bin/bash
# The webOS listing on the website, for the Homebrew Channel and for the app's own update check:
#   <site>/webos/apps.json                                  the Homebrew Channel repository (add https://betterstremio.github.io/webos/apps.json)
#   <site>/webos/api/apps/<appid>/manifest.json             version, IPK link and SHA-256, notes
#   <site>/webos/icon.png
#   <site>/webos/api/apps/tv.stremioorigin.app/manifest.json  the old id's manifest (2.1.12 and before read it), always
#                                                           naming the newest new-id IPK: their updater checks only that
#                                                           the link ends in _arm.ipk, installs it beside itself, and
#                                                           3.0.0's first run imports the old data and removes the old app
# The app reads the manifest first (GitHub Pages has no hourly limit) and GitHub's release list only when it fails.
# Older installs (2.1.4 and before) read the same manifest when the release list fails.
#   <site>/updates/<appid>/<kind>.json                      every other package (from 3.0.9), in the manifest's form, one
#                                                           file per kind (windows_setup.exe.json, android-arm64.apk.json,
#                                                           macos-arm64.dmg.json...): those apps read it first
#                                                           (src/core/update.c UPD_PKG)
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
ASSETS="$(gh api "repos/$SLUG/releases/tags/v$V" --jq '.assets[] | .name + " " + .digest')"
OLDID=tv.stremioorigin.app
mkdir -p "$SITE/webos/api/apps/$APPID" "$SITE/webos/api/apps/$OLDID" "$SITE/updates/$APPID"
cp "$ROOT/packaging/webos/icon.png" "$SITE/webos/icon.png"
"$PY" - "$V" "$NOTES" "$SITE" "$APPID" "$SLUG" "${DG#sha256:}" "$OLDID" "$ASSETS" <<'EOF'
import json, re, sys
v, notes, site, appid, slug, sha, oldid, assets = sys.argv[1:]
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
old = dict(man, id=oldid)   # same IPK (new id): installs beside the old app, which the new one then removes
for p, o in ((f"{site}/webos/api/apps/{appid}/manifest.json", man), (f"{site}/webos/api/apps/{oldid}/manifest.json", old),
             (f"{site}/webos/apps.json", lst)):
    with open(p, "w", encoding="utf-8", newline="\n") as f: json.dump(o, f, indent=2, ensure_ascii=False); f.write("\n")
# every other package: <appid>_<v>_<kind>, with GitHub's digest of the file as uploaded (update.c reads these names
# exactly as it reads the webOS manifest's)
kinds = []
for line in assets.splitlines():
    name, _, dg = line.partition(" ")
    m = re.fullmatch(re.escape(f"{appid}_{v}_") + r"(.+)", name)
    if not m or name.endswith(".sha256") or m.group(1) == "arm.ipk" or not re.fullmatch(r"sha256:[0-9a-f]{64}", dg):
        continue
    o = {"id": appid, "version": v, "title": "Origin",
         "ipkUrl": f"https://github.com/{slug}/releases/download/v{v}/{name}", "ipkHash": {"sha256": dg[7:]},
         "releaseNotes": md}
    with open(f"{site}/updates/{appid}/{m.group(1)}.json", "w", encoding="utf-8", newline="\n") as f:
        json.dump(o, f, indent=2, ensure_ascii=False); f.write("\n")
    kinds.append(m.group(1))
if not kinds:
    sys.exit("STOPPED: no package besides the IPK in the release")
print("update files: " + ", ".join(sorted(kinds)))
EOF
echo "webOS listing for $V written to $SITE/webos (IPK sha256 ${DG:7:12}...)"
