/* What the development build has and how it behaves. The shared code reads these instead of asking which OS it is on;
   tools/build.sh includes this file in every source file. */
#ifndef SB_TARGET_H
#define SB_TARGET_H
#define SB_PKG_KIND "_arm.ipk"
#ifdef __APPLE__
#define SB_DL_SUFFIX ".dylib"
#else
#define SB_DL_SUFFIX ".so"
#endif
#define SB_HAS_USB 1
#define SB_HAS_CAST 1
#define SB_HAS_DTS 1
#define SB_PREVIEW 1 /* scripted runs: a hidden window, backtraces, the network port on this machine only */
#define SB_STATIC_CURL 1 /* curl is linked, not looked up at run time */
#define SB_CACHE_MB 400
#define SB_IMG_BUDGET_MB 96
#define SB_FONT_DIR "/System/Library/Fonts"
#define SB_FONTS_EXACT 1
#ifndef SB_ADAPTIVE_LAYOUT
#define SB_ADAPTIVE_LAYOUT 1 /* the preview window's shape is the canvas's; SB_FORM=phone previews the phone layout */
#endif
#endif
