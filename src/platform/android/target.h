/* What the Android package has and how it behaves. The shared code reads these instead of asking which OS it is on;
   tools/build.sh includes this file in every source file. */
#ifndef SB_TARGET_H
#define SB_TARGET_H
#if defined(__aarch64__)
#define SB_PKG_KIND "_android-arm64.apk"
#elif defined(__arm__)
#define SB_PKG_KIND "_android-armv7.apk"
#elif defined(__x86_64__)
#define SB_PKG_KIND "_android-x86_64.apk"
#else
#define SB_PKG_KIND "_android-x86.apk"
#endif
#define SB_DL_SUFFIX ".so"
#define SB_HAS_USB 0
#define SB_HAS_CAST 1
#define SB_HAS_DTS 0
#define SB_GL_ES 1
#define SB_TV_PACER 1
#define SB_CACHE_MB 400
#define SB_IMG_BUDGET_MB 96
#define SB_PLAT_INSTALLER 1      /* plat_install_start / plat_install_poll: the system's package installer */
#define SB_CRASH_TO_SYSTEM 1     /* a crash is logged, then handed to the system, which writes its own report */
#define SB_BEST_ON_BY_DEFAULT 1  /* the best stream is chosen automatically until switched off */
#define SB_CAPS_FROM_ENV 1       /* what the display and decoders can do arrives in SB_TVCAPS at start */
#define SB_P2P_LIB_ENV 1         /* the P2P engine is in the folder SB_LIBDIR names */
#define SB_REMOTE_GUIDE 0        /* the first-start tour leaves out the remote's keys (user, 2026-11-10) */
#define SB_FONT_DIR "/system/fonts"
#define SB_FONTS_EXACT 0
#define SB_FONT_SKIP_STYLES 1    /* about 250 files: weights and styles are left out, one face per script stays */
#endif
