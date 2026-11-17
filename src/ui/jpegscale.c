#include "ui/jpegscale.h"
#ifndef SB_SYS_JPEG
unsigned char *jpeg_scaled_rgba(const unsigned char *data, size_t len, float sc, int *w, int *h) {
  (void)data;
  (void)len;
  (void)sc;
  (void)w;
  (void)h;
  return NULL;
}
#else
#include <dlfcn.h>
#include <setjmp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/jpeg62/jpeglib.h" /* the TV's libjpeg.so.62 (libjpeg-turbo, layout 6b): a wrong header gives "Wrong JPEG library version" */
typedef struct jpeg_error_mgr *(*FnStdError)(struct jpeg_error_mgr *);
typedef void (*FnCreate)(j_decompress_ptr, int, size_t);
typedef void (*FnStdioSrc)(j_decompress_ptr, FILE *);
typedef int (*FnReadHeader)(j_decompress_ptr, boolean);
typedef boolean (*FnStart)(j_decompress_ptr);
typedef JDIMENSION (*FnScanlines)(j_decompress_ptr, JSAMPARRAY, JDIMENSION);
typedef boolean (*FnFinish)(j_decompress_ptr);
typedef void (*FnDestroy)(j_decompress_ptr);
typedef void (*FnCalc)(j_decompress_ptr);
static struct {
  FnStdError stdError;
  FnCreate create;
  FnStdioSrc stdioSrc;
  FnReadHeader readHeader;
  FnStart start;
  FnScanlines scanlines;
  FnFinish finish;
  FnDestroy destroy;
  FnCalc calc;
  int ok;
} L;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void open_lib(void) {
  void *h = dlopen("libjpeg.so.62", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    h = dlopen("/usr/lib/libjpeg.so.62", RTLD_NOW | RTLD_LOCAL);
  if (h) {
    L.stdError = (FnStdError)dlsym(h, "jpeg_std_error");
    L.create = (FnCreate)dlsym(h, "jpeg_CreateDecompress");
    L.stdioSrc = (FnStdioSrc)dlsym(h, "jpeg_stdio_src");
    L.readHeader = (FnReadHeader)dlsym(h, "jpeg_read_header");
    L.start = (FnStart)dlsym(h, "jpeg_start_decompress");
    L.scanlines = (FnScanlines)dlsym(h, "jpeg_read_scanlines");
    L.finish = (FnFinish)dlsym(h, "jpeg_finish_decompress");
    L.destroy = (FnDestroy)dlsym(h, "jpeg_destroy_decompress");
    L.calc = (FnCalc)dlsym(h, "jpeg_calc_output_dimensions");
    L.ok = L.stdError && L.create && L.stdioSrc && L.readHeader && L.start && L.scanlines && L.finish && L.destroy && L.calc;
  }
  fprintf(stderr, "[img] jpeg: %s\n", L.ok ? "the system's, scaled" : "built in");
}
typedef struct {
  struct jpeg_error_mgr pub;
  jmp_buf jump;
} ErrJump;
static void err_exit(j_common_ptr c) {
  longjmp(((ErrJump *)c->err)->jump, 1);
}
static void err_quiet(j_common_ptr c) {
  (void)c;
}
unsigned char *jpeg_scaled_rgba(const unsigned char *data, size_t len, float sc, int *w, int *h) {
  if (len < 16 || data[0] != 0xFF || data[1] != 0xD8)
    return NULL;
  pthread_once(&once, open_lib);
  if (!L.ok)
    return NULL;
  FILE *f = fmemopen((void *)data, len, "rb");
  if (!f)
    return NULL;
  struct jpeg_decompress_struct ci;
  ErrJump ej;
  unsigned char *volatile px = NULL;
  memset(&ci, 0, sizeof ci);
  ci.err = L.stdError(&ej.pub);
  ej.pub.error_exit = err_exit;
  ej.pub.output_message = err_quiet;
  if (setjmp(ej.jump)) {
    L.destroy(&ci);
    fclose(f);
    free(px);
    return NULL;
  }
  L.create(&ci, JPEG_LIB_VERSION, sizeof ci);
  L.stdioSrc(&ci, f);
  L.readHeader(&ci, TRUE);
  ci.scale_num = 1;
  ci.scale_denom = 1;
  if (sc < 0.01f)
    sc = 0.01f;
  while (ci.scale_denom < 8 && 1.0f / (float)(ci.scale_denom * 2) >= sc &&
         (ci.image_width + ci.scale_denom * 2 - 1) / (ci.scale_denom * 2) >= 1)
    ci.scale_denom *= 2; /* (the smallest picture that still covers what is shown) */
  ci.out_color_space = JCS_EXT_RGBA;
  if (ci.scale_denom > 1) {
    ci.dct_method = JDCT_IFAST;
    ci.do_fancy_upsampling = FALSE;
  }
  L.calc(&ci);
  px = malloc((size_t)ci.output_width * ci.output_height * 4);
  if (!px) {
    L.destroy(&ci);
    fclose(f);
    return NULL;
  }
  L.start(&ci);
  if (ci.output_components != 4) {
    L.destroy(&ci);
    fclose(f);
    free(px);
    return NULL;
  }
  while (ci.output_scanline < ci.output_height) {
    JSAMPROW row = (JSAMPROW)(px + (size_t)ci.output_scanline * ci.output_width * 4);
    L.scanlines(&ci, &row, 1);
  }
  *w = (int)ci.output_width;
  *h = (int)ci.output_height;
  L.finish(&ci);
  L.destroy(&ci);
  fclose(f);
  return px;
}
#endif
