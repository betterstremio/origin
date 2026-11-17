/* Pictures: fetched and decoded on worker threads (disk cache in between), scaled there to the
   size they are shown at, uploaded on the main thread, kept under a memory budget. Screens ask
   for a texture every frame; 0 means "not yet" (and starts the load). */
#ifndef SB_IMG_H
#define SB_IMG_H
#include <stddef.h>
#include "ui/gl.h"
#include "ui/palette.h"

int img_init(const char *cacheDir);
/* w,h = the size the picture is shown at (layout units); the decode scales to cover it. Returns the
   texture or 0. Also reports the texture's own aspect so the caller can cover-crop. */
GLuint img_get(const char *url, int w, int h, float *aspect);
/* the same picture made to fit the box exactly (fit 0: stretched to it, 1: cover-cropped at the
   centre, 2: cover-cropped from the top) with its corners rounded by `radius` (layout units), so it
   draws with the plain image program. A variant of its own; shares the download with img_get. */
GLuint img_get_shaped(const char *url, int w, int h, float radius, int fit);
int img_shaped_loaded(const char *url, int w, int h, float radius, int fit);
int img_shaped_failed(const char *url, int w, int h, float radius, int fit);
/* fit 3: a poster card cut from a wide picture, at its subject (Netflix's way): the window of the picture's 16:9
   middle. Where the subject is (0..1 across that middle; -1 until the crop is in), and where a window cw wide starts in
   a picture rw wide for it: the open card puts its 16:9 picture so that the crop's part of it stays where it was */
float img_shaped_focal(const char *url, int w, int h, float radius, int fit);
float img_crop_left(float focal, float rw, float cw);
int img_back_shaped(const char *url, int w, int h, float radius,
                    int fit); /* 1: shown before, or its small copy is on disk: ask it at once, it comes back in one step */
int img_idle(void); /* no picture waits to load */
GLuint img_get_sized(const char *url, int w, int h, float *aspect); /* its own copy at this size */
void img_drop_shaped(const char *url, int w, int h, float radius, int fit); /* its texture freed now (not wanted) */
int img_palette(const char *url, int w, int h, int alpha, Palette *out); /* 1 ready, 0 loading, -1 failed */
int img_failed(const char *url); /* 1 when the picture could not be loaded */
int img_loaded(const char *url); /* 1 when ready */
const char *img_src_of(GLuint tex); /* the address a plain picture was loaded from, or NULL */
void img_urgent_shaped(const char *url, int w, int h, float radius,
                       int fit); /* the picture the screen waits on, ahead of the queue */
int img_has_room(void);
/* a picture wanted soon (the next cards of the row being scrolled): ahead of the plain queue; ask it every frame */
void img_ahead_shaped(const char *url, int w, int h, float radius, int fit);
void img_prefetch(const char *url, int w, int h);
int img_budget_mb(void); /* the picture memory allowed (small on most TVs) */
void img_urgent(const char *url, int w, int h); /* the next one decoded (the backdrop waited on) */
void img_prefetch_mode(int on); /* 1 while a look-ahead asks: what it asks for is not on screen (loaded after what is) */
void img_prefetch_pri(const char *url, int w, int h); /* ahead of the queue, and never evicted (title art) */
void img_pump(void); /* uploads finished decodes; call per frame (bounded work) */
void img_frame(void); /* marks the frame (for eviction) */
void img_ctx_row(int row); /* the Home row being drawn (-1 none): what it asks for is evicted by distance from the focus */
void img_focus_row(int row); /* the focused row, each frame the board draws */
void img_pin_begin(void); /* the first screen's pictures are pinned afresh: begin, then img_pin_shaped each; the rest is let go */
void img_pin_shaped(const char *url, int w, int h, float radius, int fit); /* (a loaded one only; with the budget's third) */
void img_release_unused(int keepMb); /* evicts least recently drawn textures over the budget */
void img_set_budget_mb(int mb);
void img_set_draw_scale(
    float s); /* surface pixels per layout unit when below 1: pictures are decoded at the size really drawn */
void img_set_draw_scale_up(float s); /* the same above 1 too (a phone's dp canvas): pictures as sharp as its pixels */
int img_used_mb(void);
typedef struct {
  unsigned ev[5]; /* development, per fill-test phase: textures evicted by cause (1 budget, 2 guard, 3 queue drop, 4 upload skipped) */
  unsigned ups, upFrames;
  int upP95KB, picPeakMB, dnPeakMB; /* (dn: decoded pixels waiting for upload) */
} ImgPhase;
void img_phase_reset(void);
void img_phase_get(ImgPhase *o);
int img_why_shaped(const char *url, int w, int h, float radius, int fit); /* development: stage | lost<<4 (see img.c) */
void img_stats(char *out, size_t cap); /* development: evictions and returns so far, for the memory line */
/* computes cover-crop uv for drawing a texture of aspect `ta` into a box of aspect `ba`, anchored at the top (posters)
 * or centre */
void img_cover_uv(float ta, float ba, int anchorTop, float *u0, float *v0, float *u1, float *v1);
#endif
void img_queue_stats(int *queued, int *waiting); /* development: decodes queued, decoded pictures waiting for upload */
