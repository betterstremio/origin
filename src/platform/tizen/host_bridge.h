/* Samsung: what the app sees of the .NET host (packaging/tizen/). Included in every file of the Samsung build.

   The host owns the window, the GL surface and the loop that draws; the app keeps its own loop on a thread of its own
   (host_bridge.c). SDL is ours (linked in, dummy video), so the few SDL calls that would make a window's GL context
   are answered here from the host's surface instead. */
#ifndef SB_HOST_BRIDGE_H
#define SB_HOST_BRIDGE_H

#ifdef SB_TPK
/* libraries opened at run time (the torrent engine, the sound converter): the system's loader first, else laid out by
   the app where a TV refuses to map code from the app's files (elfload.c); every dlopen / dlsym / dlerror goes here */
void *tpk_dlopen(const char *path, int flags);
void *tpk_dlsym(void *handle, const char *name);
char *tpk_dlerror(void);
#define dlopen tpk_dlopen
#define dlsym tpk_dlsym
#define dlerror tpk_dlerror

#include <SDL2/SDL.h>

/* the package's res/ folder, where the libraries opened at run time are (not the .NET runtime's folder) */
char *sbh_base_path(void);
#define SDL_GetBasePath sbh_base_path

/* the host's GL surface, as SDL's window calls would give it */
void *sbh_context(void);                    /* the host's context (made current on the app's thread for each frame) */
void  sbh_present(void);                    /* the frame is drawn: the host takes the surface back and shows it */
void  sbh_drawable(int *w, int *h);         /* the surface's size in pixels */
int   sbh_attribute(SDL_GLattr a, int *v);  /* the surface's real colour, depth and stencil bits */
void *sbh_native_window(void);              /* the host's native window (the player's picture goes under it), or NULL */
void  sbh_trace(const char *stage, const char *detail); /* a line of the start trace (data/start-trace.log) */
void  video_pause_now(void);                /* video_tizen.c: pause at once, from the host's thread */

#define SDL_CreateWindow(t, x, y, w, h, f) SDL_CreateWindow((t), (x), (y), (w), (h), 0) /* (a plain dummy window: no GL of SDL's own) */
#define SDL_GL_CreateContext(w)            ((void)(w), (SDL_GLContext)sbh_context())
#define SDL_GL_DeleteContext(c)            ((void)(c))
#define SDL_GL_GetCurrentContext()         ((SDL_GLContext)sbh_context())
#define SDL_GL_SwapWindow(w)               ((void)(w), sbh_present())
#define SDL_GL_SetSwapInterval(i)          ((void)(i), 0) /* (the host's loop keeps the display's pace) */
#define SDL_GL_SetAttribute(a, v)          ((void)(a), (void)(v), 0)
#define SDL_GL_GetAttribute(a, v)          sbh_attribute((a), (v))
#define SDL_GL_GetDrawableSize(w, a, b)    ((void)(w), sbh_drawable((a), (b)))
#define SDL_GL_GetCurrentWindow()          ((SDL_Window *)NULL)
#endif /* SB_TPK */

#endif /* SB_HOST_BRIDGE_H */
