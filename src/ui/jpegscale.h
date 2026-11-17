#ifndef SB_JPEGSCALE_H
#define SB_JPEGSCALE_H
#include <stddef.h>
/* A JPEG decoded at 1/1, 1/2, 1/4 or 1/8 of its size straight from the DCT, through the TV's own libjpeg-turbo (loaded at
   run time; only where the target says it has one). sc: the scale the picture must still cover (0..1], e.g. 0.42 for a
   1920x1080 shown at 800x450: it is decoded at the smallest 1/n that stays >= sc. RGBA, malloc'd; NULL when the library
   is missing, the data is not a JPEG or decoding fails (the caller falls back to stb). */
unsigned char *jpeg_scaled_rgba(const unsigned char *data, size_t len, float sc, int *w, int *h);
#endif
