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

/* the error of the half pl with the base colour (br, bg, bb): sets the table and each pixel's index (0 +small, 1 +big,
   2 -small, 3 -big, the bits ETC1 stores) */
static int eval_half(const Blk *k, const unsigned char *pl, int br, int bg, int bb, int *tbl, unsigned char *sel) {
  int dr[8], dg[8], db[8], S[8], d2 = 0;
  for (int i = 0; i < 8; i++) {
    int p = pl[i];
    dr[i] = k->r[p] - br;
    dg[i] = k->g[p] - bg;
    db[i] = k->b[p] - bb;
    S[i] = dr[i] + dg[i] + db[i];
    d2 += dr[i] * dr[i] + dg[i] * dg[i] + db[i] * db[i];
  }
  int lo = br < bg ? (br < bb ? br : bb) : (bg < bb ? bg : bb);
  int hi = br > bg ? (br > bb ? br : bb) : (bg > bb ? bg : bb);
  int best = INT_MAX;
  for (int t = 0; t < 8; t++) {
    int a = T1[t][0], b = T1[t][1], e;
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
      e = 0;
      for (int i = 0; i < 8; i++) {
        int be = INT_MAX, bc = 0;
        for (int c = 0; c < 4; c++) {
          int x = clamp255(br + M[c]) - k->r[pl[i]], y = clamp255(bg + M[c]) - k->g[pl[i]],
              z = clamp255(bb + M[c]) - k->b[pl[i]];
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
      if (e == 0)
        break;
    }
  }
  return best;
}

static void enc_color(const Blk *k, unsigned char *o) {
  int bestErr = INT_MAX;
  int uniform = 1;
  for (int p = 1; p < 16 && uniform; p++)
    uniform = k->r[p] == k->r[0] && k->g[p] == k->g[0] && k->b[p] == k->b[0];
  for (int flip = 0; flip < 2; flip++) {
    int mean[2][3];
    for (int s = 0; s < 2; s++) {
      int sr = 0, sg = 0, sb = 0;
      for (int i = 0; i < 8; i++) {
        int p = SUBP[flip][s][i];
        sr += k->r[p];
        sg += k->g[p];
        sb += k->b[p];
      }
      mean[s][0] = (sr + 4) / 8;
      mean[s][1] = (sg + 4) / 8;
      mean[s][2] = (sb + 4) / 8;
    }
    for (int diff = uniform ? 1 : 0; diff < 2; diff++) {
      int q[2][3], tb[2], er[2];
      unsigned char se[2][8];
      int ok = 1;
      for (int c = 0; c < 3; c++) {
        if (diff) {
          int q0 = (mean[0][c] * 31 + 127) / 255, q1 = (mean[1][c] * 31 + 127) / 255;
          if (q1 < q0 - 4)
            q1 = q0 - 4;
          if (q1 > q0 + 3)
            q1 = q0 + 3;
          if (q1 < 0 || q1 > 31) { /* (q0 at 0 or 31 with a clamped delta can step out) */
            ok = 0;
            break;
          }
          q[0][c] = q0;
          q[1][c] = q1;
        } else {
          q[0][c] = (mean[0][c] * 15 + 127) / 255;
          q[1][c] = (mean[1][c] * 15 + 127) / 255;
        }
      }
      if (!ok)
        continue;
      int tot = 0;
      for (int s = 0; s < 2; s++) {
        int c8[3];
        for (int c = 0; c < 3; c++)
          c8[c] = diff ? (q[s][c] << 3) | (q[s][c] >> 2) : q[s][c] * 17;
        er[s] = eval_half(k, SUBP[flip][s], c8[0], c8[1], c8[2], &tb[s], se[s]);
        tot += er[s];
        if (tot >= bestErr)
          break;
      }
      if (tot >= bestErr)
        continue;
      bestErr = tot;
      if (diff) {
        o[0] = (unsigned char)((q[0][0] << 3) | ((q[1][0] - q[0][0]) & 7));
        o[1] = (unsigned char)((q[0][1] << 3) | ((q[1][1] - q[0][1]) & 7));
        o[2] = (unsigned char)((q[0][2] << 3) | ((q[1][2] - q[0][2]) & 7));
      } else {
        o[0] = (unsigned char)((q[0][0] << 4) | q[1][0]);
        o[1] = (unsigned char)((q[0][1] << 4) | q[1][1]);
        o[2] = (unsigned char)((q[0][2] << 4) | q[1][2]);
      }
      o[3] = (unsigned char)((tb[0] << 5) | (tb[1] << 2) | (diff << 1) | flip);
      unsigned msb = 0, lsb = 0;
      for (int s = 0; s < 2; s++)
        for (int i = 0; i < 8; i++) {
          int p = SUBP[flip][s][i], bit = (p & 3) * 4 + (p >> 2); /* stored by column: x * 4 + y */
          msb |= (unsigned)(se[s][i] >> 1) << bit;
          lsb |= (unsigned)(se[s][i] & 1) << bit;
        }
      o[4] = (unsigned char)(msb >> 8);
      o[5] = (unsigned char)msb;
      o[6] = (unsigned char)(lsb >> 8);
      o[7] = (unsigned char)lsb;
      if (bestErr <= 8)
        return; /* (as good as the table steps allow) */
    }
    if (uniform)
      return;
  }
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
    for (int m = m0 - 1; m <= m0 + 1; m++) {
      if (m < 1 || m > 15)
        continue;
      int bases[3] = { (mn + mx + 1) / 2, mn - m * tmin, mx - m * tmax };
      for (int bi3 = 0; bi3 < 3; bi3++) {
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
