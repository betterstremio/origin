#include "platform/plat.h"
#include "core/subs.h"
#include "core/net.h"
#include "core/json.h"
#include "core/addons.h"
#include "core/stremio.h"
#include <zlib.h>
#include <ctype.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
static int tc(const char *s, double *out, const char **end) {
  int h = 0, m = 0, sec = 0;
  char ms[8] = "";
  int n = 0;
  const char *p = s;
  if (sscanf(p, "%d:%d:%d%n", &h, &m, &sec, &n) == 3 && n) {
    p += n;
  } else if (sscanf(p, "%d:%d%n", &m, &sec, &n) == 2 && n) {
    h = 0;
    p += n;
  } else
    return 0;
  if (*p == ',' || *p == '.') {
    p++;
    int i = 0;
    while (*p >= '0' && *p <= '9' && i < 7)
      ms[i++] = *p++;
    ms[i] = 0;
  }
  double frac = ms[0] ? atof(ms) / pow(10, strlen(ms)) : 0;
  *out = h * 3600 + m * 60 + sec + frac;
  *end = p;
  return 1;
}
static int cmp_cue(const void *a, const void *b) {
  double x = ((const Cue *)a)->s, y = ((const Cue *)b)->s;
  return x < y ? -1 : x > y;
}
static void cue_add(Cue **cues, int *n, int *cap, double s, double e, const char *t) {
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 256;
    *cues = realloc(*cues, *cap * sizeof **cues);
  }
  (*cues)[*n].s = s;
  (*cues)[*n].e = e;
  (*cues)[*n].t = strdup(t);
  (*n)++;
}

/* ---- the bytes as UTF-8 text ---- */
static size_t put_utf8(char *o, unsigned c) {
  if (c < 0x80) {
    o[0] = (char)c;
    return 1;
  }
  if (c < 0x800) {
    o[0] = (char)(0xc0 | c >> 6);
    o[1] = (char)(0x80 | (c & 63));
    return 2;
  }
  if (c < 0x10000) {
    o[0] = (char)(0xe0 | c >> 12);
    o[1] = (char)(0x80 | ((c >> 6) & 63));
    o[2] = (char)(0x80 | (c & 63));
    return 3;
  }
  o[0] = (char)(0xf0 | c >> 18);
  o[1] = (char)(0x80 | ((c >> 12) & 63));
  o[2] = (char)(0x80 | ((c >> 6) & 63));
  o[3] = (char)(0x80 | (c & 63));
  return 4;
}
static int utf8_ok(const unsigned char *b, size_t n) {
  for (size_t i = 0; i < n;) {
    unsigned c = b[i];
    int k = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
    if (k < 0 || (k == 1 && c < 0xc2) || i + k >= n)
      return 0;
    for (int j = 1; j <= k; j++)
      if ((b[i + j] & 0xc0) != 0x80)
        return 0;
    i += k + 1;
  }
  return 1;
}
/* Windows-1252's 0x80-0x9F (the rest of the byte range is Latin-1 as it stands) */
static const unsigned short CP1252[32] = { 0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
                                           0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0xfffd, 0x017d, 0xfffd,
                                           0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
                                           0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178 };
/* subtitle files come in whatever their authors saved: UTF-16 (with a BOM, or told by its zero bytes), UTF-8 (BOM or
   not), else the Windows code page most old SRT files use. Line ends become "\n" whatever they were. malloc'd */
static char *to_text(const unsigned char *b, size_t n) {
  int be = -1; /* UTF-16: 0 little-endian, 1 big-endian */
  if (n >= 2 && b[0] == 0xff && b[1] == 0xfe) {
    be = 0;
    b += 2;
    n -= 2;
  } else if (n >= 2 && b[0] == 0xfe && b[1] == 0xff) {
    be = 1;
    b += 2;
    n -= 2;
  } else if (n >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) {
    b += 3;
    n -= 3;
  } else if (n >= 4) {
    size_t lim = n < 512 ? n & ~(size_t)1 : 512, ze = 0, zo = 0;
    for (size_t i = 0; i < lim; i += 2) {
      ze += !b[i];
      zo += !b[i + 1];
    }
    if (zo * 3 > lim / 2 && ze * 10 < lim / 2)
      be = 0;
    else if (ze * 3 > lim / 2 && zo * 10 < lim / 2)
      be = 1;
  }
  char *out = malloc(n * 3 + 8);
  size_t o = 0;
  if (be >= 0)
    for (size_t i = 0; i + 1 < n; i += 2) {
      unsigned c = be ? (unsigned)(b[i] << 8 | b[i + 1]) : (unsigned)(b[i + 1] << 8 | b[i]);
      if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {
        unsigned d = be ? (unsigned)(b[i + 2] << 8 | b[i + 3]) : (unsigned)(b[i + 3] << 8 | b[i + 2]);
        if (d >= 0xdc00 && d < 0xe000) {
          c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00);
          i += 2;
        }
      }
      if (c)
        o += put_utf8(out + o, c);
    }
  else if (utf8_ok(b, n)) {
    for (size_t i = 0; i < n; i++)
      if (b[i])
        out[o++] = (char)b[i];
  } else
    for (size_t i = 0; i < n; i++) {
      unsigned c = b[i];
      if (!c)
        continue;
      o += put_utf8(out + o, c >= 0x80 && c < 0xa0 ? CP1252[c - 0x80] : c);
    }
  out[o] = 0;
  size_t w = 0;
  for (size_t i = 0; i < o; i++) {
    if (out[i] == '\r') {
      out[w++] = '\n';
      if (out[i + 1] == '\n')
        i++;
    } else
      out[w++] = out[i];
  }
  out[w] = 0;
  return out;
}

/* ---- SubStation (.ass / .ssa): the [Events] Dialogue lines, override blocks dropped ---- */
static double ass_time(const char *s) {
  int h = 0, m = 0;
  double sec = 0;
  if (sscanf(s, " %d:%d:%lf", &h, &m, &sec) != 3)
    return -1;
  return h * 3600 + m * 60 + sec;
}
static int parse_ass(const char *txt, Cue **out) {
  Cue *cues = NULL;
  int n = 0, cap = 0, inEv = 0, nf = 10, fs = 1, fe = 2, ft = 9;
  char *buf = strdup(txt);
  for (char *ln = buf, *nx; ln; ln = nx) {
    nx = strchr(ln, '\n');
    if (nx)
      *nx++ = 0;
    while (*ln == ' ' || *ln == '\t')
      ln++;
    if (*ln == '[') {
      inEv = !strncasecmp(ln, "[events]", 8);
      continue;
    }
    if (!inEv)
      continue;
    if (!strncasecmp(ln, "Format:", 7)) {
      nf = 0;
      fs = fe = ft = -1;
      for (char *f = ln + 7, *c; f; f = c) {
        c = strchr(f, ',');
        if (c)
          *c++ = 0;
        while (*f == ' ')
          f++;
        if (!strncasecmp(f, "Start", 5))
          fs = nf;
        else if (!strncasecmp(f, "End", 3))
          fe = nf;
        else if (!strncasecmp(f, "Text", 4))
          ft = nf;
        nf++;
      }
      if (fs < 0 || fe < 0 || ft != nf - 1) {
        nf = 10;
        fs = 1;
        fe = 2;
        ft = 9;
      }
      continue;
    }
    if (strncasecmp(ln, "Dialogue:", 9))
      continue;
    char *f[32];
    int k = 0;
    char *p = ln + 9;
    f[k++] = p;
    while (*p && k < nf && k < 32) {
      if (*p == ',') {
        *p = 0;
        f[k++] = p + 1;
      }
      p++;
    }
    if (k < nf)
      continue;
    double s = ass_time(f[fs]), e = ass_time(f[fe]);
    if (s < 0 || e <= s)
      continue;
    const char *t = f[ft];
    char text[2048];
    size_t o = 0;
    int drawing = 0;
    for (size_t i = 0; t[i] && o < sizeof text - 4;) {
      if (t[i] == '{') {
        const char *z = strchr(t + i, '}');
        if (!z)
          break;
        for (const char *q = t + i; q < z; q++)
          if (q[0] == '\\' && q[1] == 'p' && q[2] >= '0' && q[2] <= '9')
            drawing = q[2] != '0';
        i = (size_t)(z - t) + 1;
        continue;
      }
      if (t[i] == '\\' && (t[i + 1] == 'N' || t[i + 1] == 'n')) {
        while (o && text[o - 1] == ' ')
          o--;
        text[o++] = '\n';
        i += 2;
        continue;
      }
      if (t[i] == '\\' && t[i + 1] == 'h') {
        text[o++] = ' ';
        i += 2;
        continue;
      }
      if (!drawing)
        text[o++] = t[i];
      i++;
    } /* vector drawings are shapes, not words */
    text[o] = 0;
    char *a = text;
    while (*a == ' ' || *a == '\n')
      a++;
    size_t l = strlen(a);
    while (l && (a[l - 1] == ' ' || a[l - 1] == '\n'))
      a[--l] = 0;
    if (*a)
      cue_add(&cues, &n, &cap, s, e, a);
  }
  free(buf);
  if (n)
    qsort(cues, n, sizeof *cues, cmp_cue);
  *out = cues;
  return n;
}

/* blocks separated by blank lines; the line with "-->" times the text under it; tags and {..} are dropped */
int subs_parse(const char *txt, Cue **out) {
  *out = NULL;
  if (!txt)
    return 0;
  if (!strncmp(txt, "\xef\xbb\xbf", 3))
    txt += 3;
  char *buf = to_text((const unsigned char *)txt, strlen(txt)); /* "\r\n" must not read as a blank line */
  if (strstr(buf, "[Events]") || strstr(buf, "[events]") || !strncmp(buf, "[Script Info]", 13)) {
    int k = parse_ass(buf, out);
    free(buf);
    return k;
  }
  Cue *cues = NULL;
  int n = 0, cap = 0;
  char *blk = buf;
  while (blk && *blk) {
    char *next = strstr(blk, "\n\n");
    if (next) {
      *next = 0;
      next += 2;
      while (*next == '\n')
        next++;
    }
    char *arrow = strstr(blk, "-->");
    if (arrow) {
      char *ls = arrow;
      while (ls > blk && ls[-1] != '\n')
        ls--;
      double s, e;
      const char *p1, *p2;
      const char *a = ls;
      while (*a == ' ')
        a++;
      if (tc(a, &s, &p1)) {
        const char *b = arrow + 3;
        while (*b == ' ')
          b++;
        if (tc(b, &e, &p2)) {
          char *tl = strchr(arrow, '\n');
          char text[2048] = "";
          size_t o = 0;
          if (tl) {
            tl++;
            int tag = 0, brace = 0;
            for (const char *c = tl; *c && o < sizeof text - 1; c++) {
              if (*c == '<') {
                tag = 1;
                continue;
              }
              if (*c == '>') {
                tag = 0;
                continue;
              }
              if (*c == '{') {
                brace = 1;
                continue;
              }
              if (*c == '}') {
                brace = 0;
                continue;
              }
              if (!tag && !brace)
                text[o++] = *c;
            }
            text[o] = 0;
          }
          char *t0 = text;
          while (*t0 == ' ' || *t0 == '\n')
            t0++;
          char *t1 = t0 + strlen(t0);
          while (t1 > t0 && (t1[-1] == ' ' || t1[-1] == '\n'))
            *--t1 = 0;
          if (*t0 && e > s)
            cue_add(&cues, &n, &cap, s, e, t0);
        }
      }
    }
    blk = next;
  }
  free(buf);
  if (n)
    qsort(cues, n, sizeof *cues, cmp_cue);
  *out = cues;
  return n;
}
void subs_free(Cue *c, int n) {
  if (!c)
    return;
  for (int i = 0; i < n; i++)
    free(c[i].t);
  free(c);
}

/* some subtitle hosts hand out the .gz file itself (no Content-Encoding): unpacked here, 16 MB at most; then the
   text's own encoding (UTF-16 has zero bytes, so it is decoded before anything reads it as a C string) */
int subs_parse_body(const unsigned char *body, size_t len, Cue **out) {
  *out = NULL;
  if (!body)
    return 0;
  unsigned char *buf = NULL;
  size_t n = 0;
  if (len >= 2 && body[0] == 0x1f && body[1] == 0x8b) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK)
      return 0;
    size_t cap = len * 4 + 4096;
    buf = malloc(cap);
    int rc = Z_OK;
    z.next_in = (unsigned char *)body;
    z.avail_in = (uInt)len;
    while (rc == Z_OK) {
      if (cap - n < 4096) {
        if (cap >= 16u << 20)
          break;
        cap *= 2;
        buf = realloc(buf, cap);
      }
      z.next_out = buf + n;
      z.avail_out = (uInt)(cap - n - 1);
      rc = inflate(&z, Z_NO_FLUSH);
      n = cap - 1 - z.avail_out;
    }
    inflateEnd(&z);
    if (rc != Z_STREAM_END && !n) {
      free(buf);
      return 0;
    }
    body = buf;
    len = n;
  }
  char *txt = to_text(body, len);
  free(buf);
  int k = subs_parse_any(txt, out);
  free(txt);
  return k;
}

/* ---- fetching on a thread of their own ----
   The shared request queue can sit behind six stream addons for 45 s each: a subtitle the viewer just picked, or the
   lists, must not wait there. Each fetch runs on its own short thread (files are also read there, off the drawing
   thread); subs_pump hands the answers over on the main thread. */
typedef struct FindQ FindQ;
typedef struct SubJob {
  struct SubJob *next;
  char *url;
  int list, status;
  unsigned char *body;
  size_t len;
  Cue *cues;
  int n;
  void (*cb)(int, Cue *, int, void *);
  void *ud;
  FindQ *fq;
  int src;
  SubEntry *got;
  int ngot, parsed, raw, noUrl, longUrl, langCap, sourceCap;
  Uint32 elapsed;
  int os;
} SubJob; /* list: one addon's answer for a FindQ (src: which); os: the built-in OpenSubtitles list */
static pthread_mutex_t jmu = PTHREAD_MUTEX_INITIALIZER;
static SubJob *jdone;
static void job_finish(SubJob *j) {
  pthread_mutex_lock(&jmu);
  j->next = jdone;
  jdone = j;
  pthread_mutex_unlock(&jmu);
}
static void list_parse(SubJob *j);
static void *job_main(void *a) {
  plat_thread_background(5);
  SubJob *j = a;
  /* Some add-ons time subtitles on the first request. Each has its own worker, so the others can still answer. */
  int timeout = j->os ? 20000 : j->list ? 35000 : 45000;
  Uint32 started = SDL_GetTicks();
  j->body = net_fetch_sync(j->url, &j->len, &j->status, timeout);
  j->elapsed = SDL_GetTicks() - started;
  if (j->os) {
  } /* read on the main thread (on_list) */
  else if (j->list)
    list_parse(j);
  else if (j->body) {
    j->n = subs_parse_body(j->body, j->len, &j->cues);
    free(j->body);
    j->body = NULL;
  }
  job_finish(j);
  return NULL;
}
static void job_start(SubJob *j) {
  pthread_t t;
  if (pthread_create(&t, NULL, job_main, j))
    job_main(j);
  else
    pthread_detach(t);
}
static void find_answer(SubJob *j);
static void on_list(int st, const unsigned char *b, size_t n, void *ud);
void subs_pump(void) {
  pthread_mutex_lock(&jmu);
  SubJob *l = jdone;
  jdone = NULL;
  pthread_mutex_unlock(&jmu);
  SubJob *rev = NULL;
  while (l) {
    SubJob *nx = l->next;
    l->next = rev;
    rev = l;
    l = nx;
  } /* in the order they finished */
  while (rev) {
    SubJob *j = rev;
    rev = j->next;
    if (j->os)
      on_list(j->status, j->body ? j->body : (const unsigned char *)"", j->len, j->ud);
    else if (j->list)
      find_answer(j);
    else
      j->cb(j->status, j->cues, j->n, j->ud);
    free(j->body);
    free(j->url);
    free(j->got);
    free(j);
  }
}
void subs_load(const char *url, void (*cb)(int status, Cue *cues, int n, void *ud), void *ud) {
  SubJob *j = calloc(1, sizeof *j);
  j->url = strdup(url);
  j->cb = cb;
  j->ud = ud;
  job_start(j);
}

void subs_filename_from_url(const char *url, char *filename, size_t cap) {
  if (!cap || filename[0])
    return;
  const char *end = url + strcspn(url, "?#");
  const char *path = strstr(url, "://");
  path = path ? strchr(path + 3, '/') : NULL;
  if (!path || path >= end)
    return;
  const char *base = end;
  while (base > path && base[-1] != '/')
    base--;
  size_t n = 0;
  for (const char *p = base; p < end; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '%' && p + 2 < end && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
      unsigned value;
      sscanf(p + 1, "%2x", &value);
      c = (unsigned char)value;
      p += 2;
    }
    if (c < 32 || c == 127 || c == '/' || c == '\\' || n + 1 >= cap) {
      filename[0] = 0;
      return;
    }
    filename[n++] = (char)c;
  }
  filename[n] = 0;
  const char *ext = strrchr(filename, '.');
  static const char *const formats[] = { ".mkv", ".mp4", ".avi",  ".mov",  ".m4v", ".webm",
                                         ".wmv", ".mpg", ".mpeg", ".m2ts", ".ts" };
  for (size_t i = 0; ext && i < sizeof formats / sizeof formats[0]; i++)
    if (!strcasecmp(ext, formats[i]))
      return;
  filename[0] = 0;
}

/* ---- the OpenSubtitles hash: the size plus the 64-bit little-endian words of the first and the last 64 KB ---- */
void subs_os_hash(SubQuery *q) {
  NetRange *r = net_range_new();
  if (!r)
    return;
  unsigned char *a = NULL, *b = NULL;
  long ga = 0, gb = 0;
  long long tot = -1;
  if (net_range_get(r, q->url, 0, 65536, &a, &ga, &tot, NULL, 8000) == NR_OK) {
    const char *resolved = net_range_url(r);
    if (resolved)
      subs_filename_from_url(resolved, q->filename, sizeof q->filename);
    if (tot > 0)
      q->size = (double)tot;
    else if (q->size > 0)
      tot = (long long)q->size;
    if (ga == 65536 && tot >= 131072 &&
        net_range_get(r, q->url, tot - 65536, 65536, &b, &gb, NULL, NULL, 8000) == NR_OK && gb == 65536) {
      unsigned long long h = (unsigned long long)tot;
      for (int k = 0; k < 65536; k += 8) {
        unsigned long long x = 0, y = 0;
        for (int q = 7; q >= 0; q--) {
          x = x << 8 | a[k + q];
          y = y << 8 | b[k + q];
        }
        h += x + y;
      }
      snprintf(q->hash, sizeof q->hash, "%016llx", h);
    }
  }
  free(a);
  free(b);
  net_range_free(r);
}

/* what an entry says of the file it was made for: its name, and whether the video's hash found it ("m": "h" on
   OpenSubtitles' answers; other addons' own words for it) */
void subs_entry_facts(SubEntry *e, const struct cJSON *s) {
  const char *rel = js_str(s, "subtitleFileName", js_str(s, "releaseName", js_str(s, "release", "")));
  snprintf(e->rel, sizeof e->rel, "%s", rel);
  const char *m = js_str(s, "m", "");
  const cJSON *hm = cJSON_GetObjectItemCaseSensitive(s, "hashMatch");
  const char *mb = js_str(s, "matchedBy", "");
  e->hm =
      !strcmp(m, "h") || cJSON_IsTrue(hm) || (cJSON_IsNumber(hm) && hm->valuedouble > 0) || strstr(mb, "hash") != NULL;
  const cJSON *fm = cJSON_GetObjectItemCaseSensitive(s, "fpsMilli");
  double fp = cJSON_IsNumber(fm) ? fm->valuedouble : cJSON_IsString(fm) ? atof(fm->valuestring) : 0;
  e->fps = fp > 10000 && fp < 61000 ? fp / 1000 : 0;
}
/* the hash worked out for the stream last asked about (the reference Auto-sync uses asks again with it) */
static pthread_mutex_t hmu = PTHREAD_MUTEX_INITIALIZER;
static char hUrl[4096], hHash[24];
static double hSize;
static void hash_keep(const char *url, const char *h, double size) {
  pthread_mutex_lock(&hmu);
  snprintf(hUrl, sizeof hUrl, "%s", url);
  snprintf(hHash, sizeof hHash, "%s", h);
  hSize = size;
  pthread_mutex_unlock(&hmu);
}
int subs_hash_of(const char *url, char out[24], double *size) {
  pthread_mutex_lock(&hmu);
  int ok = hHash[0] && !strcmp(url, hUrl);
  if (ok) {
    snprintf(out, 24, "%s", hHash);
    if (size)
      *size = hSize;
  }
  pthread_mutex_unlock(&hmu);
  return ok;
}

/* ---- the built-in OpenSubtitles list: asked by id alone, English first, then by name, 12 per language ---- */
typedef struct {
  void (*cb)(SubEntry *, int, int, void *);
  void *ud;
} ListReq;
static int cmp_os(const void *a, const void *b) {
  const SubEntry *x = a, *y = b;
  int xe = !strcmp(x->lang, "eng"), ye = !strcmp(y->lang, "eng");
  if (xe != ye)
    return ye - xe;
  int c = strcmp(x->name, y->name);
  return c ? c : x->n - y->n;
}
static void on_list(int st, const unsigned char *b, size_t n, void *ud) {
  (void)n;
  ListReq *r = ud;
  cJSON *j = st == 200 ? net_json(b) : NULL;
  const cJSON *arr = js_arr(j, "subtitles"), *s;
  SubEntry *out = calloc(256, sizeof *out);
  int no = 0;
  char langs[64][24];
  int per[64], nl = 0;
  if (arr)
    cJSON_ArrayForEach(s, arr) {
      if (no >= 256)
        break;
      const char *url = js_str(s, "url", NULL);
      const char *lang = js_str(s, "lang", "");
      char key[24];
      subs_lang_canon(lang, key, sizeof key);
      if (!*lang || !url || strlen(url) >= sizeof out->url)
        continue;
      int li = -1;
      for (int i = 0; i < nl; i++)
        if (!strcmp(langs[i], key))
          li = i;
      if (li < 0 && nl < 64) {
        li = nl++;
        snprintf(langs[li], sizeof langs[li], "%s", key);
        per[li] = 0;
      }
      if (li < 0)
        continue;
      if (++per[li] > 12)
        continue;
      SubEntry *e = &out[no++];
      snprintf(e->lang, sizeof e->lang, "%s", key);
      const char *nm = subs_sub_name(lang);
      if (nm)
        snprintf(e->name, sizeof e->name, "%s", nm);
      else {
        snprintf(e->name, sizeof e->name, "%s", lang);
        for (char *p = e->name; *p; p++)
          if (*p >= 'a' && *p <= 'z')
            *p -= 32;
      }
      snprintf(e->url, sizeof e->url, "%s", url);
      snprintf(e->addon, sizeof e->addon, "OpenSubtitles");
      e->from = SUB_FALLBACK;
      e->order = 999;
      e->n = per[li];
      subs_entry_facts(e, s);
    }
  if (no)
    qsort(out, no, sizeof *out, cmp_os);
  r->cb(out, no, j != NULL, r->ud);
  net_json_free(j);
  free(r);
}
void subs_list(const char *type, const char *vid, void (*cb)(SubEntry *, int, int, void *), void *ud) {
  char t[96], enc[400], url[600];
  addon_urlenc(type, t, sizeof t);
  addon_urlenc(vid, enc, sizeof enc);
  snprintf(url, sizeof url, "https://opensubtitles-v3.strem.io/subtitles/%s/%s.json", t, enc);
  ListReq *r = malloc(sizeof *r);
  r->cb = cb;
  r->ud = ud;
  SubJob *j = calloc(1, sizeof *j);
  j->url = strdup(url);
  j->list = 1;
  j->os = 1;
  j->ud = r;
  job_start(j);
}

/* ---- the addons' lists: every installed subtitles addon for this video, asked with the file's hash, size and name
   when known (/subtitles/{type}/{id}/videoHash=..&videoSize=..&filename=...json); the subtitles the stream carries
   first. The built-in list above is never mixed in. ---- */
/* subtitle addons asked per video: 16, not ADDONS_MAX, since find_add's duplicate check compares every new file with
   all kept so far on the player's thread (64 addons of 400 files each would stall a weak TV) */
#define SUBS_ADDONS_MAX 16
typedef struct {
  char name[64], base[ADDON_URL_MAX];
  int order, preferred;
} FindSrc;
struct FindQ {
  unsigned tok;
  SubQuery q;
  FindSrc src[SUBS_ADDONS_MAX];
  int nsrc, pending, nok, preferredPending;
  SubEntry *all;
  int n, cap;
  void (*cb)(SubEntry *, int, int, int, int, void *);
  void *ud;
};
static void entry_name(SubEntry *e, const char *raw) {
  const char *name = subs_sub_name(raw);
  snprintf(e->name,sizeof e->name,"%s", name ? name : raw);
  if (!name && strlen(e->name) <= 3)
    for (char *p = e->name; *p; p++)
      *p = (char)toupper((unsigned char)*p);
}
static void list_parse(SubJob *j) {
  cJSON *js = j->body && j->status == 200 ? cJSON_ParseWithLength((const char *)j->body, j->len) : NULL;
  const cJSON *arr = js_arr(js, "subtitles"), *s;
  if (j->body && j->status == 200 && !js)
    fprintf(stderr, "[subs] %s: the answer is not JSON\n", j->fq->src[j->src].name);
  j->parsed = js != NULL;
  int n = j->raw = arr ? cJSON_GetArraySize(arr) : 0;
  if (n > 400)
    n = 400;
  j->got = calloc(n ? n : 1, sizeof *j->got);
  char langs[64][24];
  int per[64], nl = 0;
  const FindSrc *fs = &j->fq->src[j->src];
  if (arr)
    cJSON_ArrayForEach(s, arr) {
      if (j->ngot >= n) {
        j->sourceCap = j->raw - j->ngot - j->noUrl - j->longUrl - j->langCap;
        break;
      }
      const char *url = js_str(s, "url", NULL);
      if (!url || !*url) {
        j->noUrl++;
        continue;
      }
      if (strlen(url) >= sizeof j->got->url) {
        j->longUrl++;
        continue;
      }
      const char *lang = js_str(s, "lang", "und");
      if (!*lang)
        lang = "und";
      char key[24];
      subs_lang_canon(lang, key, sizeof key);
      int li = -1;
      for (int i = 0; i < nl; i++)
        if (!strcmp(langs[i], key))
          li = i;
      if (li < 0 && nl < 64) {
        li = nl++;
        snprintf(langs[li], sizeof langs[li], "%s", key);
        per[li] = 0;
      }
      if (li < 0 || per[li] >= 12) {
        j->langCap++;
        continue;
      }
      per[li]++;
      SubEntry *e = &j->got[j->ngot++];
      snprintf(e->lang, sizeof e->lang, "%s", key);
      snprintf(e->url, sizeof e->url, "%s", url);
      snprintf(e->label, sizeof e->label, "%s", js_str(s, "label", ""));
      snprintf(e->addon, sizeof e->addon, "%s", fs->name);
      entry_name(e,lang);
      e->from = SUB_ADDON;
      e->order = fs->order;
      e->n = per[li];
      e->preferred = fs->preferred ? e->n : 0;
      subs_entry_facts(e, s);
    }
  cJSON_Delete(js);
  free(j->body);
  j->body = NULL;
}
static int cmp_entry(const void *a, const void *b) {
  const SubEntry *x = a, *y = b;
  if (x->from != y->from)
    return x->from - y->from;
  if (x->order != y->order)
    return x->order - y->order;
  return x->n - y->n;
}
static void find_deliver(FindQ *f) {
  SubEntry *out = f->n ? malloc(f->n * sizeof *out) : NULL;
  if (out)
    memcpy(out, f->all, f->n * sizeof *out);
  f->cb(out, out ? f->n : 0, f->pending <= 0, f->nok, f->preferredPending, f->ud);
  if (f->pending <= 0) {
    free(f->all);
    free(f);
  }
}
static int find_add(FindQ *f, const SubEntry *e, int n) {
  int duplicates = 0;
  for (int i = 0; i < n; i++) {
    int k = 0;
    for (; k < f->n && strcmp(f->all[k].url, e[i].url); k++)
      ;
    if (k < f->n) {
      duplicates++;
      if (e[i].preferred && !f->all[k].preferred) {
        f->all[k] = e[i];
      }
      continue;
    }
    if (f->n == f->cap) {
      int cap = f->cap ? f->cap * 2 : 64;
      SubEntry *all = realloc(f->all, cap * sizeof *all);
      if (!all)
        break;
      f->all = all;
      f->cap = cap;
    }
    f->all[f->n] = e[i];
    if (e[i].from == SUB_STREAM) {
      entry_name(&f->all[f->n], e[i].lang);
      subs_lang_canon(e[i].lang, f->all[f->n].lang, sizeof f->all[f->n].lang);
    }
    f->n++;
  }
  if (f->n > 1)
    qsort(f->all, f->n, sizeof *f->all, cmp_entry);
  return duplicates;
}
static void addon_host(const char *url, char *host, size_t cap) {
  const char *p = strstr(url, "://");
  if (!p) {
    snprintf(host,cap,"?");
    return;
  }
  p += 3;
  const char *end = p + strcspn(p, "/?#");
  for (const char *x = p; x < end; x++)
    if (*x == '@')
      p = x+1;
  snprintf(host,cap,"%.*s",(int)(end-p),p);
}
static void log_skipped(const char *name, const char *url, const char *reason) {
  char host[128];
  addon_host(url,host,sizeof host);
  fprintf(stderr,"[subs] addon %s (%s): skipped: %s\n",name,host,reason);
}
static void find_answer(SubJob *j) {
  FindQ *f = j->fq;
  f->pending--;
  if (f->src[j->src].preferred)
    f->preferredPending = 0;
  int before = f->n, dup = find_add(f, j->got, j->ngot);
  char host[128];
  const FindSrc *src = &f->src[j->src];
  addon_host(src->base, host, sizeof host);
  if (j->parsed)
    f->nok++;
  if (j->status > 0)
    fprintf(stderr, "[subs] addon %s (%s): http %d, %d raw, %d kept, dropped: %d no-url, %d long-url, "
            "%d lang-cap, %d dup, %d source-cap\n", src->name, host, j->status, j->raw, f->n-before,
            j->noUrl, j->longUrl, j->langCap, dup, j->sourceCap);
  else
    fprintf(stderr, "[subs] addon %s (%s): no answer after %.1fs\n", src->name, host, j->elapsed / 1000.0);
  find_deliver(f);
}
/* the hash (when the file can be read in ranges), then one request per source */
static void *find_main(void *a) {
  plat_thread_background(5);
  FindQ *f = a;
  SubQuery *q = &f->q;
  if (!q->hash[0] && q->hashable)
    subs_os_hash(q);
  if (q->hash[0])
    hash_keep(q->url, q->hash, q->size);
  char ex[sizeof q->filename * 3 + 100] = "", n[32];
  if (q->hash[0])
    addon_extra_add(ex, sizeof ex, "videoHash", q->hash);
  if (q->size > 0) {
    snprintf(n, sizeof n, "%.0f", q->size);
    addon_extra_add(ex, sizeof ex, "videoSize", n);
  }
  if (q->filename[0]) {
    char name[sizeof q->filename * 3 + 4];
    addon_urlenc(q->filename, name, sizeof name);
    size_t used = strlen(ex);
    snprintf(ex + used, sizeof ex - used, "%sfilename=%s", used ? "&" : "", name);
  }
  fprintf(stderr, "[subs] asking %d source(s) (hash %d, size %d, filename %d)\n", f->nsrc,
          !!q->hash[0], q->size > 0, !!q->filename[0]);
  int ns = f->nsrc; /* f may be gone once the last request is out */
  for (int i = 0; i < ns; i++) {
    SubJob *j = calloc(1, sizeof *j);
    char url[ADDON_URL_MAX + 2000];
    addon_url(url, sizeof url, f->src[i].base, "subtitles", q->type, q->vid, ex);
    j->url = strdup(url);
    j->list = 1;
    j->fq = f;
    j->src = i;
    job_start(j);
  }
  return NULL;
}
int subs_addons_for(const char *type, const char *vid) {
  return addon_count_subs(type, vid);
}
void subs_find(const SubQuery *q, const SubEntry *own, int nown, const char *preferred,
               void (*cb)(SubEntry *, int, int, int, int, void *), void *ud) {
  FindQ *f = calloc(1, sizeof *f);
  if (!f) {
    cb(NULL, 0, 1, 0, 0, ud);
    return;
  }
  f->q = *q;
  f->cb = cb;
  f->ud = ud;
  cJSON *a = sb_addons();
  const cJSON *ad;
  int order = 0;
  if (a)
    cJSON_ArrayForEach(ad, a) {
      order++;
      if (!addon_serves_subs(ad, q->type, q->vid)) {
        if (addon_serves_subs(ad, "movie", NULL))
          log_skipped(addon_name(ad), js_str(ad,"transportUrl",""), "ineligible type or id prefix");
        continue;
      }
      int selected = 0;
      if (*preferred && !f->preferredPending) {
        char key[SUBS_PROVIDER_KEY_SIZE];
        sb_addon_key(ad, key, sizeof key);
        selected = !strcmp(key, preferred);
      }
      if (f->nsrc >= SUBS_ADDONS_MAX) {
        if (!selected) {
          log_skipped(addon_name(ad),js_str(ad,"transportUrl",""),"over the 16-provider cap");
          continue;
        }
        FindSrc *last = &f->src[SUBS_ADDONS_MAX - 1];
        log_skipped(last->name,last->base,"over the 16-provider cap");
      }
      int index = f->nsrc < SUBS_ADDONS_MAX ? f->nsrc++ : SUBS_ADDONS_MAX - 1;
      FindSrc *s = &f->src[index];
      snprintf(s->name, sizeof s->name, "%s", addon_name(ad));
      addon_base(ad, s->base, sizeof s->base);
      s->order = order;
      s->preferred = selected;
      f->preferredPending |= selected;
    }
  if (nown)
    find_add(f, own, nown);
  f->pending = f->nsrc;
  if (!f->nsrc) {
    find_deliver(f);
    return;
  } /* only the stream's own: final at once */
  if (nown || f->preferredPending)
    find_deliver(f); /* the stream's own and the preferred provider's pending state, before any requests start */
  pthread_t t;
  if (pthread_create(&t, NULL, find_main, f))
    find_main(f);
  else
    pthread_detach(t);
}

/* ---- YouTube timed text (trailer captions) ------------------------------------------------------ */
/* the entities these files use; &amp; first, so the classic format's doubly escaped "&amp;#39;" comes out whole */
static void tt_text(const char *s, size_t n, int stripTags, char *out, size_t cap) {
  char *raw = malloc(n + 1);
  size_t o = 0;
  int tag = 0;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (stripTags) {
      if (c == '<') {
        tag = 1;
        continue;
      }
      if (tag) {
        if (c == '>')
          tag = 0;
        continue;
      }
    }
    raw[o++] = c;
  }
  raw[o] = 0;
  static const char *ent[][2] = {
    { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&#39;", "'" }
  };
  for (int k = 0; k < 5; k++) {
    size_t el = strlen(ent[k][0]);
    char *w = raw;
    for (char *r = raw; *r;) {
      if (!strncmp(r, ent[k][0], el)) {
        *w++ = ent[k][1][0];
        r += el;
      } else
        *w++ = *r++;
    }
    *w = 0;
  }
  /* line breaks become one space; trimmed */
  o = 0;
  int nl = 0;
  for (char *r = raw; *r && o < cap - 1; r++) {
    if (*r == '\n' || *r == '\r') {
      if (!nl)
        out[o++] = ' ';
      nl = 1;
      continue;
    }
    nl = 0;
    out[o++] = *r;
  }
  out[o] = 0;
  free(raw);
  char *a = out;
  while (*a == ' ' || *a == '\t')
    a++;
  if (a != out)
    memmove(out, a, strlen(a) + 1);
  size_t l = strlen(out);
  while (l && (out[l - 1] == ' ' || out[l - 1] == '\t'))
    out[--l] = 0;
}
static const char *ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  return p;
}
/* attr="..." right here (after white space); the value's span */
static int tt_attr(const char **pp, const char *name, const char **v, size_t *vl) {
  const char *p = *pp, *q = ws(p);
  if (q == p)
    return 0;
  size_t nl = strlen(name);
  if (strncmp(q, name, nl) || q[nl] != '=' || q[nl + 1] != '"')
    return 0;
  q += nl + 2;
  const char *e = strchr(q, '"');
  if (!e)
    return 0;
  *v = q;
  *vl = (size_t)(e - q);
  *pp = e + 1;
  return 1;
}
/* srv3 (<p t="ms" d="ms">, words in <s> tags) first, then the classic <text start="s" dur="s"> */
int subs_parse_timedtext(const char *x, Cue **out) {
  *out = NULL;
  if (!x)
    return 0;
  Cue *cues = NULL;
  int n = 0, cap = 0;
  char text[2048];
  for (int pass = 0; pass < 2 && !n; pass++) {
    const char *open = pass ? "<text" : "<p", *close = pass ? "</text>" : "</p>", *a1 = pass ? "start" : "t",
               *a2 = pass ? "dur" : "d";
    size_t ol = strlen(open);
    for (const char *p = strstr(x, open); p; p = strstr(p, open)) {
      const char *q = p + ol, *v1, *v2;
      size_t l1, l2;
      p += ol;
      if (!tt_attr(&q, a1, &v1, &l1) || !tt_attr(&q, a2, &v2, &l2))
        continue;
      const char *gt = strchr(q, '>');
      if (!gt)
        break;
      const char *body = gt + 1, *end = strstr(body, close);
      if (!end)
        break;
      char b1[32], b2[32];
      snprintf(b1, sizeof b1, "%.*s", (int)(l1 < 31 ? l1 : 31), v1);
      snprintf(b2, sizeof b2, "%.*s", (int)(l2 < 31 ? l2 : 31), v2);
      double s = atof(b1), d = atof(b2);
      if (!pass) {
        s /= 1000;
        d /= 1000;
      }
      tt_text(body, (size_t)(end - body), !pass, text, sizeof text);
      if (pass) {
        char *w = text;
        int tag = 0;
        for (char *r = text; *r; r++) {
          if (*r == '<')
            tag = 1;
          else if (tag) {
            if (*r == '>')
              tag = 0;
          } else
            *w++ = *r;
        }
        *w = 0;
      } /* the classic format escapes its markup: dropped once decoded (no markup on screen) */
      if (*text)
        cue_add(&cues, &n, &cap, s, s + d, text);
      p = end + strlen(close);
    }
  }
  if (n)
    qsort(cues, n, sizeof *cues, cmp_cue);
  *out = cues;
  return n;
}
/* whatever a subtitle URL answered: timed text when it looks like it (and parses), else SRT / WebVTT */
int subs_parse_any(const char *txt, Cue **out) {
  *out = NULL;
  if (!txt)
    return 0;
  if (strstr(txt, "<timedtext") || strstr(txt, "<transcript") || strstr(txt, "<text ") || strstr(txt, "<p ")) {
    int n = subs_parse_timedtext(txt, out);
    if (n)
      return n;
  }
  return subs_parse(txt, out);
}
