/* What the macOS package has and how it behaves. The shared code reads these instead of asking which OS it is on;
   tools/build.sh includes this file in every source file. Apple Silicon, macOS 12 or later. */
#ifndef SB_TARGET_H
#define SB_TARGET_H
#define SB_PKG_KIND "_macos-arm64.dmg"
#define SB_DL_SUFFIX ".dylib"
#define SB_HAS_USB 0
#define SB_HAS_CAST 0 /* the receiver would listen on every network at start: macOS asks for network access first */
#define SB_HAS_DTS 0 /* mpv decodes DTS and TrueHD itself */
#define SB_VIDEO_UNDER                                                                       \
  1 /* the video is a texture put under each finished frame, showing where it is see-through \
       (video_macos.c) */
#define SB_TV_PACER 1 /* the frame loop's own pacing */
#define SB_VSYNC_DEFAULT                                                                        \
  0 /* never the driver's: a swap that waits for the screen blocks for ever while the window is \
       hidden from it (another Space, covered, a hidden test window) */
#define SB_POINTER 1 /* a mouse or trackpad: rows hold still under it (board.c) */
#define SB_WINDOWED 1 /* a desktop window: resized, fullscreen, minimised; a mouse and a keyboard */
#define SB_ADAPTIVE_LAYOUT 1 /* use the entire window without stretching the 16:9 design */
#define SB_STATIC_CURL 1 /* the system's libcurl, linked */
#define SB_CACHE_MB 400
#define SB_IMG_BUDGET_MB 96
#define SB_MANUAL_UPDATE 1 /* an unsigned app cannot replace itself quietly: the new DMG opens in the browser */
#define SB_UPDATE_IN_BROWSER 1
#define SB_CRASH_TO_SYSTEM 1 /* a crash is logged, then handed to the system (its crash reporter) */
#define SB_CAPS_FROM_ENV 1 /* what the display and decoders can do arrives in SB_TVCAPS (plat_pre_init sets it) */
#define SB_P2P_DISK_ONLY 1 /* the P2P window in the data folder as segment files (no RAM-backed /tmp) */
#define SB_REMOTE_GUIDE 0 /* the first-start tour leaves out the remote's keys, as on Windows */
#define SB_PHONE_TYPE 0 /* no "Type on your phone" code in Search: a keyboard types */
#define SB_FONT_DIR "/System/Library/Fonts"
#define SB_FONTS_EXACT 1
#endif
