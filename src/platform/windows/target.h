/* What the Windows package has and how it behaves. The shared code reads these instead of asking which OS it is on;
   tools/build.sh includes this file in every source file (after compat/compat.h, which fills in the POSIX calls). */
#ifndef SB_TARGET_H
#define SB_TARGET_H
#define SB_PKG_KIND "_windows_setup.exe"
#define SB_DL_SUFFIX ".dll"
#define SB_HAS_USB 0
#define SB_HAS_CAST 0 /* the receiver would listen on every network at start: a firewall prompt on first launch */
#define SB_HAS_DTS 0 /* mpv decodes DTS and TrueHD itself */
#define SB_GL_ES 1 /* GLES2 through ANGLE, on Direct3D 11 */
#define SB_PLAT_GL 1 /* the platform makes the GL surface (plat_gl_*): a composition layer above the video window */
#define SB_TV_PACER 1 /* the frame loop's own pacing, with the driver waiting for the screen (SB_VSYNC_DEFAULT) */
#define SB_VSYNC_DEFAULT 1 /* a desktop has no reason to keep the swap from waiting for the screen */
#define SB_WINDOWED 1 /* a desktop window: resized, fullscreen, minimised; a mouse and a keyboard */
#define SB_ADAPTIVE_LAYOUT 1 /* use the entire window without stretching the 16:9 design */
#define SB_STATIC_CURL 1
#define SB_CACHE_MB 400
#define SB_IMG_BUDGET_MB 96
#define SB_PLAT_INSTALLER 1 /* plat_install_start / plat_install_poll: the installer downloaded and run silently */
#define SB_CRASH_TO_SYSTEM 1 /* a crash is logged, then handed to the system (Windows Error Reporting) */
#define SB_CAPS_FROM_ENV 1 /* what the display and decoders can do arrives in SB_TVCAPS (plat_pre_init sets it) */
#define SB_P2P_DISK_ONLY \
  1 /* the P2P window in the data folder as 16 MB segment files (no hole punching here, no RAM-backed /tmp) */
#define SB_REMOTE_GUIDE 0 /* the first-start tour leaves out the remote's keys (user, 2026-11-10) */
#define SB_PHONE_TYPE 0 /* no "Type on your phone" code in Search: a keyboard types (user, 2026-11-11) */
#define SB_FONT_DIR "C:/Windows/Fonts"
#define SB_FONTS_EXACT 1
#define SB_FONT_TABLE "fonts_win.h" /* the exact list of system fonts to use, by file name */
#endif
