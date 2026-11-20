# Building

One codebase serves every platform, and each package carries only its own platform's code. `src/platform/<target>/target.h` says what a package has, and `tools/targets/<target>.sh` how it is compiled. Everything builds in Linux or WSL except the macOS package, which is made on a Mac; `tools/paths.sh` says where the toolchains, keys and build folders are.

```
tools/build.sh desktop                         # a development build for Linux, WSL or a Mac
tools/build.sh webos                           # the webOS app (needs the webOS toolchain)
packaging/webos/pack.sh                        # the webOS IPK

tools/android/build-deps.sh                    # Android: SDL2, Mbed TLS, curl for each ABI
tools/build-engine.sh android-arm64-v8a        # the P2P engine (also android-armeabi-v7a, android-x86)
tools/android/build-apk.sh                     # the release APKs: one per CPU type, and one with both
tools/android/build-apk.sh --dev               # a test build with x86 for the emulator

tools/windows/build-deps.sh                    # Windows: SDL2, curl with Schannel, ANGLE, libmpv, the Vulkan loader
tools/build-engine.sh windows                  # the P2P engine as libsbengine.dll
tools/build.sh windows                         # Origin.exe (mingw-w64)
packaging/windows/pack.sh                      # the Windows setup (NSIS) and portable zip
tools/macos/build-deps.sh                      # macOS, on a Mac: SDL2.framework, libmpv, OpenSSL for the engine
tools/build-engine.sh mac                      # the P2P engine as libsbengine.dylib
tools/build.sh macos                           # the Origin binary (Apple Silicon, macOS 11 or later)
packaging/macos/pack.sh                        # Origin.app in a DMG, signed ad hoc

tools/release.sh build X.Y.Z                   # every package from empty folders, into dist/X.Y.Z
tools/release.sh mac X.Y.Z                     # on a Mac, the same commit: the DMG, then copied into that dist/X.Y.Z
```

Android tests on the emulator: `python tools/android/test/run.py --build` (after `tools/android/test/media.sh`). The script's header says what it needs.

Code style: the C code follows `.clang-format` at the repo root, with clang-format 19.1.7 (`pip install clang-format==19.1.7`). Run `tools/format.sh` before you commit; `tools/format.sh --check` lists the files that still need it. `third_party/` keeps its upstream style.

Samsung: see [the Samsung guide](tizen.md#build).
