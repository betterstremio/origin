#include "ui/etc.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>

/* Fast ETC1 encoder: per 4x4 block, both orientations, per half the average colour in the differential and in the
   individual mode, the best of the 8 intensity tables with the best index per pixel; the lowest squared error wins.
   Plain RGB error: a table's modifier moves all three channels alike, so a pixel's best index is the modifier closest
   to the pixel's mean difference (no search); only a base near black or white, where the sum clamps, is tried one by
   one. Plain C99. */
size_t etc_size(int w, int h, int alpha) {
  if (w <= 0 || h <= 0)
    return 0;
  return (size_t)((w + 3) / 4) * (size_t)((h + 3) / 4) * (alpha ? 16u : 8u);
}

static const int T1[8][2] = { { 2, 8 },   { 5, 17 },  { 9, 29 },   { 13, 42 },
                              { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 } };

typedef struct {
  int r[16], g[16], b[16], a[16]; /* pixel p = y * 4 + x */
} Blk;

/* the 8 pixels of each half: [flip][half][i] */
static const unsigned char SUBP[2][2][8] = {
  { { 0, 4, 8, 12, 1, 5, 9, 13 }, { 2, 6, 10, 14, 3, 7, 11, 15 } }, /* flip 0: left and right (2 wide, 4 high) */
  { { 0, 1, 2, 3, 4, 5, 6, 7 }, { 8, 9, 10, 11, 12, 13, 14, 15 } }, /* flip 1: top and bottom */
};

static int clamp255(int v) {
  return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* the table and each pixel's index (0 +small, 1 +big, 2 -small, 3 -big, the bits ETC1 stores) for the half pl with the
   base colour (br, bg, bb): the table whose big step best matches the half's spread, and its two neighbours, tried */
static void fit_half(const Blk *k, const unsigned char *pl, int br, int bg, int bb, int *tbl, unsigned char *sel) {
  int S[8], mx = 0, d2 = 0;
  for (int i = 0; i < 8; i++) {
    int p = pl[i], dr = k->r[p] - br, dg = k->g[p] - bg, db = k->b[p] - bb;
    d2 += dr * dr + dg * dg + db * db;
    S[i] = dr + dg + db;
    int a = S[i] < 0 ? -S[i] : S[i];
    if (a > mx)
      mx = a;
  }
  int want = mx / 3, t0 = 0; /* (a modifier moves all three channels: a pixel's offset is S / 3) */
  while (t0 < 7 && T1[t0][1] + T1[t0 + 1][1] < 2 * want)
    t0++;
  int lo = br < bg ? (br < bb ? br : bb) : (bg < bb ? bg : bb);
  int hi = br > bg ? (br > bb ? br : bb) : (bg > bb ? bg : bb);
  int best = INT_MAX;
  for (int t = t0 > 0 ? t0 - 1 : 0; t <= (t0 < 7 ? t0 + 1 : 7); t++) {
    int a = T1[t][0], b = T1[t][1], e = 0;
    unsigned char s[8];
    if (lo - b >= 0 && hi + b <= 255) { /* nothing clamps: error = |d|^2 + 3m^2 - 2mS */
      e = d2;
      for (int i = 0; i < 8; i++) {
        int Si = S[i], m, c;
        if (2 * Si < -3 * (a + b)) {
          c = 3;
          m = -b;
        } else if (Si < 0) {
          c = 2;
          m = -a;
        } else if (2 * Si < 3 * (a + b)) {
          c = 0;
          m = a;
        } else {
          c = 1;
          m = b;
        }
        s[i] = (unsigned char)c;
        e += 3 * m * m - 2 * m * Si;
      }
    } else { /* a channel clamps at 0 or 255: each of the 4 tried */
      const int M[4] = { a, b, -a, -b };
      for (int i = 0; i < 8; i++) {
        int be = INT_MAX, bc = 0, p = pl[i];
        for (int c = 0; c < 4; c++) {
          int x = clamp255(br + M[c]) - k->r[p], y = clamp255(bg + M[c]) - k->g[p], z = clamp255(bb + M[c]) - k->b[p];
          int er = x * x + y * y + z * z;
          if (er < be) {
            be = er;
            bc = c;
          }
        }
        s[i] = (unsigned char)bc;
        e += be;
      }
    }
    if (e < best) {
      best = e;
      *tbl = t;
      memcpy(sel, s, 8);
    }
  }
}

/* one orientation, one mode, one table a half: the orientation with the less variance inside its halves, the
   differential mode when the halves' colours are close enough, individual when not */
static void enc_color(const Blk *k, unsigned char *o) {
  int s[4][3] = { { 0 } }, ss[4][3] = { { 0 } };
  for (int p = 0; p < 16; p++) {
    int q = (p >> 3) * 2 + ((p & 3) >> 1);
    int v[3] = { k->r[p], k->g[p], k->b[p] };
    for (int c = 0; c < 3; c++) {
      s[q][c] += v[c];
      ss[q][c] += v[c] * v[c];
    }
  }
  /* flip 0: left (quadrants 0, 2) and right (1, 3); flip 1: top (0, 1) and bottom (2, 3) */
  static const unsigned char Q[2][2][2] = { { { 0, 2 }, { 1, 3 } }, { { 0, 1 }, { 2, 3 } } };
  int hs[2][2][3], cost[2] = { 0, 0 };
  for (int f = 0; f < 2; f++)
    for (int h = 0; h < 2; h++)
      for (int c = 0; c < 3; c++) {
        int sum = s[Q[f][h][0]][c] + s[Q[f][h][1]][c], sq = ss[Q[f][h][0]][c] + ss[Q[f][h][1]][c];
        hs[f][h][c] = sum;
        cost[f] += sq - (sum * sum >> 3);
      }
  int flip = cost[1] < cost[0];
  int q0[3], q1[3], diff = 1;
  for (int c = 0; c < 3; c++) {
    int m0 = (hs[flip][0][c] + 4) >> 3, m1 = (hs[flip][1][c] + 4) >> 3;
    q0[c] = (m0 * 31 + 127) / 255;
    q1[c] = (m1 * 31 + 127) / 255;
    if (q1[c] - q0[c] < -4 || q1[c] - q0[c] > 3)
      diff = 0;
  }
  if (!diff)
    for (int c = 0; c < 3; c++) {
      q0[c] = (((hs[flip][0][c] + 4) >> 3) * 15 + 127) / 255;
      q1[c] = (((hs[flip][1][c] + 4) >> 3) * 15 + 127) / 255;
    }
  int tb[2];
  unsigned char se[2][8];
  for (int h = 0; h < 2; h++) {
    const int *q = h ? q1 : q0;
    int c8[3];
    for (int c = 0; c < 3; c++)
      c8[c] = diff ? (q[c] << 3) | (q[c] >> 2) : q[c] * 17;
    fit_half(k, SUBP[flip][h], c8[0], c8[1], c8[2], &tb[h], se[h]);
  }
  if (diff) {
    for (int c = 0; c < 3; c++)
      o[c] = (unsigned char)((q0[c] << 3) | ((q1[c] - q0[c]) & 7));
  } else {
    for (int c = 0; c < 3; c++)
      o[c] = (unsigned char)((q0[c] << 4) | q1[c]);
  }
  o[3] = (unsigned char)((tb[0] << 5) | (tb[1] << 2) | (diff << 1) | flip);
  unsigned msb = 0, lsb = 0;
  for (int h = 0; h < 2; h++)
    for (int i = 0; i < 8; i++) {
      int p = SUBP[flip][h][i], bit = (p & 3) * 4 + (p >> 2); /* stored by column: x * 4 + y */
      msb |= (unsigned)(se[h][i] >> 1) << bit;
      lsb |= (unsigned)(se[h][i] & 1) << bit;
    }
  o[4] = (unsigned char)(msb >> 8);
  o[5] = (unsigned char)msb;
  o[6] = (unsigned char)(lsb >> 8);
  o[7] = (unsigned char)lsb;
}

/* EAC alpha: base, multiplier, one of 16 modifier tables, 3 bits a pixel */
static const signed char EA[16][8] = {
  { -3, -6, -9, -15, 2, 5, 8, 14 }, { -3, -7, -10, -13, 2, 6, 9, 12 }, { -2, -5, -8, -13, 1, 4, 7, 12 },
  { -2, -4, -6, -13, 1, 3, 5, 12 }, { -3, -6, -8, -12, 2, 5, 7, 11 },  { -3, -7, -9, -11, 2, 6, 8, 10 },
  { -4, -7, -8, -11, 3, 6, 7, 10 }, { -3, -5, -8, -11, 2, 4, 7, 10 },  { -2, -6, -8, -10, 1, 5, 7, 9 },
  { -2, -5, -8, -10, 1, 4, 7, 9 },  { -2, -4, -8, -10, 1, 3, 7, 9 },   { -2, -5, -7, -10, 1, 4, 6, 9 },
  { -3, -4, -7, -10, 2, 3, 6, 9 },  { -1, -2, -3, -10, 0, 1, 2, 9 },   { -4, -6, -8, -9, 3, 5, 7, 8 },
  { -3, -5, -7, -9, 2, 4, 6, 8 },
};
/* table 13, index 4 (modifier 0) in all 16 pixels: the block is exactly its base */
static void alpha_flat(unsigned char *o, int a) {
  o[0] = (unsigned char)a;
  o[1] = 0x1d;
  o[2] = 0x92;
  o[3] = 0x49;
  o[4] = 0x24;
  o[5] = 0x92;
  o[6] = 0x49;
  o[7] = 0x24;
}
static void enc_alpha(const Blk *k, unsigned char *o) {
  int mn = 255, mx = 0;
  for (int p = 0; p < 16; p++) {
    if (k->a[p] < mn)
      mn = k->a[p];
    if (k->a[p] > mx)
      mx = k->a[p];
  }
  if (mn == mx) {
    alpha_flat(o, mn);
    return;
  }
  int best = INT_MAX, bb = 0, bm = 1, bt = 0;
  unsigned char bi[16];
  int R = mx - mn;
  for (int t = 0; t < 16; t++) {
    int tmin = EA[t][3], tmax = EA[t][7], span = tmax - tmin;
    int m0 = (R + span / 2) / span;
    for (int m = m0; m <= m0; m++) { /* (one multiplier, two bases: alpha edges are smooth, the pixel's own table step is small) */
      if (m < 1)
        m = 1;
      if (m > 15)
        m = 15;
      int bases[2] = { (mn + mx + 1) / 2, mn - m * tmin };
      for (int bi3 = 0; bi3 < 2; bi3++) {
        int base = clamp255(bases[bi3]);
        int lv[8];
        for (int i = 0; i < 8; i++)
          lv[i] = clamp255(base + m * EA[t][i]);
        int e = 0;
        unsigned char idx[16];
        for (int p = 0; p < 16 && e < best; p++) {
          int be = INT_MAX, bc = 0, a = k->a[p];
          for (int i = 0; i < 8; i++) {
            int d = lv[i] - a;
            d *= d;
            if (d < be) {
              be = d;
              bc = i;
            }
          }
          idx[p] = (unsigned char)bc;
          e += be;
        }
        if (e < best) {
          best = e;
          bb = base;
          bm = m;
          bt = t;
          memcpy(bi, idx, 16);
          if (e == 0)
            goto done;
        }
      }
    }
  }
done:;
  uint64_t v = 0;
  for (int x = 0; x < 4; x++)
    for (int y = 0; y < 4; y++)
      v = (v << 3) | bi[y * 4 + x]; /* column by column */
  o[0] = (unsigned char)bb;
  o[1] = (unsigned char)((bm << 4) | bt);
  for (int i = 0; i < 6; i++)
    o[2 + i] = (unsigned char)(v >> (8 * (5 - i)));
}

static void gather(const unsigned char *rgba, int w, int h, int bx, int by, Blk *k) {
  for (int y = 0; y < 4; y++) {
    int yy = by * 4 + y;
    if (yy >= h)
      yy = h - 1;
    for (int x = 0; x < 4; x++) {
      int xx = bx * 4 + x;
      if (xx >= w)
        xx = w - 1;
      const unsigned char *p = rgba + ((size_t)yy * w + xx) * 4;
      int i = y * 4 + x;
      k->r[i] = p[0];
      k->g[i] = p[1];
      k->b[i] = p[2];
      k->a[i] = p[3];
    }
  }
}

void etc1_encode(const unsigned char *rgba, int w, int h, unsigned char *out) {
  int bw = (w + 3) / 4, bh = (h + 3) / 4;
  Blk k;
  for (int by = 0; by < bh; by++)
    for (int bx = 0; bx < bw; bx++) {
      gather(rgba, w, h, bx, by, &k);
      enc_color(&k, out);
      out += 8;
    }
}

void etc2_eac_encode(const unsigned char *rgba, int w, int h, unsigned char *out) {
  int bw = (w + 3) / 4, bh = (h + 3) / 4;
  Blk k;
  for (int by = 0; by < bh; by++)
    for (int bx = 0; bx < bw; bx++) {
      gather(rgba, w, h, bx, by, &k);
      enc_alpha(&k, out);
      enc_color(&k, out + 8);
      out += 16;
    }
}
