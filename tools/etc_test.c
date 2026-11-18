/* Self test of src/ui/etc.c (not part of the app build): encodes pictures, decodes the blocks with a reference decoder
   written from the Khronos spec, prints PSNR and encode time.
     gcc -O2 -std=c11 -I src -I third_party/stb tools/etc_test.c src/ui/etc.c -lm -o /tmp/etc_test
     /tmp/etc_test picture.png picture.jpg ...
   Each picture is tested as it is (ETC1 on the colours, alpha ignored); a second run has rounded corners put into its
   alpha (ETC2 + EAC: PSNR of the colours where the alpha is above 0, and of the alpha). */
#include "ui/etc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static const int ITAB[8][2] = { { 2, 8 },   { 5, 17 },  { 9, 29 },   { 13, 42 },
                                { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 } };
static const int ATAB[16][8] = {
  { -3, -6, -9, -15, 2, 5, 8, 14 }, { -3, -7, -10, -13, 2, 6, 9, 12 }, { -2, -5, -8, -13, 1, 4, 7, 12 },
  { -2, -4, -6, -13, 1, 3, 5, 12 }, { -3, -6, -8, -12, 2, 5, 7, 11 },  { -3, -7, -9, -11, 2, 6, 8, 10 },
  { -4, -7, -8, -11, 3, 6, 7, 10 }, { -3, -5, -8, -11, 2, 4, 7, 10 },  { -2, -6, -8, -10, 1, 5, 7, 9 },
  { -2, -5, -8, -10, 1, 4, 7, 9 },  { -2, -4, -8, -10, 1, 3, 7, 9 },   { -2, -5, -7, -10, 1, 4, 6, 9 },
  { -3, -4, -7, -10, 2, 3, 6, 9 },  { -1, -2, -3, -10, 0, 1, 2, 9 },   { -4, -6, -8, -9, 3, 5, 7, 8 },
  { -3, -5, -7, -9, 2, 4, 6, 8 },
};
static int cl(int v) {
  return v < 0 ? 0 : v > 255 ? 255 : v;
}
static void dec_etc1(const unsigned char *b, unsigned char *px /* 16 x RGB, y * 4 + x */) {
  unsigned long long v = 0;
  for (int i = 0; i < 8; i++)
    v = (v << 8) | b[i];
  int flip = v >> 32 & 1, diff = v >> 33 & 1, t2 = v >> 34 & 7, t1 = v >> 37 & 7;
  int c1[3], c2[3];
  for (int c = 0; c < 3; c++) { /* R at bits 63..56, G 55..48, B 47..40 */
    int sh = 56 - 8 * c, byte = (int)(v >> sh & 255);
    if (diff) {
      int base = byte >> 3, d = byte & 7;
      if (d > 3)
        d -= 8;
      int o = base + d;
      c1[c] = (base << 3) | (base >> 2);
      c2[c] = (o << 3) | (o >> 2);
    } else {
      c1[c] = (byte >> 4) * 17;
      c2[c] = (byte & 15) * 17;
    }
  }
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++) {
      int k = x * 4 + y; /* pixel index, column major */
      int msb = v >> (16 + k) & 1, lsb = v >> k & 1;
      int sub2 = flip ? y >= 2 : x >= 2;
      int t = sub2 ? t2 : t1;
      int sel = msb << 1 | lsb; /* 0 +small 1 +big 2 -small 3 -big */
      int m = sel == 0 ? ITAB[t][0] : sel == 1 ? ITAB[t][1] : sel == 2 ? -ITAB[t][0] : -ITAB[t][1];
      const int *c = sub2 ? c2 : c1;
      for (int ch = 0; ch < 3; ch++)
        px[(y * 4 + x) * 3 + ch] = (unsigned char)cl(c[ch] + m);
    }
}
static void dec_eac(const unsigned char *b, unsigned char *a) {
  int base = b[0], mult = b[1] >> 4, tab = b[1] & 15;
  unsigned long long v = 0;
  for (int i = 2; i < 8; i++)
    v = (v << 8) | b[i];
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++) {
      int k = x * 4 + y, idx = (int)(v >> (45 - 3 * k) & 7);
      a[y * 4 + x] = (unsigned char)cl(base + mult * ATAB[tab][idx]);
    }
}
static void decode(const unsigned char *blk, int w, int h, int alpha, unsigned char *rgba) {
  int bw = (w + 3) / 4, bh = (h + 3) / 4;
  for (int by = 0; by < bh; by++)
    for (int bx = 0; bx < bw; bx++) {
      const unsigned char *b = blk + ((size_t)by * bw + bx) * (alpha ? 16 : 8);
      unsigned char c[48], a[16];
      if (alpha)
        dec_eac(b, a);
      dec_etc1(b + (alpha ? 8 : 0), c);
      for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
          int xx = bx * 4 + x, yy = by * 4 + y;
          if (xx >= w || yy >= h)
            continue;
          unsigned char *o = rgba + ((size_t)yy * w + xx) * 4;
          memcpy(o, c + (y * 4 + x) * 3, 3);
          o[3] = alpha ? a[y * 4 + x] : 255;
        }
    }
}
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static void corners(unsigned char *px, int w, int h, float r) { /* as img.c's round_corners */
  int n = (int)ceilf(r) + 1;
  for (int y = 0; y < n && y < h; y++)
    for (int x = 0; x < n && x < w; x++) {
      float dx = r - (x + 0.5f), dy = r - (y + 0.5f);
      if (dx < 0 || dy < 0)
        continue;
      float d = sqrtf(dx * dx + dy * dy) - r, k = 0.5f - d;
      if (k >= 1)
        continue;
      if (k < 0)
        k = 0;
      int xs[2] = { x, w - 1 - x }, ys[2] = { y, h - 1 - y };
      for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
          unsigned char *p = px + ((size_t)ys[j] * w + xs[i]) * 4;
          p[3] = (unsigned char)(p[3] * k + 0.5f);
        }
    }
}
int main(int argc, char **argv) {
  double sumPsnr = 0, sumMs = 0, sumMp = 0;
  int n = 0;
  for (int i = 1; i < argc; i++) {
    int w, h, c;
    unsigned char *px = stbi_load(argv[i], &w, &h, &c, 4);
    if (!px) {
      printf("%s: cannot load\n", argv[i]);
      continue;
    }
    int trimW = w - (i % 3), trimH = h - (i % 2) * 2; /* sizes that are not multiples of 4 as well */
    for (int pass = 0; pass < 2; pass++) {
      int ww = pass ? trimW : w, hh = pass ? trimH : h;
      unsigned char *src = malloc((size_t)ww * hh * 4);
      for (int y = 0; y < hh; y++)
        memcpy(src + (size_t)y * ww * 4, px + (size_t)y * w * 4, (size_t)ww * 4);
      int alpha = pass;
      if (alpha)
        corners(src, ww, hh, 20);
      else
        for (long k = 3; k < (long)ww * hh * 4; k += 4)
          src[k] = 255;
      size_t sz = etc_size(ww, hh, alpha);
      unsigned char *blk = malloc(sz), *dec = malloc((size_t)ww * hh * 4);
      double t0 = now();
      int reps = 3;
      for (int r = 0; r < reps; r++)
        alpha ? etc2_eac_encode(src, ww, hh, blk) : etc1_encode(src, ww, hh, blk);
      double ms = (now() - t0) / reps;
      decode(blk, ww, hh, alpha, dec);
      double se = 0, sa = 0;
      long nc = 0;
      for (long k = 0; k < (long)ww * hh; k++) {
        const unsigned char *a = src + k * 4, *b = dec + k * 4;
        if (a[3] > 0 || !alpha) {
          for (int ch = 0; ch < 3; ch++)
            se += (double)(a[ch] - b[ch]) * (a[ch] - b[ch]);
          nc += 3;
        }
        sa += (double)(a[3] - b[3]) * (a[3] - b[3]);
      }
      double psnr = se > 0 ? 10 * log10(255.0 * 255.0 / (se / nc)) : 99;
      double mp = (double)ww * hh / 1e6;
      printf("%-28s %4dx%-4d %s  %6.2f dB colour",
             argv[i] + (strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 - argv[i] : 0), ww, hh,
             alpha ? "ETC2+EAC" : "ETC1    ", psnr);
      if (alpha)
        printf(", alpha %6.2f dB", sa > 0 ? 10 * log10(255.0 * 255.0 / (sa / ((double)ww * hh))) : 99.0);
      printf("  %7.2f ms  %6.1f ms/MP  %zu bytes\n", ms, ms / mp, sz);
      if (!alpha) {
        sumPsnr += psnr;
        sumMs += ms;
        sumMp += mp;
        n++;
      }
      free(src);
      free(blk);
      free(dec);
    }
    stbi_image_free(px);
  }
  if (n)
    printf("ETC1 mean PSNR %.2f dB, %.1f ms/MP over %d pictures\n", sumPsnr / n, sumMs / sumMp, n);
  return 0;
}
