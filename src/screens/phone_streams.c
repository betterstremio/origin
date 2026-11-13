/* A title's streams on a phone: a sheet that rises from the bottom over the page. At its head the name and the
   episode; then chips (All, and one per addon); then the list, the stream last played first and the best pick for
   this device next (when that setting is on), each row its addon, its name and its details. A tap plays it; Back,
   a tap above the sheet or the close button shuts it. The lookups are the TV sheet's (core/stremio.c). */
#include "screens/phone.h"
#include "core/form.h"
#include "core/i18n.h"
#include "core/hit.h"
#include "core/keys.h"
#include "core/best.h"
#include "core/stremio.h"
#include "platform/plat.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/icon.h"
#include "ui/widgets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAXG 32
#define MAXI 400
enum { R_PLAIN, R_RESUME, R_BEST };
typedef struct { char name[96]; Stream *items; int n; } Group;
static struct {
  int open, closing; Tween k; unsigned tok; int loading, pending, asked;
  char type[16], id[64], vid[80], title[256], label[300];
  Group g[MAXG]; int ng; Stream resume; int hasResume;
  const Stream *items[MAXI]; int kind[MAXI], group[MAXI], n; int filter; char bestSum[192];
  Scroller list, chips;
} S;
static Screen sheet;

static void groups_clear(void) { for (int i = 0; i < S.ng; i++) free(S.g[i].items); memset(S.g, 0, sizeof S.g); S.ng = 0; }
static void rebuild(void) {
  int n = 0;
  if (S.hasResume && S.filter < 0) { S.items[n] = &S.resume; S.kind[n] = R_RESUME; S.group[n] = -1; n++; }
  int first = n;
  for (int g = 0; g < S.ng && n < MAXI - 1; g++) { if (S.filter >= 0 && g != S.filter) continue;
    for (int i = 0; i < S.g[g].n && n < MAXI - 1; i++) { S.items[n] = &S.g[g].items[i]; S.kind[n] = R_PLAIN; S.group[n] = g; n++; } }
  if (best_on() && S.filter < 0 && n > first) {
    const Stream *list[MAXI]; int m = 0; for (int i = first; i < n; i++) list[m++] = S.items[i];
    BestRate r; const Stream *b = best_pick(list, m, best_dur(sb_meta_cached(S.type, S.id), S.type), NULL, -1, &r);
    if (b && n < MAXI) { memmove(S.items + first + 1, S.items + first, (size_t)(n - first) * sizeof *S.items); memmove(S.kind + first + 1, S.kind + first, (size_t)(n - first) * sizeof *S.kind); memmove(S.group + first + 1, S.group + first, (size_t)(n - first) * sizeof *S.group);
      S.items[first] = b; S.kind[first] = R_BEST; S.group[first] = -1; n++; snprintf(S.bestSum, sizeof S.bestSum, "%s", r.sum); } }
  S.n = n;
}
static void add(const Stream *arr, int n, int gi, const char *name) {
  if (gi < 0 || gi >= MAXG) return; Group *g = &S.g[gi]; if (gi >= S.ng) S.ng = gi + 1; if (!g->name[0]) snprintf(g->name, sizeof g->name, "%s", name ? name : T("Addon"));
  int k = n < 60 - g->n ? n : 60 - g->n; if (k > 0) { Stream *x = realloc(g->items, (size_t)(g->n + k) * sizeof *x); if (!x) return; g->items = x; memcpy(g->items + g->n, arr, (size_t)k * sizeof *arr); g->n += k; }
  rebuild();
}
static void on_part(StreamGroup *g, int gi, void *ud) { if ((unsigned)(uintptr_t)ud == S.tok) add(g->items, g->n, gi, g->name); free(g->items); }
static void on_done(int total, void *ud) { (void)total; if ((unsigned)(uintptr_t)ud == S.tok) { S.loading = 0; rebuild(); } }
static void on_ans(int pending, int count, void *ud) { if ((unsigned)(uintptr_t)ud == S.tok) { S.pending = pending; S.asked = count; } }

void phone_streams_show(const char *type, const char *id, const char *vid, const char *title, const char *label) {
  groups_clear(); memset(S.items, 0, sizeof S.items); S.n = 0; S.filter = -1; S.bestSum[0] = 0; S.pending = 0; S.asked = 0;
  snprintf(S.type, sizeof S.type, "%s", type); snprintf(S.id, sizeof S.id, "%s", id); snprintf(S.vid, sizeof S.vid, "%s", vid);
  snprintf(S.title, sizeof S.title, "%s", title ? title : ""); snprintf(S.label, sizeof S.label, "%s", label ? label : "");
  S.hasResume = sb_resume_stream(id, vid, &S.resume); if (S.hasResume) snprintf(S.resume.name, sizeof S.resume.name, "%s", T("Resume last stream"));
  S.open = 1; S.closing = 0; S.loading = 1; S.tok++; tween_to(&S.k, 1, 0.3f, EASE_STD); scroller_set(&S.list, 0); scroller_set(&S.chips, 0);
  rebuild(); app_overlay = &sheet;
  sb_streams_ex(type, vid, on_part, on_done, on_ans, (void *)(uintptr_t)S.tok);
}
void phone_streams_close(void) { if (!S.open || S.closing) return; S.closing = 1; S.tok++; tween_to(&S.k, 0, 0.22f, EASE_CSS_EASEIN); }
int phone_streams_on(void) { return S.open && !S.closing; }
static void close_act(int a, int b) { (void)a; (void)b; phone_streams_close(); }
static void chip_act(int g, int b) { (void)b; S.filter = g; rebuild(); scroller_set(&S.list, 0); }
static void play_act(int i, int b) { (void)b; if (i < 0 || i >= S.n) return; Stream s = *S.items[i];
  if (!s.url[0] && s.ext[0]) { if (!plat_open_url(s.ext)) toast(T("Could not open the link"), 2000, NULL); return; } /* a web page: the phone's browser */
  S.open = 0; S.closing = 0; if (app_overlay == &sheet) app_overlay = NULL; tween_set(&S.k, 0);
  app_play(&s, S.type, S.id, S.vid, 0); }

static int key(int k) { if (k == K_BACK) phone_streams_close(); return 1; }
static void tick(float dt) { tween_tick(&S.k, dt); scroller_tick(&S.list, dt); scroller_tick(&S.chips, dt);
  if (S.closing && S.k.v <= 0.001f) { S.open = S.closing = 0; groups_clear(); S.n = 0; if (app_overlay == &sheet) app_overlay = NULL; } }

/* a row's height for its text (the name's lines, the details' lines) */
static void row_text(int i, const char **name, const char **desc) { const Stream *s = S.items[i]; *name = s->name; *desc = s->title[0] ? s->title : s->filename; if (!s->url[0] && s->ext[0]) *desc = T("Opens in your browser"); }
static float row_h(int i, float w) { const char *nm, *ds; row_text(i, &nm, &ds); int a = text_wrap_count(nm, w, 2, PH_TX_BODY + 1, 700, 0), b = *ds ? text_wrap_count(ds, w, 4, PH_TX_SMALL + 1, 400, 0) : 0; return 12 + 18 + a * 20 + (b ? 4 + b * 18 : 0) + (S.kind[i] == R_BEST && S.bestSum[0] ? 22 : 0) + 12; }
static void draw(void) {
  if (!S.open) return; float k = S.k.v;
  gfx_rect(0, 0, SB_W, SB_H, BLACK_A((int)(150 * k)));
  float H = SB_H * 0.86f, top = SB_H - H * k, W = SB_W, x0 = 0;
  if (W > 640) { x0 = (SB_W - 640) / 2; W = 640; } /* a tablet: a column in the middle */
  hit_act(0, 0, SB_W, top, close_act, 0, 0);
  gfx_rrect(x0, top, W, H + 20, 18, PH_SURF); hit_shield(x0, top, W, H);
  gfx_rrect(x0 + W / 2 - 18, top + 8, 36, 4, 2, WHITE_A(60));
  /* the head */
  float y = top + 22;
  text_draw(S.title, x0 + PH_M, y, PH_TX_ROW + 1, 800, PH_TXT, -0.2f, W - 2 * PH_M - 44, ALIGN_LEFT);
  int pr = touch_pressed(NULL, close_act, 0, 0); gfx_circle(x0 + W - PH_M - 16, y + 12, 16, pr ? WHITE_A(60) : WHITE_A(22)); icon_draw("ph_close", x0 + W - PH_M - 26, y + 2, 20, PH_TXT);
  hit_act(x0 + W - PH_M - 40, y - 8, 48, 44, close_act, 0, 0);
  y += 28; if (S.label[0]) { text_draw(S.label, x0 + PH_M, y, PH_TX_SMALL + 1, 500, PH_TXT2, 0, W - 2 * PH_M, ALIGN_LEFT); y += 22; }
  /* the chips */
  { float cx = x0 + PH_M - S.chips.off, c0 = cx; touch_region(x0, y, W, 40, &S.chips, NULL); gfx_clip_push(x0, y - 2, W, 40);
    for (int g = -1; g < S.ng; g++) { if (g >= 0 && !S.g[g].n) continue; const char *t = g < 0 ? T("All") : S.g[g].name; float w = text_width(t, PH_TX_SMALL + 1, 700, 0) + 26; if (w > 200) w = 200;
      int on = S.filter == g, p = touch_pressed(NULL, chip_act, g, 0); gfx_rrect(cx, y, w, 32, 16, on ? WHITE_A(255) : p ? WHITE_A(50) : WHITE_A(22));
      text_draw(t, cx + w / 2, y + 16 - text_line_height(PH_TX_SMALL + 1) / 2.0f, PH_TX_SMALL + 1, 700, on ? RGB(12, 11, 17) : PH_TXT, 0, w - 16, ALIGN_CENTER); hit_act(cx, y, w, 32, chip_act, g, 0); cx += w + 8; }
    gfx_clip_pop(); scroller_limits(&S.chips, 0, cx - c0 - 8 + 2 * PH_M - W); y += 44; }
  /* the list */
  float ly = y, lh = SB_H - y, rw = W - 2 * PH_M - 24; touch_region(x0, ly, W, lh, NULL, &S.list); gfx_clip_push(x0, ly, W, lh);
  float yy = ly - S.list.off + 4;
  for (int i = 0; i < S.n; i++) { float h = row_h(i, rw);
    if (yy + h > ly && yy < SB_H) { const Stream *s = S.items[i]; const char *nm, *ds; row_text(i, &nm, &ds); int p = touch_pressed(NULL, play_act, i, 0);
      gfx_rrect(x0 + PH_M, yy, W - 2 * PH_M, h - 8, 12, p ? WHITE_A(34) : S.kind[i] != R_PLAIN ? col_alpha(ACC | 0xff000000u, 0.22f) : WHITE_A(10));
      float tx = x0 + PH_M + 12, ty = yy + 10; const char *head = S.kind[i] == R_RESUME ? T("Last played") : S.kind[i] == R_BEST ? T("Best for this device") : s->addon;
      text_draw(head, tx, ty, PH_TX_SMALL, 700, S.kind[i] != R_PLAIN ? col_mix(ACC, 0xffffffffu, 0.35f) : PH_TXT2, 0.2f, rw, ALIGN_LEFT); ty += 18;
      int a = text_draw_wrapped(nm, tx, ty, rw, 2, PH_TX_BODY + 1, 700, PH_TXT, 0, ALIGN_LEFT, 20); ty += a * 20;
      if (*ds) { ty += 4; int b = text_draw_wrapped(ds, tx, ty, rw, 4, PH_TX_SMALL + 1, 400, PH_TXT2, 0, ALIGN_LEFT, 18); ty += b * 18; }
      if (S.kind[i] == R_BEST && S.bestSum[0]) text_draw(S.bestSum, tx, ty + 4, PH_TX_SMALL, 600, PH_TXT2, 0, rw, ALIGN_LEFT);
      hit_act(x0 + PH_M, yy, W - 2 * PH_M, h - 8, play_act, i, 0); }
    yy += h; }
  if (S.loading || !S.n) { char m[120]; if (S.loading && S.asked) snprintf(m, sizeof m, "%s (%d)", T("Asking your addons"), S.pending); else snprintf(m, sizeof m, "%s", S.loading ? T("Asking your addons") : T("No streams found.")); text_draw(m, x0 + W / 2, yy + 14, PH_TX_BODY, 600, PH_TXT2, 0, W - 32, ALIGN_CENTER); yy += 50; }
  gfx_clip_pop();
  scroller_limits(&S.list, 0, yy + S.list.off - ly + 16 - lh);
}
static Screen sheet = { NULL, NULL, key, tick, draw, NULL, NULL, NULL, NULL };
