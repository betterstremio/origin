#!/bin/bash
# Serve Origin's synthetic MP4, HLS, subtitles and v1 torrent to a physical TV.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST="${1:?Usage: bash tests/tizen/serve.sh LAN_IPV4 [PORT]}"
PORT="${2:-8765}"
AB="${AB:-$HOME/abuild}"
AB="$AB" bash "$ROOT/tools/android/test/media.sh"
MEDIA="$AB/testmedia"

python3 - "$MEDIA" "$HOST" "$PORT" <<'PYTHON'
import ipaddress
import json
from pathlib import Path
import sys

media = Path(sys.argv[1])
host = ipaddress.IPv4Address(sys.argv[2])
port = int(sys.argv[3])
base = f"http://{host}:{port}"
item_id = "origin-tizen-qa"
item = {"id": item_id, "type": "movie", "name": "Origin TV test clip"}

def write(path, payload):
    file = media / path
    file.parent.mkdir(parents=True, exist_ok=True)
    file.write_text(json.dumps(payload), encoding="utf-8")

write("manifest.json", {
    "id": "io.github.betterstremio.origin.qa", "version": "1.0.0",
    "name": "Origin TV test streams", "description": "Synthetic preview test media",
    "types": ["movie"], "resources": ["catalog", "meta", "stream"],
    "idPrefixes": [item_id],
    "catalogs": [{"id": "origin-tv-tests", "type": "movie", "name": "Origin TV tests"}],
})
write("catalog/movie/origin-tv-tests.json", {"metas": [item]})
write(f"meta/movie/{item_id}.json", {"meta": item})
write(f"stream/movie/{item_id}.json", {"streams": [
    {"name": "QA MP4", "url": f"{base}/media/clip.mp4",
     "subtitles": [{"id": "qa-en", "lang": "eng", "url": f"{base}/media/clip.srt"}]},
    {"name": "QA HLS", "url": f"{base}/media/hls/master.m3u8"},
    {"name": "QA torrent", "infoHash": (media / "clip.ih").read_text().strip(),
     "fileIdx": 0, "sources": [f"tracker:{base}/announce"]},
]})
print(f"Add-on URL: {base}/media/manifest.json")
PYTHON

exec python3 "$ROOT/tools/android/test/serve.py" "$MEDIA" "$PORT" "$HOST" 716800
