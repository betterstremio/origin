Third-party code in this app and its licences:
- libsbengine.so: the torrent engine github.com/NuvioMedia/nuvio-engine (GPL-3.0, torrent-engine-GPL-3.0.txt), patched (tools/engine/engine.patch in the source),
- Collections (src/core/collections.c, sb_source_def in src/core/stremio.c): logic ported from github.com/NuvioMedia/NuvioTVSmart (GPL-3.0).
  on libtorrent 2.0.12 (BSD, libtorrent-BSD.txt) and Boost 1.86 (Boost Software License 1.0, boost-BSL-1.0.txt).
- libsbdts.so: the sound converter (tools/dts/sbdts.c) on FFmpeg 7.1.1 (LGPL 2.1 or later, ffmpeg-LGPL-2.1.txt), unmodified, built
  as described in ffmpeg-SOURCE.txt (tools/build-ffmpeg.sh); a shared library of its own, so it can be rebuilt and replaced.
- SHA-256 and AES (third_party/crypto): Brad Conte's crypto-algorithms, public domain (crypto-algorithms-public-domain.txt).
- P-256 key exchange (third_party/crypto/uecc): micro-ecc by Kenneth MacKay, BSD-2-Clause (micro-ecc-BSD-2-Clause.txt).
- FreeType, HarfBuzz, FriBidi, libass (ISC, 0.17.5), cJSON, stb, qrcodegen, libwebp, SDL2: their licences ship with their sources in third_party/.
The Android build adds (tools/android/build-deps.sh fetches and pins them; they are not in third_party/):
- SDL 2.30.9 (zlib, sdl2-zlib.txt): libSDL2.so and its Java activity.
- curl 8.11.0 (curl licence, MIT-style, curl-MIT-like.txt): libcurl.so.
- Mbed TLS 3.6.2 (Apache-2.0, mbedtls-Apache-2.0.txt): inside libcurl.so.
- AndroidX Media3 1.5.0 / ExoPlayer and AndroidX Annotation (Apache-2.0, the same text as mbedtls-Apache-2.0.txt): the video player.
The Windows build adds (tools/windows/build-deps.sh fetches and pins them; they are not in third_party/):
- SDL 2.30.9 (sdl2-zlib.txt), curl 8.11.0 (curl-MIT-like.txt), Mbed TLS 3.6.2 (mbedtls-Apache-2.0.txt) and zlib 1.3.1
  (zlib licence, zlib-zlib.txt): linked into Origin.exe.
- The mingw-w64 runtime and winpthreads, linked into Origin.exe and libsbengine.dll (ZPL-2.1 and others, each file
  named in mingw-w64-runtime-copyright.txt).
- ANGLE (libEGL.dll, libGLESv2.dll), unmodified, as shipped in Electron 43.8.3: BSD-3-Clause, with the licences of
  the Chromium code inside it in LICENSES.chromium.html.gz (gzip; any browser opens the unpacked file).
- libmpv-2.dll: mpv and FFmpeg, unmodified, as built by github.com/shinchiro/mpv-winbuild-cmake (release 20261110, mpv
  git a61f6c3abd); GPL-3.0 as built (the text in torrent-engine-GPL-3.0.txt). Sources: mpv-SOURCE.txt.
- libsbengine.dll: the torrent engine above, built for Windows with tools/engine/windows.patch as well.
The app itself is GPL-3.0.
