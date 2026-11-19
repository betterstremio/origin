# Samsung TVs (preview)

The Samsung build is a preview, and we have not run it on a Samsung TV yet. It builds on the [Stremio Origin Samsung port](https://github.com/stremio-origin/tizen) at version 1.1.0, which so far works best on Tizen 4 to 5.5 (TVs from about 2018 to 2020). On newer TVs that port often closes during start-up. If Origin fails on your TV, [open an issue](https://github.com/betterstremio/origin/issues) with the TV model, its Tizen version and what you saw.

Each release has `io.github.betterstremio.origin_<version>_tizen-beta.tpk` and its `.sha256`. The Samsung package ID is `BtrOrigin1` and the app ID is `BtrOrigin1.StremioOrigin`.

## Install in Developer Mode

1. Put the TV and the computer running Apps2Samsung on the same network, and find the computer's LAN IP address.
2. On the TV, open **Apps** and enter **12345** with the remote or its on-screen number pad. Turn **Developer Mode** on, enter the computer's IP address, and restart the TV. The Apps screen should now say **Develop Mode**.
3. Download the TPK and its `.sha256` from the [latest release](https://github.com/betterstremio/origin/releases/latest). To check the download, run `sha256sum -c` (or `shasum -a 256 -c`) on the `.sha256` file in the same folder. On Windows, `Get-FileHash -Algorithm SHA256 <TPK>` should print the first field of the `.sha256` file.
4. In [Apps2Samsung](https://github.com/Apps2Samsung/Apps2Samsung), select the TV and install the TPK as a custom package. Sign in to Samsung if it asks. Apps2Samsung signs the package for that TV.
5. Open **Origin** from Apps and sign in.

[Stremio Origin's Samsung install guide](https://github.com/stremio-origin/tizen/blob/fdd1e6a30dfc42f19c845fe9ae1d9b09beda2311/docs/install.md) · [Apps2Samsung help](https://github.com/Apps2Samsung/Apps2Samsung/wiki)

## Updates

The app tells you when a newer version is out (Settings > About), but you install it yourself. Download the new TPK and install it over Origin in Apps2Samsung with the same signing profile, so the author certificate stays the same. Leave **Uninstall installed version first** off. If Apps2Samsung reports an author certificate mismatch, go back to the profile you used the first time. Removing the app deletes its settings and sign-in.

## Build

`tools/release.sh build X.Y.Z` builds the TPK with the other packages. For the TPK alone, run `bash packaging/tizen/pack.sh --clean`; it writes the TPK and its `.sha256` to `dist/` (or `$OUT`).

You need Bash on Linux or WSL with Zig 0.17.0, CMake, Ninja, pkg-config, Python 3, `shasum`, `unzip` and the .NET 8 SDK (`DOTNET` if it is not on PATH). `TIZEN_SYSROOT` (default `$ORIGIN_BUILD/tizen/sysroot`) must point at an ARM soft-float sysroot with EGL, GLES2, curl and PulseAudio headers, an ARM `usr/lib/libGLESv2.so` and the PulseAudio pkg-config files. Put the [SDL 2.30.12](https://www.libsdl.org/release/SDL2-2.30.12.tar.gz) and [zlib 1.3.1](https://zlib.net/fossils/zlib-1.3.1.tar.gz) tarballs in `$ORIGIN_BUILD/src` (default `~/origin-build/src`); the build checks both hashes.

`pack.sh` builds the native library, the pinned torrent engine and four .NET hosts (Tizen 4 to 5.5, 6.0, 6.5, and 7 or later), and checks that the TPK holds the engine it built. At start-up a small picker chooses the host for the TV's Tizen version. The .NET SDK signs the package with its development certificate, which Apps2Samsung replaces on install.

## Checks on a TV

None of these has been run yet. When someone runs them, record the TV model, Tizen and firmware versions, the Apps2Samsung version, the build commit and the TPK's SHA-256.

| Check | Passes when |
| --- | --- |
| Install and cold start | It installs in Developer Mode and opens from Apps after the TV is switched off at the wall and on again. |
| Remote and sign-in | The arrows, OK and Back work. Sign-in reaches Home, and the session is still there after reopening the app. |
| Direct MP4 and HLS | Both test clips play with sound and moving picture, and seeking in the MP4 works. |
| Subtitles | The SRT on the MP4 and the embedded track on the torrent change at the right times. |
| Leave and come back | Going to the TV's Home during playback and back to Origin resumes at the same point, and the remote still works. |
| Update over an older build | A test build with a lower `VERSION` (such as `2.99.0`), the same package ID and the same signing profile keeps its settings and sign-in when the new TPK is installed over it. |
| Torrent play, seek and stop | The picture starts before the whole file has arrived, seeking both ways works, and after stopping, the torrent's files in the engine's logged cache folder are gone and peer traffic stops. |

For test media, run `bash tests/tizen/serve.sh <LAN IPv4>` on a Linux machine the TV can reach on TCP 8765 and 6881 (from WSL, make sure those ports are reachable from the TV). It needs FFmpeg with libx264 and the Python libtorrent module. Add the local addon URL it prints and open the **Origin TV tests** catalog. The addon serves a generated test pattern with a tone as MP4 and HLS, with subtitles, and seeds the same clip as a torrent at a limited rate. The seeder's byte counts and Origin's playback log show when the picture started and when the torrent was removed.
