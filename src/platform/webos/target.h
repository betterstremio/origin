/* What the webOS package has and how it behaves. The shared code reads these instead of asking which OS it is on;
   tools/build.sh includes this file in every source file. */
#ifndef SB_TARGET_H
#define SB_TARGET_H
#define SB_PKG_KIND "_arm.ipk"
#define SB_DL_SUFFIX ".so"
#define SB_HAS_USB 1 /* an optional USB drive for the P2P cache */
#define SB_HAS_CAST 1 /* the cast receiver */
#define SB_HAS_DTS 1 /* DTS and TrueHD converted on the device (libsbdts) */
#define SB_GL_ES 1
#define SB_TV_PACER 1 /* the loop paces itself and finds the panel's refresh rate */
#define SB_CACHE_MB 40 /* the app partition is small and shared with every app's install */
#define SB_IMG_BUDGET_MB 40 /* the TVs have little memory to spare */
#define SB_VTX_CLIENT_ARRAYS 1 /* the driver maps new memory for every buffer update */
#define SB_SDL_QUEUE_BY_NAME 1 /* older TVs have an SDL without the audio queue */
#define SB_720_FULLSCREEN 1 /* 720p drawing goes to a fullscreen window, which the TV scales */
#endif
