#include "screens/board.h"
#include "screens/btrailer.h"
#include "core/keys.h"
#include "core/i18n.h"
#include "core/tmdb.h"
#include "ui/glass.h"
#include "ui/icon.h"
#include "ui/gfx.h"
#include "ui/shared.h"
#include "ui/text.h"
#include "ui/img.h"
#include "ui/widgets.h"
#include "ui/sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "core/hit.h"
#include "screens/player.h"
#include "screens/lib.h"
#include "screens/catgrid.h"
#include "core/store.h"
#include "core/colstore.h"
#include "core/httpd.h"
#include "core/recs.h"
#include "core/pace.h"
#include "platform/plat.h"
#include "core/bgtrailer.h"

#define MAXROWS 64 /* (a catalog answer packs its row in 6 bits: 64 is the most) */
#define SPOT_ROW 255                 /* (a details answer for a spotlight title, in place of its row) */
#define ROWS_TOP 478.0f             /* a focused row ends ~80 px above the bottom edge */
#define HERO_X 48.0f
#define HERO_Y 100.0f
#define HERO_W (SB_W * 0.52f)
/* the boards' cards: fewer, bigger (about five and a half across, one row in full view) */
#define RC_W 300.0f
#define RC_H 450.0f
#define RW_W 500.0f
#define RW_H 281.0f
#define RGAP 28.0f
#define DESC_PX 27
#define DESC_LH (27 * 1.5f)
#define META_PX 25
/* the cinematic Home (Settings > Home layout): a framed billboard on top (the Top picks, turning by themselves);
   in the rows the focused row docks under the bar, its card opens wide at once with its logo, and its details sit
   under it; no full-screen backdrop (cheaper to draw, too). Reference: so/recs-mockups A and B. */
#define BILL_X 48.0f
#define BILL_Y 112.0f
#define BILL_W (SB_W - 96.0f)
#define BILL_H 816.0f
#define HOME_SPOT_DY 466.0f  /* the rows under the billboard: the first row's title just below it */
#define CINE_TOP 104.0f       /* the focused row's top, docked under the bar */
#define CINE_DET_H 150.0f     /* the details under the open card */
#define CINE_DESC_W 1100.0f
#define RANK_W 130.0f         /* a Top 10 card's numeral, in front of it */
/* cp: each card's resolved picture and words beside its item (cpSt 0 not yet, 1 final, 2 its translation may still
   come); pf: the ranges last fetched ahead (per call site), fetched again only when they move or a picture changes */
typedef struct { char title[96]; ItemList items; int col, wide, label, hidden, closing; float hk; double closeAt; Tween off, a; FocusPop pop; int popCol; Nudge nudge; double locAt, wideAt;
  CardPrep *cp; unsigned char *cpSt; int ncp; unsigned cpGen, cpLg; int pf[3][2]; unsigned char pfWait[3]; int pfOpen; /* pfOpen: col + 1 when the open card's wide pictures around it were last asked for */
  /* a catalog that pages: where its next page starts (the items its addon sent so far), one page asked at a time */
  CatalogDef def; int hasDef, skipAt, more, paging; double pageAt;
  /* a collection's row: tiles, not titles (no Item, so no details, trailer or hold menu come near them) */
  const ColFolder *tiles; int ntiles; int answered;
  int recsIdx, nrec; /* a For You row: its row in recs.c (-1 other rows) and its card count; its items are loaded only near the focus (rows_window) */
  int rank; float extra; /* rank: a Top 10 row (numerals); extra: the details under the open card (cinematic Home) */ } Row;
#define NFEAT 8
/* what the hero shows: put in while it is invisible, all together */
typedef struct { char id[64], name[256], metaName[256], metaLine[96], desc[1024], logo[512]; int wait; unsigned sig; } HeroView;
/* what the drawing measures in the view, made once per view (and language): wrap counts, the meta line */
typedef struct { unsigned ver, lg; int spot, discName, discDesc, homeName; MetaParts mp; } HeroLay;
typedef struct {
  int kind;                       /* 0 home, 1 disc */
  Row rows[MAXROWS]; int n, rowIdx, built, winRow; unsigned gen, libVer;
  /* Discover's spotlight: this year's films and the trending series, alternating; it moves on every 4 s */
  Item feat[NFEAT]; int nfeat, featIdx, featOn, featLoad, featGotM, featGotS; ItemList featM, featS; double featAt;
  Tween spotY, rowsA, spotA, dotsA, dotW[NFEAT]; int heroSpot, dotsWant; double dotsGo; char scoped[64];
  Tween hostY; Timer1 fade; Nudge nudge;
  /* the focused card opens after a rest (as Nuvio's): wider, its backdrop over its poster, then the trailer in it */
  Tween exp; int expRow, expCol; double wideBt, wideSt;
  double heroAt, fadeHold; int heroPending, fastBg; /* fadeHold: the fade-in waits (a moment at most) for the whole description */
  /* the hero, in one path: fade out, gather (details, translation, logo: 0.45 s at most), put in, fade in; anything
     better that arrives later comes in with one more fade */
  Item hero; int heroSet, heroShown, heroPhase; Tween heroA; double heroT0, laterAt, putAt; HeroView view; Tween logoIn; TmdbWait hw; int aheadDone;
  /* the view is compared with what it would be now only when something it reads moved: the hero item (heroVer), its
     translation, its logo's state */
  unsigned heroVer, inVer, viewVer; const void *inL, *inLn, *inLo, *inLl; char inLogo[512]; int inSt, inWait; HeroLay lay;
  /* a title just taken out of Continue Watching: it fades and shrinks where it was while the ones after close the gap */
  Item remIt; int remRow, remIdx; double remAt;
  /* the cinematic Home: its billboard (the Top picks in feat[]), the button in focus on it, its picture crossfading */
  int bill, cine, billBtn; unsigned recsVer; Tween topY; char billCur[512], billPrev[512]; double billSwitch;
} Board;
enum { HP_IDLE, HP_GATHER, HP_LATER_WAIT, HP_LATER_OUT };
static Board boards[3]; /* Home, Discover, a collection tile's page */
static const ColFolder *colFolder; static double colAt;
static Board *cur(void) { return SCR == SCR_HOME ? &boards[0] : SCR == SCR_DISC ? &boards[1] : SCR == SCR_COL ? &boards[2] : NULL; }
static int spotk(const Board *b) { return b->kind == 1 || (b->kind == 0 && b->bill && b->cine); } /* a spotlight: Discover's, or Home's billboard */
static int spotOn(Board *b) { return spotk(b) && b->featOn; } /* the spotlight has the focus (its titles may still be on the way) */
static int focusOn(Board *b) { return cur() == b && !topNav.active && !spotOn(b); }
static float card_w(const Row *r) { return r->wide ? RW_W : RC_W; }
static float card_h(const Row *r) { return r->wide ? RW_H : RC_H; }
static float row_h(const Row *r) { return (18 + text_line_height(31) + 26 + card_h(r) + 34 + r->extra) * (r->closing ? r->hk : 1); } /* (a row closing: its height goes as the ones under it move up) */
static float step_of(const Row *r) { return card_w(r) + RGAP + (r->rank ? RANK_W : 0); }
/* a row that pages ends, at SEEALL_AT titles at most, in a See All card (one place past its items: no Item, so the hero,
   the details and the hold menu never see it); the whole catalog is on the grid it opens */
#define SEEALL_AT 60
#define CARD_OPEN_AFTER 3.0f
#define CINE_OPEN_MIN 0.12f /* the cinematic card at its quickest: the row has settled under a held arrow */
/* the cinematic card's rest before it opens (Settings > For You > Card opens after), read again only when a setting changes */
static float cine_open_after(void) { static unsigned ser; static float v = 0.25f; if (store_serial() != ser) { ser = store_serial(); v = (float)atof(store_get("cardDelay", "250")) / 1000.0f; if (v < CINE_OPEN_MIN) v = CINE_OPEN_MIN; if (v > 3) v = 3; } return v; }
#define CARD_OPEN_W 800.0f /* 16:9 at a poster's height */
#define WIDE_SM_W 320
#define WIDE_SM_H 180
static int row_n(const Row *r) { return r->recsIdx >= 0 ? r->nrec : r->items.n; } /* (a For You row far away has no items loaded: its count is known all the same) */
static int see_all(const Row *r) { return r->hasDef && r->def.skip && r->items.n > 0 && !r->more; }
static int col_last(const Row *r) { return r->ntiles ? r->ntiles - 1 : r->items.n - 1 + see_all(r); }
static const ColFolder *tile_cur(const Board *b) { if (b->rowIdx < 0 || b->rowIdx >= b->n) return NULL; const Row *r = &b->rows[b->rowIdx]; return r->ntiles && r->col < r->ntiles ? &r->tiles[r->col] : NULL; }
static void see_all_open(const Row *r) { CatParams p; memset(&p, 0, sizeof p); p.def = r->def; snprintf(p.title, sizeof p.title, "%s", T(r->title)); app_go(SCR_CAT, &p, 0); }
static const Item *focused_of(Board *b) { if (spotOn(b) && b->nfeat) return &b->feat[b->featIdx % b->nfeat]; if (b->rowIdx < 0 || b->rowIdx >= b->n) return NULL; Row *r = &b->rows[b->rowIdx]; return r->col < r->items.n ? &r->items.items[r->col] : NULL; }
const Item *board_next_item(void) { Board *b = cur(); if (!b || topNav.active) return NULL; if (spotOn(b)) return b->kind == 0 && b->nfeat > 1 ? &b->feat[(b->featIdx + 1) % b->nfeat] : NULL; /* (the billboard's next title: its link ready before it turns) */ if (b->rowIdx >= b->n) return NULL; Row *r = &b->rows[b->rowIdx]; return r->col + 1 < r->items.n ? &r->items.items[r->col + 1] : NULL; }
float board_open_wait(void) { Board *b = cur(); return b && b->kind == 0 && b->cine && focusOn(b) ? cine_open_after() - CINE_OPEN_MIN : 0; } /* (the clip in the card waits as long as it does) */
int board_on_billboard(void) { Board *b = cur(); return b && !topNav.active && spotOn(b) && b->kind == 0 && b->cine && b->bill; }
int board_near_items(const Item **out, int max) { Board *b = cur(); int n = 0; if (!b || topNav.active) return 0;
#define NEAR_ADD(rw, i) do { if ((i) >= 0 && (i) < (rw)->items.n && n < max) out[n++] = &(rw)->items.items[i]; } while (0)
  if (spotOn(b)) { for (int k = 1; k <= 2 && b->kind == 0 && k < b->nfeat; k++) if (n < max) out[n++] = &b->feat[(b->featIdx + k) % b->nfeat]; if (b->n) for (int i = 0; i < 4; i++) NEAR_ADD(&b->rows[0], i); return n; }
  if (b->rowIdx < 0 || b->rowIdx >= b->n) return 0;
  { const Row *r = &b->rows[b->rowIdx]; for (int d = 1; d <= 5; d++) NEAR_ADD(r, r->col + d); NEAR_ADD(r, r->col - 1); }
  for (int dr = -1; dr <= 1; dr += 2) { int ri = b->rowIdx + dr; if (ri < 0 || ri >= b->n) continue; const Row *q = &b->rows[ri]; NEAR_ADD(q, q->col); NEAR_ADD(q, q->col + 1); }
  return n; }
int board_spot_up(void) { Board *b = cur(); return b && b->featOn && !topNav.active && !(b->kind == 0 && b->cine && b->bill); } /* (a spotlight with no place for a video; the cinematic billboard has one) */

/* ---- rows ---- */
static void prep_free(Row *r) { free(r->cp); free(r->cpSt); r->cp = NULL; r->cpSt = NULL; r->ncp = 0; for (int k = 0; k < 3; k++) { r->pf[k][0] = r->pf[k][1] = -1; r->pfWait[k] = 0; } r->pfOpen = 0; }
static void row_clear(Row *r) { itemlist_clear(&r->items); prep_free(r); }
static Row *row_add(Board *b, const char *title, int wide, int label) {
  if (b->n >= MAXROWS) return NULL; Row *r = &b->rows[b->n++]; row_clear(r); memset(r, 0, sizeof *r); prep_free(r);
  snprintf(r->title, sizeof r->title, "%s", title); r->wide = wide; r->label = label; r->hidden = 1; r->closing = 0; r->hk = 1; r->recsIdx = -1; r->popCol = -1; tween_set(&r->off, 0); tween_set(&r->a, 1); return r;
}
static void slide_row(Row *r) {
  float w = step_of(r); int vis = (int)((SB_W - 96) / w);
  int k = r->col - (vis - 2); if (k < 0) k = 0; float off = k * w; if (r->col <= 0) off = 0;
  tween_to(&r->off, off, 0.18f, EASE_ROW);
  if (r->popCol != r->col) { r->popCol = r->col; focus_pop(&r->pop); } /* the floating copy pops on every move */
}
static void board_cw_row(Board *b) {
  ItemList cw; if (lib_cw_items(&cw) > 0) { Row *r = row_add(b, "Continue Watching", 1, 0); if (r) { r->items = cw; r->hidden = 0; } else itemlist_clear(&cw); } else itemlist_clear(&cw);
}
/* a catalog answer finds its row through a packed token (board, row, generation): nothing to free */
static void *tok(Board *b, int row) { return (void *)(uintptr_t)(((uintptr_t)(b - boards) << 28) | ((uintptr_t)(b->gen & 0x3fffff) << 6) | (uintptr_t)row); }
static void col_search(Board *b);
static int colPend, colSearched;
static void on_catalog(ItemList *l, int cached, void *ud) {
  (void)cached; uintptr_t t = (uintptr_t)ud; Board *b = &boards[(t >> 28) & 3]; int row = t & 63; unsigned gen = (t >> 6) & 0x3fffff;
  if ((b->gen & 0x3fffff) != gen || row >= b->n) { itemlist_free(l); return; }
  Row *r = &b->rows[row]; row_clear(r); r->items = *l; free(l); r->locAt = 0;
  r->skipAt = r->items.total > r->items.n ? r->items.total : r->items.n; r->more = r->hasDef && r->def.skip && r->items.n > 0; r->paging = 0;
  r->hidden = r->items.n == 0; if (r->col >= r->items.n) r->col = r->items.n ? r->items.n - 1 : 0;
  if (b->kind == 2 && !r->hidden && b->rows[b->rowIdx].hidden) b->rowIdx = row; /* (a tile's page: the focus waits on the first row that came) */
  if (b->kind == 2 && !r->answered) { r->answered = 1; if (--colPend <= 0) { int any = 0; for (int i = 0; i < b->n && !any; i++) any = !b->rows[i].hidden; if (!any) col_search(b); } }
  if (cur() == b) { slide_row(r); b->heroPending = b->heroPending || (b->rowIdx == row); if (b->rowIdx == row && !b->heroPending) { b->heroAt = app_now(); b->heroPending = 1; } }
}
static int first_visible_row(Board *b, int from, int dir);
/* every row of a tile's page came back empty, or none could be made (its addon is not installed, its lists are gone):
   what the installed addons find under the tile's name, instead of an empty page. Once per page */
static void col_search(Board *b) {
  if (colSearched || !colFolder) return; colSearched = 1; static CatalogDef x[8]; int n = sb_search_defs(colFolder->title, x, 8);
  for (int i = 0; i < n; i++) { char ti[96]; snprintf(ti, sizeof ti, "%s Â· %s", colFolder->title, !strcmp(x[i].type, "movie") ? T("Movies") : !strcmp(x[i].type, "series") ? T("Series") : x[i].title); Row *r = row_add(b, ti, 0, 0); if (!r) break; r->def = x[i]; r->hasDef = 1; colPend++; sb_catalog_url(x[i].url, x[i].type, 60, on_catalog, tok(b, (int)(r - b->rows))); }
}
/* a tile's page: a row per source of the tile, then the catalogs of any installed addon that carry its name */
static void col_rows(Board *b, const ColFolder *f) {
  for (int i = 0; i < b->n; i++) row_clear(&b->rows[i]); b->n = 0; b->gen++; b->rowIdx = 0; colAt = app_now(); colPend = 0; colSearched = 0; if (!f) return;
  static CatalogDef x[MAXROWS]; int nx = 0;
  for (int s = 0; s < f->nsrc && nx < MAXROWS; s++) { const ColSource *c = &f->src[s]; const char *ty = c->type; char ti[96], lb[24]; snprintf(lb, sizeof lb, "%s", !strcmp(ty, "movie") || !strcmp(ty, "movies") ? T("Movies") : !strcmp(ty, "series") ? T("Series") : ty); if (lb[0] >= 'a' && lb[0] <= 'z') lb[0] = (char)(lb[0] - 32);
    int same = 1; snprintf(ti, sizeof ti, "%s · %s", f->title, lb); for (int k = 0; k < nx; k++) if (!strncmp(x[k].title, ti, strlen(ti))) same++; if (same > 1) { char t2[96]; snprintf(t2, sizeof t2, "%.86s %d", ti, same); snprintf(ti, sizeof ti, "%s", t2); }
    if (sb_source_def(c->addonId, c->base, ty, c->catalog, c->genre, ti, &x[nx])) nx++; }
  CatalogDef named[12]; int nn = sb_name_defs(f->title, named, 12);
  for (int i = 0; i < nn && nx < MAXROWS; i++) { int dup = 0; for (int k = 0; k < nx && !dup; k++) dup = !strcmp(x[k].path, named[i].path); if (!dup) x[nx++] = named[i]; }
  colPend = nx; for (int i = 0; i < nx; i++) { Row *r = row_add(b, x[i].title, 0, 0); if (!r) break; r->def = x[i]; r->hasDef = 1; sb_catalog_url(x[i].url, x[i].type, 60, on_catalog, tok(b, (int)(r - b->rows))); }
  if (!nx) col_search(b);
}
/* Home's head: What to watch next (its whole first page kept, so its paging leaves no gap) */
static void pinned_rows(Board *b) {
  { CatalogDef x[4]; int n = sb_pinned_defs(x, 4); for (int i = 0; i < n; i++) { Row *r = row_add(b, x[i].title, 0, 0); if (!r) return; r->def = x[i]; r->hasDef = 1; sb_catalog_url(x[i].url, x[i].type, 60, on_catalog, tok(b, (int)(r - b->rows))); } }
}
/* Discover's head, under its spotlight: the collections, the pinned first */
static void col_disc_rows(Board *b, int keep, const int *oldCols) {
  const ColSet *s = colstore();
  for (int pass = 1; pass >= 0; pass--) for (int i = 0; i < s->n; i++) { const Collection *c = &s->c[i]; if (!c->pinTop != !pass || !c->nfolders) continue; Row *r = row_add(b, c->title, c->folders[0].shape == COL_LANDSCAPE, 0); if (!r) return; r->tiles = c->folders; r->ntiles = c->nfolders; r->hidden = 0; if (keep && oldCols[r - b->rows] < r->ntiles) r->col = oldCols[r - b->rows]; }
}
/* Home's For You rows (recs.c): only their titles and counts here; the cards are loaded near the focus (rows_window) */
static void home_recs_rows(Board *b) {
  char t[160]; int rk, cnt;
  for (int i = 0, n = recs_rows(); i < n; i++) { if (!recs_row_info(i, t, sizeof t, &rk, &cnt) || cnt <= 0) continue; Row *r = row_add(b, t, 0, 0); if (!r) break; r->hidden = 0; r->rank = rk; r->recsIdx = i; r->nrec = cnt; }
}
/* the window: a For You row holds its cards only within WIN_LOAD rows of the focus and lets them go beyond WIN_DROP (the gap stops thrashing);
   run when the focused row changes and after a rebuild, not every frame (a load is a memcpy of 20 Items) */
#define WIN_LOAD 3
#define WIN_DROP 6
static void row_load(Row *r) { if (r->recsIdx < 0 || r->items.n) return; recs_row_items(r->recsIdx, &r->items); r->nrec = r->items.n; if (r->col >= r->items.n) r->col = r->items.n ? r->items.n - 1 : 0; }
static void rows_window(Board *b) {
  b->winRow = b->rowIdx; if (b->kind != 0) return;
  for (int i = 0; i < b->n; i++) { Row *r = &b->rows[i]; if (r->recsIdx < 0) continue; int d = abs(i - b->rowIdx);
    if (d <= WIN_LOAD) row_load(r); else if (d > WIN_DROP && r->items.n) { itemlist_clear(&r->items); prep_free(r); } }
}
/* the billboard's titles: the Top picks (none: no billboard, Home as before) */
static void bill_fill(Board *b, int first) {
  ItemList l; int n = recs_billboard(&l); b->nfeat = 0;
  for (int i = 0; i < n && b->nfeat < NFEAT; i++) b->feat[b->nfeat++] = l.items[i];
  itemlist_clear(&l); b->recsVer = recs_version();
  int was = b->bill; b->bill = b->nfeat > 0;
  if (!b->bill) { b->featOn = 0; return; }
  if (b->featIdx >= b->nfeat) b->featIdx = 0;
  for (int i = 0; i < NFEAT; i++) tween_set(&b->dotW[i], i == b->featIdx ? 34 : 10);
  if ((first || !was) && b->cine) b->featOn = 1; /* Home opens on its billboard */
}
static void load_catalog_rows(Board *b, int disc, int keep) {
  if (b->kind == 2) { col_rows(b, colFolder); return; }
  int oldRow = b->rowIdx, oldCols[MAXROWS] = { 0 }; /* (rows new since: at their start) */ for (int i = 0; i < b->n; i++) oldCols[i] = b->rows[i].col;
  Item *was = NULL; int nwas = 0; /* Continue Watching as it was, to find one taken out of it */
  if (keep && !disc && b->n && !strcmp(b->rows[0].title, "Continue Watching") && b->rows[0].items.n) { nwas = b->rows[0].items.n; was = malloc(sizeof(Item) * (size_t)nwas); if (was) memcpy(was, b->rows[0].items.items, sizeof(Item) * (size_t)nwas); else nwas = 0; }
  for (int i = 0; i < b->n; i++) row_clear(&b->rows[i]); b->n = 0; b->gen++; b->winRow = -1;
  if (!disc) { b->cine = b->kind == 0 && strcmp(store_get("homeLayout", "cinematic"), "classic"); bill_fill(b, !keep); board_cw_row(b); home_recs_rows(b); }
  if (!disc && !was) pinned_rows(b);
  if (was) { Row *r = b->n && !strcmp(b->rows[0].title, "Continue Watching") ? &b->rows[0] : NULL;
    if (r && r->items.n == nwas - 1) for (int i = 0; i < nwas; i++) { int in = 0; for (int j = 0; j < r->items.n && !in; j++) in = !strcmp(r->items.items[j].id, was[i].id);
      if (!in) { b->remIt = was[i]; b->remRow = 0; b->remIdx = i; b->remAt = app_now(); break; } }
    if (!r && nwas == 1) { Row *z = row_add(b, "Continue Watching", 1, 0); /* its last title taken out: the row stays a moment, the title fading and shrinking in it, then closes as the rows under it move up */
      if (z) { z->hidden = 0; z->closing = 1; z->closeAt = app_now(); z->hk = 1; b->remIt = was[0]; b->remRow = (int)(z - b->rows); b->remIdx = 0; b->remAt = app_now(); } }
    free(was); pinned_rows(b); }
  if (disc) col_disc_rows(b, keep, oldCols);
  CatalogDef defs[12]; int nd = disc ? sb_disc_defs(defs, 12) : sb_home_defs(defs, 12);
  for (int d = 0; d < nd; d++) { Row *r = row_add(b, defs[d].title, 0, 0); if (!r) break; int ri = (int)(r - b->rows); if (keep && ri < MAXROWS) r->col = oldCols[ri]; r->def = defs[d]; r->hasDef = 1; sb_catalog(&defs[d], on_catalog, tok(b, ri)); }
  if (keep) { b->rowIdx = oldRow < b->n ? oldRow : (b->n ? b->n - 1 : 0); if (b->rowIdx < b->n && b->rows[b->rowIdx].closing) { int nx = first_visible_row(b, b->rowIdx + 1, 1); if (nx >= 0) b->rowIdx = nx; } /* (never on the row closing) */ for (int j = 0; j < b->n; j++) { Row *r = &b->rows[j]; if (row_n(r) && r->col >= row_n(r)) r->col = row_n(r) - 1; } }
  else b->rowIdx = 0;
}
/* ---- the spotlight's own picks: this year's new films and the trending series, alternating (not the first row again) ---- */
static void hero_soon(Board *b, int ms);
static void logos_ahead(Board *b, int force);
static int feat_keep(const Item *it) { return it->id[0] && (it->background[0] || it->poster[0]); }
static void feat_done(Board *b) {
  if (!b->featGotM || !b->featGotS || b->nfeat) return; /* both lists in, once (a cached list answers again when refreshed) */
  int mv[4], sr[64], nm = 0, ns = 0;
  for (int i = 0; i < b->featM.n && nm < 4; i++) if (feat_keep(&b->featM.items[i])) mv[nm++] = i;
  for (int i = 0; i < b->featS.n && ns < 64; i++) if (feat_keep(&b->featS.items[i])) sr[ns++] = i;
  int srr[64], k = 0; for (int i = 4; i < ns; i++) srr[k++] = sr[i]; for (int i = 0; i < 4 && i < ns; i++) srr[k++] = sr[i]; /* a little further down the trending list than the row below */
  for (int i = 0; i < 4; i++) { if (i < nm && b->nfeat < NFEAT) b->feat[b->nfeat++] = b->featM.items[mv[i]]; if (i < k && b->nfeat < NFEAT) b->feat[b->nfeat++] = b->featS.items[srr[i]]; }
  itemlist_clear(&b->featM); itemlist_clear(&b->featS);
  if (!b->nfeat) { b->featLoad = 0; return; }
  b->featIdx = 0; for (int i = 0; i < NFEAT; i++) tween_set(&b->dotW[i], i == 0 ? 34 : 10);
  logos_ahead(b, 1); /* the spotlight's text art, all of it, now: it shows each title's art the moment it comes round */
  if (b->featOn && cur() == b) hero_soon(b, 0);
}
static void on_feat(ItemList *l, int cached, void *ud) {
  (void)cached; Board *b = &boards[1]; int series = (int)(uintptr_t)ud;
  if (b->nfeat || (series ? b->featGotS : b->featGotM)) { itemlist_free(l); return; }
  if (series) { b->featS = *l; b->featGotS = 1; } else { b->featM = *l; b->featGotM = 1; } free(l);
  feat_done(b);
}
static void feat_fill(Board *b) {
  if (b->kind != 1 || b->nfeat || b->featLoad) return; b->featLoad = 1; b->featGotM = b->featGotS = 0;
  time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); char u[200];
  snprintf(u, sizeof u, CINEMETA "/catalog/movie/year/genre=%d.json", tm.tm_year + 1900); sb_catalog_url(u, "movie", 20, on_feat, (void *)0);
  sb_catalog_url(CINEMETA "/catalog/series/top.json", "series", 20, on_feat, (void *)1);
}
static void feat_arm(Board *b) { if (spotk(b)) b->featAt = app_now() + (b->kind == 0 ? 8 : 4); } /* it moves on by itself every 4 s while you rest on it */
static void feat_go(Board *b, int step) { int n = b->nfeat; if (!n) return; b->featIdx = (b->featIdx + step + n) % n; hero_soon(b, 1); feat_arm(b); }
static int first_visible_row(Board *b, int from, int dir) { for (int i = from; i >= 0 && i < b->n; i += dir) if (!b->rows[i].hidden && !b->rows[i].closing) return i; return -1; }

/* ---- the hero ---- */
static void hero_soon(Board *b, int ms) { b->heroAt = app_now() + ms / 1000.0; b->heroPending = 1;
  const Item *it = focused_of(b); if (it) { char pu[512], lu[512]; accent_tint_fetch(it->id, sb_poster_of(it, pu, sizeof pu), sb_logo_of(it, lu, sizeof lu)); } }
static Item *item_at(Board *b, int row, int col) {
  if (row == SPOT_ROW) return col < b->nfeat ? &b->feat[col] : NULL; /* a spotlight title */
  if (row >= b->n || col >= b->rows[row].items.n) return NULL; return &b->rows[row].items.items[col];
}
static void on_meta(cJSON *m, void *ud) {
  uintptr_t t = (uintptr_t)ud; Board *b = &boards[(t >> 28) & 3]; int row = (t >> 0) & 255, col = (t >> 8) & 0xfff; unsigned gen = (t >> 20) & 0xff;
  if (!m || (row != 15 && (b->gen & 0xff) != gen)) return; Item *it = item_at(b, row, col); if (!it) return;
  if (!it->description[0]) { const char *d = cJSON_GetStringValue(cJSON_GetObjectItem(m, "description")); if (d) snprintf(it->description, sizeof it->description, "%s", d); }
  /* the details' facts are what the title page shows (a catalog can list a series' total running time where the
     details give an episode's): the line follows them, keeping the catalog's rating when the details have none */
  { char dl[96]; sb_meta_line(m, dl, sizeof dl); if (dl[0]) { const char *oi = strstr(it->metaLine, "IMDb "); if (!strstr(dl, "IMDb") && oi) { size_t l = strlen(dl); snprintf(dl + l, sizeof dl - l, "   %s", oi); }
      if (strcmp(dl, it->metaLine)) snprintf(it->metaLine, sizeof it->metaLine, "%s", dl); } }
  if (b->heroSet && !strcmp(b->hero.id, it->id)) { snprintf(b->hero.description, sizeof b->hero.description, "%s", it->description); snprintf(b->hero.metaLine, sizeof b->hero.metaLine, "%s", it->metaLine); b->heroVer++; }
}
static void meta_ask(Board *b, int row, int col) { Item *it = item_at(b, row, col); if (!it || !it->id[0]) return; /* (the details also when the catalog gave words: their facts are the title page's) */
  sb_meta(it->type, it->id, on_meta, (void *)(uintptr_t)(((uintptr_t)(b - boards) << 28) | ((uintptr_t)(b->gen & 0xff) << 20) | ((uintptr_t)col << 8) | (uintptr_t)row)); }
static void logo_src(const Item *it, char *out, size_t cap) { const TmdbLoc *L = tmdb_loc(it->type, it->id, 1); if (L && L->logo && L->logo[0]) snprintf(out, cap, "%s", L->logo); else sb_logo_of(it, out, cap); }
/* the text art of the titles the hero may show next, asked for well before (at the exact size it is drawn: one
   decode, ready when the hero changes): every spotlight title, and the cards around the focus in its row and the
   rows next to it. Asked again every half second, so translated art that turns up later is fetched too and what
   is near stays recently used */
static void logos_ahead(Board *b, int force) {
  static double at[3]; double now = app_now(); int bi = (int)(b - boards); if (!force && now - at[bi] < 0.5) return; at[bi] = now;
  char lu[512];
  for (int i = 0; i < b->nfeat; i++) { logo_src(&b->feat[i], lu, sizeof lu); if (lu[0]) img_prefetch_pri(lu, 520, 175); }
  for (int ri = b->rowIdx - 1; ri <= b->rowIdx + 2; ri++) { if (ri < 0 || ri >= b->n || b->rows[ri].hidden) continue; Row *r = &b->rows[ri];
    for (int c = r->col - 2; c <= r->col + 8; c++) { if (c < 0 || c >= r->items.n) continue; logo_src(&r->items.items[c], lu, sizeof lu); if (lu[0]) img_prefetch_pri(lu, 520, 175); } }
}
/* what the hero would show now (the translation and the logo as far as they are in) */
static void hero_view(Board *b, HeroView *v) {
  const Item *it = &b->hero; const TmdbLoc *L = tmdb_loc(it->type, it->id, 1);
  snprintf(v->id, sizeof v->id, "%s", it->id); logo_src(it, v->logo, sizeof v->logo); v->wait = 0;
  if (v->logo[0]) { if (img_failed(v->logo)) v->logo[0] = 0; else if (!img_loaded(v->logo)) { img_get(v->logo, 520, 175, NULL); v->wait = app_now() - b->heroT0 < 1.1; v->logo[0] = 0; } } /* no art by 1.1 s: the name */
  snprintf(v->name, sizeof v->name, "%s", L && L->name && L->name[0] ? L->name : it->name);
  snprintf(v->metaName, sizeof v->metaName, "%s", L && L->name && L->name[0] && !(L->logo && L->logo[0]) && strcmp(L->name, it->name) ? L->name : "");
  snprintf(v->metaLine, sizeof v->metaLine, "%s", it->metaLine);
  snprintf(v->desc, sizeof v->desc, "%s", L && L->overview && L->overview[0] ? L->overview : it->description);
  unsigned h = 2166136261u; const char *parts[6] = { v->logo, v->wait ? "W" : "T", v->logo[0] ? "" : v->name, v->metaName, v->metaLine, v->desc };
  for (int i = 0; i < 6; i++) { for (const char *p = parts[i]; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u; h = (h ^ 0x1f) * 16777619u; }
  v->sig = h;
}
/* the description's lines all rasterised, as each face draws it (the missing ones are asked for) */
static int desc_ready(Board *b, const char *d) { return !d[0] || (b->kind == 1 ? text_wrapped_ready(d, 1080, 2, DESC_PX, 400, 0) : b->cine ? text_wrapped_ready(d, CINE_DESC_W, 2, DESC_PX, 400, 0) : text_wrapped_ready(d, HERO_W, 3, DESC_PX, 400, 0)); }
static const char *hero_key(const Board *b) { static char k[80]; snprintf(k, sizeof k, "logo:%s", b->hero.id); return k; }
static void hero_put(Board *b) {
  int late = b->view.wait; hero_view(b, &b->view); b->viewVer++; b->inVer = b->heroVer - 1; /* the next idle frame compares once */
  if (late && b->view.logo[0]) { tween_set(&b->logoIn, 0); tween_to(&b->logoIn, 1, 0.3f, EASE_CSS_EASEOUT); } /* a logo that came late fades in */
  else if (b->kind == 1 && b->view.logo[0] && !sx_arrived(hero_key(b))) { tween_set(&b->logoIn, 0); tween_to(&b->logoIn, 1, 0.45f, EASE_CSS_EASE); } /* Discover's big logo: a soft fade of its own, never a pop over the picture */
  else tween_set(&b->logoIn, 1);
  b->heroSpot = spotOn(b); b->heroShown = 1; b->aheadDone = 0; /* Discover's layouts come and go with the content, while the hero is invisible */
  if (desc_ready(b, b->view.desc)) { b->fadeHold = 0; tween_to(&b->heroA, 1, 0.20f, EASE_CSS_EASEOUT); } else b->fadeHold = app_now() + 0.15; /* every line in the same fade */
}
static void hero_apply(Board *b) {
  if (cur() != b) return; const Item *it = focused_of(b);
  if (!it) { tween_to(&b->heroA, 0, 0.12f, EASE_CSS_EASEIN); b->heroSet = 0; const ColFolder *tf = tile_cur(b); if (tf && (tf->hero[0] || tf->cover[0])) bg_set(tf->hero[0] ? tf->hero : tf->cover, b->fastBg ? 0 : 400, tf->cover); b->fastBg = 0; return; } /* nothing to show yet: never the previous screen's title */
  int again = b->heroSet && b->heroShown && !strcmp(b->hero.id, it->id) && b->heroPhase == HP_IDLE && b->heroSpot == spotOn(b); /* back from its page: it is already there */
  char bgu[512], pu[512]; sb_bg_mid(it, bgu, sizeof bgu); sb_poster_of(it, pu, sizeof pu);
  int fast = b->fastBg; b->fastBg = 0; /* entering a board: show its backdrop right away */
  float idle = app_idle(); int delay = (app_splash_done && !fast) ? (int)((0.4f - idle) * 1000) : 0; if (delay < 0) delay = 0;
  if (b->kind == 0 && b->cine) bg_set(NULL, 0, NULL); else bg_set(bgu, delay, pu); /* shown ~0.4 s after the last move (the cinematic Home has none) */
  if (spotOn(b) && b->nfeat) { char lu[512]; accent_scope(it->id, pu, sb_logo_of(it, lu, sizeof lu)); snprintf(b->scoped, sizeof b->scoped, "%s", it->id); } /* the spotlight wears each title's colours; its dot moves in the same moment */
  { int sp = spotOn(b) && b->nfeat; meta_ask(b, sp ? SPOT_ROW : b->rowIdx, sp ? b->featIdx % b->nfeat : b->rows[b->rowIdx].col); }
  if (again) { b->hero = *it; b->heroVer++; return; }
  b->hero = *it; b->heroVer++; b->heroSet = 1; b->heroShown = 0; b->heroPhase = HP_GATHER; b->heroT0 = app_now(); b->fadeHold = 0;
  tween_to(&b->heroA, 0, 0.12f, EASE_CSS_EASEIN);
  { char lu[512]; logo_src(it, lu, sizeof lu); if (lu[0]) img_prefetch_pri(lu, 520, 175); } /* (the view shown keeps its own logo until it has faded out: it swapped in under the old words) */
}
/* the neighbours' details and logos while the remote rests: the hero then shows them at once, in one fade */
static void hero_ahead(Board *b) {
  if (b->aheadDone || app_idle() < 0.3f || b->rowIdx >= b->n) return; b->aheadDone = 1;
  char bu[512];
  if (spotOn(b)) { if (b->nfeat > 1) { const Item *nx = &b->feat[(b->featIdx + 1) % b->nfeat]; if (sb_bg_mid(nx, bu, sizeof bu)[0]) img_prefetch(bu, 1280, 720); } return; } /* the spotlight's next title, before it turns */
  Row *r = &b->rows[b->rowIdx];
  for (int d = -2; d <= 3; d++) { int c = r->col + d; if (!d || c < 0 || c >= r->items.n) continue; const Item *it = &r->items.items[c];
    if (d >= -1) { meta_ask(b, b->rowIdx, c); char lu[512]; logo_src(it, lu, sizeof lu); if (lu[0]) img_prefetch_pri(lu, 520, 175); }
    if (d <= 2 && sb_bg_mid(it, bu, sizeof bu)[0]) img_prefetch(bu, 1280, 720); } /* the backdrops two either side, at the size the backdrop draws them */
}
static void hero_tick(Board *b, float dt) {
  tween_tick(&b->heroA, dt); tween_tick(&b->logoIn, dt);
  if (!b->heroSet) return;
  double now = app_now(), el = now - b->heroT0;
  if (b->fadeHold > 0 && (now >= b->fadeHold || desc_ready(b, b->view.desc))) { b->fadeHold = 0; tween_to(&b->heroA, 1, 0.20f, EASE_CSS_EASEOUT); }
  if (b->heroPhase == HP_GATHER) {
    const Item *it = &b->hero; char lu[512]; logo_src(it, lu, sizeof lu);
    int loc = tmdb_wait(&b->hw, it->type, it->id, 1, TMDB_WAIT_HERO), logo = !lu[0] || img_loaded(lu) || img_failed(lu), meta = it->description[0] != 0 && sb_meta_cached(it->type, it->id) != NULL; /* the details too: their facts, not the catalog's, so the line never changes after it shows */
    if (lu[0] && !logo) img_get(lu, 520, 175, NULL);
    if (loc && logo && meta) { const TmdbLoc *L = tmdb_loc(it->type, it->id, 1); meta = desc_ready(b, L && L->overview && L->overview[0] ? L->overview : it->description); } /* its lines rasterised while it is invisible */
    if (el >= 0.13 && ((loc && logo && meta) || el >= 0.45)) { b->heroPhase = HP_IDLE; hero_put(b); } /* the fade-out (0.12 s) is over first */
    return;
  }
  if (b->heroPhase == HP_IDLE && b->heroShown) { hero_ahead(b);
    /* what hero_view reads, cheaply; the view itself is made and compared only when one of them moved */
    const Item *it = &b->hero; const TmdbLoc *L = tmdb_loc(it->type, it->id, 1);
    const void *ln = L ? (const void *)L->name : NULL, *lo = L ? (const void *)L->overview : NULL, *ll = L ? (const void *)L->logo : NULL;
    int moved = b->inVer != b->heroVer || b->inL != L || b->inLn != ln || b->inLo != lo || b->inLl != ll;
    if (moved) { logo_src(it, b->inLogo, sizeof b->inLogo); b->inVer = b->heroVer; b->inL = L; b->inLn = ln; b->inLo = lo; b->inLl = ll; }
    int st = !b->inLogo[0] ? 0 : img_failed(b->inLogo) ? 1 : img_loaded(b->inLogo) ? 2 : 3, wait = st == 3 && app_now() - b->heroT0 < 1.1;
    if (st != b->inSt || wait != b->inWait) { b->inSt = st; b->inWait = wait; moved = 1; }
    if (moved) { HeroView v; hero_view(b, &v); if (v.sig != b->view.sig) { b->heroPhase = HP_LATER_WAIT; b->laterAt = now + 0.06; } } /* better data after it showed: one more fade */
    else if (st == 3) img_get(b->inLogo, 520, 175, NULL); /* still on its way: asked as the view asks */
    return; }
  if (b->heroPhase == HP_LATER_WAIT && now >= b->laterAt) { b->heroPhase = HP_LATER_OUT; b->putAt = now + 0.14; b->fadeHold = 0; tween_to(&b->heroA, 0, 0.12f, EASE_CSS_EASEIN); return; }
  if (b->heroPhase == HP_LATER_OUT && now >= b->putAt) { b->heroPhase = HP_IDLE; hero_put(b); }
}
/* the logo fitted in its box (never larger than it was drawn at: 520x175) */
static int logo_fit(const char *u, float maxW, float maxH, float *w, float *h, GLuint *t) {
  float asp = 0; *t = u[0] ? img_get(u, 520, 175, &asp) : 0; if (!*t || asp <= 0) return 0;
  float W = 520, H = W / asp; if (H > 175) { H = 175; W = H * asp; }
  if (W > maxW) { W = maxW; H = W / asp; } if (H > maxH) { H = maxH; W = H * asp; }
  *w = W; *h = H; return 1;
}
/* Discover's own face: everything centred over the picture; with the spotlight up a small accent label above the
   title, the dots under it, a soft dark glow behind the words and the rows lowered and dimmed */
static uint32_t acc_light(void) { return col_mix(ACC, 0xffffffffu, 0.2f); }
static HeroLay *hero_lay(Board *b) {
  HeroLay *l = &b->lay; HeroView *v = &b->view;
  if (l->ver == b->viewVer && l->lg == i18n_gen() && l->spot == b->heroSpot) return l;
  l->ver = b->viewVer; l->lg = i18n_gen(); l->spot = b->heroSpot;
  l->discName = text_wrap_count(v->name, 1500, 2, b->heroSpot ? 92 : 76, 800, -1.6f);
  l->discDesc = v->desc[0] ? text_wrap_count(v->desc, 1080, 2, DESC_PX, 400, 0) : 0;
  l->homeName = text_wrap_count(v->name, HERO_W, 2, 76, 800, -1.6f);
  meta_prep(&l->mp, v->metaLine, NULL, v->metaName, META_PX);
  return l;
}
/* the spotlight's label, upper-cased once per language */
static const char *trending_up(void) { static char eye[64]; static unsigned lg; if (lg != i18n_gen()) { lg = i18n_gen(); snprintf(eye, sizeof eye, "%s", T("Trending now")); text_upper(eye, sizeof eye); } return eye; }
static void hero_draw_disc(Board *b, float a) {
  int spot = b->heroSpot; float maxLogoH = spot ? 150 : 175, titlePx = spot ? 92 : 76, cx = SB_W / 2;
  HeroView *v = &b->view; HeroLay *hl = hero_lay(b); float lw = 0, lh = 0, headH; GLuint lt = 0; int drewLogo = logo_fit(v->logo, 520, maxLogoH, &lw, &lh, &lt);
  if (drewLogo) headH = lh; else headH = hl->discName * titlePx * 1.04f;
  int dl = hl->discDesc;
  float metaH = text_line_height(META_PX) > 34 ? text_line_height(META_PX) : 34, bodyH = headH + 20 + metaH + (dl ? 16 + dl * DESC_LH : 0);
  float y = spot ? 214 : 1080 - 622 - bodyH; /* the spotlight's label and dots stay put; otherwise the face stands just above the rows */
  if (spot) { /* the spotlight's label, where it always was (nothing under it moves): a little bigger and brighter than before,
       an accent dot before it; it comes in and goes with the title's logo */
    static float tw; static unsigned lg; if (lg != i18n_gen() || tw <= 0) { lg = i18n_gen(); tw = text_width(trending_up(), 25, 800, 5.0f); }
    float ty = y - 2, l22 = text_line_height(22), ly = ty + (l22 - text_line_height(25)) / 2; int rtl = app_rtl();
    static int shown; static double inAt; /* it comes in once with the title's logo, then stays put while the titles change (only its dot's colour follows them, as the page dots do) */
    float sa = b->spotA.v; /* (the spotlight itself showing: the hero's own fade between titles does not touch it) */
    if (sa < 0.01f) shown = 0; else if (!shown && a * b->logoIn.v > 0.5f) { shown = 1; inAt = app_now(); }
    float ke = shown ? ease_apply(EASE_OUT, clampf((float)((app_now() - inAt) / 0.45), 0, 1)) : 0, la = sa * ke;
    if (la > 0.002f) { gfx_opacity_push(la); gfx_offset_push(0, 12 * (1 - ke));
      text_draw(trending_up(), cx + 9, ly, 25, 800, WHITE_A(236), 5.0f, 0, ALIGN_CENTER);
      gfx_circle(rtl ? cx + 9 + tw / 2 + 16 : cx + 9 - tw / 2 - 16, ly + text_line_height(25) / 2, 5, acc_light() | 0xff000000u);
      gfx_offset_pop(); gfx_opacity_pop(); }
    y += l22 + 18; }
  gfx_opacity_push(a);
  if (drewLogo) { char sk[80]; snprintf(sk, sizeof sk, "logo:%s", b->hero.id); sx_image(sk, lt, cx - lw / 2, y, lw, lh, 0, WHITE_A((int)(255 * b->logoIn.v)), 0, 0, 1, 1); }
  else if (!v->wait) { char sk[80]; snprintf(sk, sizeof sk, "logo:%s", b->hero.id); sx_text_wrapped(sk, v->name, cx, y, 1500, 2, (int)titlePx, 800, WHITE_A(255), -1.6f, ALIGN_CENTER, titlePx * 1.04f); } /* the name as text art moves across like a logo */
  y += headH + 20; meta_draw_p(&hl->mp, cx, y, WHITE_A(255), ALIGN_CENTER); y += metaH;
  if (dl) text_draw_wrapped(v->desc, cx, y + 16, 1080, 2, DESC_PX, 400, WHITE_A(204), 0, ALIGN_CENTER, DESC_LH);
  gfx_opacity_pop();
}
static void dots_draw(Board *b) {
  float da = b->dotsA.v; if (da <= 0.001f || !b->nfeat) return;
  int n = b->nfeat, f = !topNav.active, ci = b->featIdx % n; float tot = 0; for (int i = 0; i < n; i++) tot += b->dotW[i].v + 10;
  float x = SB_W / 2 - tot / 2 + 5, y = 214 + 392; uint32_t al = acc_light();
  gfx_opacity_push(da);
  for (int i = 0; i < n; i++) { int c = i == ci; float w = b->dotW[i].v; uint32_t col = c ? (f ? al : WHITE_A(179)) : (f ? WHITE_A(107) : WHITE_A(71));
    if (app_rtl()) gfx_rrect(SB_W - x - w, y, w, 10, 5, col); else gfx_rrect(x, y, w, 10, 5, col); x += w + 10; }
  gfx_opacity_pop();
}
/* the glow behind the spotlight's words: an ellipse filling its box, .62 at the centre, .34 at 55%, nothing at the edge */
static GLuint scrim_tex(void) {
  static GLuint t; if (t) return t; const int N = 64; unsigned char px[64 * 64];
  for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) { float dx = (x + .5f) / N * 2 - 1, dy = (y + .5f) / N * 2 - 1, d = sqrtf(dx * dx + dy * dy);
    float a = d < .55f ? .62f - (.62f - .34f) * d / .55f : d < 1 ? .34f * (1 - (d - .55f) / .45f) : 0; px[y * N + x] = (unsigned char)(a * 255 + .5f); }
  t = gfx_tex_create(px, N, N, 1, 1); return t;
}
/* the billboard's shading, painted once: dark on the left (under the words) and at the bottom */
static GLuint grad_tex(int vertical) {
  static GLuint t[2]; if (t[vertical]) return t[vertical]; unsigned char px[64];
  for (int i = 0; i < 64; i++) { float x = (i + .5f) / 64; float a = vertical ? (x < .6f ? 0 : (x - .6f) / .4f * .70f) : (x < .35f ? .86f - (.86f - .45f) * x / .35f : x < .62f ? .45f * (1 - (x - .35f) / .27f) : 0); px[i] = (unsigned char)(a * 255 + .5f); }
  t[vertical] = vertical ? gfx_tex_create(px, 1, 64, 1, 1) : gfx_tex_create(px, 64, 1, 1, 1); return t[vertical];
}
static void bill_btn(float x, float y, float w, int play, int on, float a) {
  float h = 66; if (on) gfx_ring_out(x - 6, y - 6, w + 12, h + 12, h / 2 + 6, 4, WHITE_A((int)(255 * a)));
  gfx_rrect(x, y, w, h, h / 2, play ? WHITE_A((int)(255 * a)) : WHITE_A((int)(64 * a)));
  uint32_t ink = play ? RGBA(0, 0, 0, (int)(255 * a)) : WHITE_A((int)(255 * a));
  if (play) icon_draw("play", x + 34, y + h / 2 - 13, 26, ink);
  else { gfx_ring(x + 30, y + h / 2 - 14, 28, 28, 14, 2.5f, ink); text_draw("i", x + 44, y + h / 2 - 15, 20, 800, ink, 0, 0, ALIGN_CENTER); }
  text_draw(T(play ? "Play" : "More info"), x + 72, y + (h - text_line_height(27)) / 2, 27, 700, ink, 0, 0, ALIGN_LEFT);
}
static float bill_btn_w(int play) { return 72 + text_width(T(play ? "Play" : "More info"), 27, 700, 0) + 34; }
static int bill_hi(void) { static int v = -1; if (v < 0) v = plat_mem_total_mb() >= 1500; return v; }
static const char *bill_src(const Item *it, char *bu, size_t cap) { return bill_hi() ? sb_bg_large(it, bu, cap) : sb_bg_mid(it, bu, cap); }
static void bill_draw(Board *b) {
  float a = b->spotA.v; if (a <= 0.002f || !b->nfeat) return;
  float dy = -60 * (1 - a), X = BILL_X, Y = BILL_Y + dy, W = BILL_W, H = BILL_H; int rtl = app_rtl();
  const Item *it = &b->feat[b->featIdx % b->nfeat]; char bu[512]; bill_src(it, bu, sizeof bu); int tw = bill_hi() ? (int)W : 1280, th = bill_hi() ? (int)H : 573; float tr = bill_hi() ? 28 : 20;
  /* (fit 2: a 16:9 picture is wider-cropped from the bottom only: faces are near the top of a backdrop, and the words cover its bottom) */
  if (strcmp(bu, b->billCur)) { snprintf(b->billPrev, sizeof b->billPrev, "%s", b->billCur); snprintf(b->billCur, sizeof b->billCur, "%s", bu); b->billSwitch = app_now(); }
  gfx_opacity_push(a);
  /* the picture at the size it is drawn, from the largest there is (a TV short of memory: 1280 wide, scaled up); the last one under it while the new fades in */
  float k = clampf((float)((app_now() - b->billSwitch) / 0.45), 0, 1); GLuint cur = b->billCur[0] ? img_get_shaped(b->billCur, tw, th, tr, 2) : 0;
  if (cur && k >= 1 && b->nfeat > 1) { char nu[512]; if (bill_src(&b->feat[(b->featIdx + 1) % b->nfeat], nu, sizeof nu)[0]) img_get_shaped(nu, tw, th, tr, 2); } /* the next title's, decoded before it turns */
  if (!cur) k = 0; else if (k < 1 && b->billPrev[0]) { GLuint pv = img_get_shaped(b->billPrev, tw, th, tr, 2); if (pv) ph_picture(pv, X, Y, W, H, 28, WHITE_A(255)); else ph_surface(X, Y, W, H, 28); }
  else if (!b->billPrev[0] && !cur) ph_surface(X, Y, W, H, 28);
  if (cur) { if (k < 1) gfx_opacity_push(ease_apply(EASE_CSS_EASE, k)); ph_picture(cur, X, Y, W, H, 28, WHITE_A(255)); if (k < 1) gfx_opacity_pop(); }
  /* its preview plays inside it, under the words: the plane goes behind this rectangle and the window is see-through there */
  if (spotOn(b) && !topNav.active && a > 0.99f) { btr_card_rect(X, Y, W, H); float va = btr_card_alpha(); if (va > 0.001f) gfx_hole(X, Y, W, H, 28, va); }
  gfx_ring(X, Y, W, H, 28, 2, WHITE_A(70));
  if (rtl) gfx_alpha_tex_uv(grad_tex(0), X + W * 0.35f, Y, W * 0.65f, H, RGB(8, 8, 12), 1, 0, 0, 1); else gfx_alpha_tex(grad_tex(0), X, Y, W * 0.65f, H, RGB(8, 8, 12));
  gfx_alpha_tex(grad_tex(1), X, Y, W, H, RGB(8, 8, 12));
  /* the words: logo (or name), facts, Play / More info; they fade between titles with the hero */
  float ha = b->heroA.v, x = rtl ? X + W - 60 : X + 60, by = Y + H - 56 - 66; int align = rtl ? ALIGN_RIGHT : ALIGN_LEFT;
  if (b->heroSet && ha > 0.002f) {
    HeroView *v = &b->view; HeroLay *hl = hero_lay(b); float lw, lh; GLuint lt; float my = by - 30 - 34;
    gfx_opacity_push(ha);
    if (logo_fit(v->logo, 560, 160, &lw, &lh, &lt)) gfx_image(lt, rtl ? x - lw : x, my - 26 - lh, lw, lh, 0, WHITE_A((int)(255 * b->logoIn.v)), 0, 0, 1, 1);
    else if (!v->wait) text_draw_wrapped(v->name, x, my - 26 - 2 * 80, 900, 2, 76, 800, WHITE_A(255), -1.6f, align, 80);
    meta_draw_p(&hl->mp, x, my, WHITE_A(235), align);
    gfx_opacity_pop(); }
  int f = spotOn(b) && !topNav.active; float pw = bill_btn_w(1), mw = bill_btn_w(0);
  if (rtl) { bill_btn(x - pw, by, pw, 1, f && b->billBtn == 0, 1); bill_btn(x - pw - 18 - mw, by, mw, 0, f && b->billBtn == 1, 1); }
  else { bill_btn(x, by, pw, 1, f && b->billBtn == 0, 1); bill_btn(x + pw + 18, by, mw, 0, f && b->billBtn == 1, 1); }
  /* the dots, bottom right: the one showing wider */
  { int n = b->nfeat, ci = b->featIdx % n; float tot = 0; for (int i = 0; i < n; i++) tot += b->dotW[i].v + 10;
    float dx = rtl ? X + 56 : X + W - 56 - tot, dyy = Y + H - 56 - 33 - 5;
    for (int i = 0; i < n; i++) { float w = b->dotW[i].v; gfx_rrect(dx, dyy, w, 10, 5, i == ci ? WHITE_A(240) : WHITE_A(102)); dx += w + 10; } }
  gfx_opacity_pop();
}
/* the open card's details, under it (cinematic Home): the facts, then two lines of description */
static void cine_details(Board *b, float x, float y, int rtl) {
  if (!b->heroSet || b->heroA.v <= 0.002f) return; HeroView *v = &b->view; HeroLay *hl = hero_lay(b); int align = rtl ? ALIGN_RIGHT : ALIGN_LEFT;
  gfx_opacity_push(b->heroA.v);
  meta_draw_p(&hl->mp, x, y, WHITE_A(230), align);
  if (v->desc[0]) text_draw_wrapped(v->desc, x, y + 44, CINE_DESC_W, 2, DESC_PX, 400, WHITE_A(210), 0, align, DESC_LH);
  gfx_opacity_pop();
}
/* the open card's own logo and badges (cinematic Home) */
static void cine_card_art(const Item *it, float ox, float cy, float ow, float ch, float ek, int rtl) {
  gfx_rrect_v(ox, cy + ch * 0.45f, ow, ch * 0.55f, CARD_R, RGBA(0, 0, 0, 0), RGBA(0, 0, 0, (int)(190 * ek)));
  char lu[512]; logo_src(it, lu, sizeof lu); float lw, lh; GLuint lt;
  if (logo_fit(lu, 360, 130, &lw, &lh, &lt)) gfx_image(lt, rtl ? ox + ow - 28 - lw : ox + 28, cy + ch - 28 - lh, lw, lh, 0, WHITE_A((int)(255 * ek)), 0, 0, 1, 1);
  int bd = recs_badges(it); float bx = rtl ? ox + 22 : ox + ow - 22, byy = cy + ch - 22 - 42;
  for (int k = 0; k < 2; k++) { int on = k == 0 ? (bd & RECS_LOVE) : (bd & RECS_NEW); if (!on) continue;
    const char *t = T(k == 0 ? "We think you'll love this" : "Recently added"); float tw = text_width(t, 21, 700, 0), w = tw + 62;
    float px = rtl ? bx : bx - w; gfx_rrect(px, byy, w, 42, 10, RGBA(12, 11, 17, (int)(220 * ek)));
    gfx_rrect(px + 12, byy + 10, 22, 22, 5, k == 0 ? RGBA(0xe8, 0xb0, 0x4a, (int)(255 * ek)) : RGBA(0xe5, 0x48, 0x4d, (int)(255 * ek)));
    text_draw(t, px + 44, byy + (42 - text_line_height(21)) / 2, 21, 700, WHITE_A((int)(255 * ek)), 0, 0, ALIGN_LEFT);
    byy -= 50; }
}
/* a Top 10 numeral: an outline (the number in light ink at eight offsets, then in the page colour on top) */
static void rank_draw(int n, float x, float y, float h) {
  char s[4]; snprintf(s, sizeof s, "%d", n); int px = (int)(h * 0.92f); float lh = text_line_height(px), ty = y + h - lh * 0.86f;
  static const float o[8][2] = { { -4, 0 }, { 4, 0 }, { 0, -4 }, { 0, 4 }, { -3, -3 }, { 3, -3 }, { -3, 3 }, { 3, 3 } };
  for (int i = 0; i < 8; i++) text_draw(s, x + o[i][0], ty + o[i][1], px, 800, WHITE_A(150), -8, 0, ALIGN_RIGHT);
  text_draw(s, x, ty, px, 800, RGB(0x0c, 0x0b, 0x11), -8, 0, ALIGN_RIGHT);
}
static void hero_draw(Board *b) {
  if (b->kind == 0 && b->cine) { if (b->bill) bill_draw(b); return; } /* the cinematic Home: the billboard; the rows draw their own details */
  if (b->kind == 1 && b->spotA.v > 0.001f) gfx_alpha_tex(scrim_tex(), 260, 170, SB_W - 520, 460, RGBA(12, 11, 17, (int)(255 * b->spotA.v))); /* painted once, one quad */
  if (b->kind == 1) dots_draw(b);
  if (!b->heroSet) return; float a = b->heroA.v; if (a <= 0.001f && !(b->kind == 1 && b->heroSpot)) return;
  /* first launch: the hero rises 18 px and fades in (no delay) */
  float rv = app_reveal_x(0, 0.5f), ry = 18 * (1 - ease_apply(EASE_WEB, rv)), ra = ease_apply(EASE_CSS_EASE, app_reveal_x(0, 0.45f));
  if (b->kind == 1) { gfx_opacity_push(ra); gfx_offset_push(0, ry); hero_draw_disc(b, a); gfx_offset_pop(); gfx_opacity_pop(); return; }
  if (a <= 0.001f) return;
  gfx_opacity_push(a * ra); gfx_offset_push(0, ry);
  int rtl = app_rtl(); float x = rtl ? SB_W - HERO_X : HERO_X, y = HERO_Y; int align = rtl ? ALIGN_RIGHT : ALIGN_LEFT;
  HeroView *v = &b->view; HeroLay *hl = hero_lay(b); float lw, lh; GLuint lt;
  if (logo_fit(v->logo, 480, 150, &lw, &lh, &lt)) { char sk[80]; snprintf(sk, sizeof sk, "logo:%s", b->hero.id); sx_image(sk, lt, rtl ? x - lw : x, y, lw, lh, 0, WHITE_A((int)(255 * b->logoIn.v)), 0, 0, 1, 1); y += lh; } /* a little smaller: the rows get room */
  else { int lines = v->wait ? hl->homeName : sx_text_wrapped(hero_key(b), v->name, x, y, HERO_W, 2, 76, 800, WHITE_A(255), -1.6f, align, 76 * 1.04f); y += lines * 76 * 1.04f; } /* text art on its way: the name waits unseen */
  y += 20; meta_draw_p(&hl->mp, x, y, WHITE_A(255), align); y += text_line_height(META_PX) > 34 ? text_line_height(META_PX) : 34;
  y += 16; text_draw_wrapped(v->desc, x, y, HERO_W, 3, DESC_PX, 400, WHITE_A(204), 0, align, DESC_LH);
  gfx_offset_pop(); gfx_opacity_pop();
}

/* ---- the screen ---- */
static void board_enter_k(Board *b) {
  int fresh = !b->built || (b->kind == 0 && (b->libVer != lib_version() || b->recsVer != recs_version())), keep = b->n > 0;
  if (fresh) { b->built = 1; b->libVer = lib_version(); load_catalog_rows(b, b->kind == 1, keep); rows_window(b); }
  if (spotk(b)) { feat_fill(b); feat_arm(b); if (spotOn(b) && (b->nfeat > 0 || b->featLoad)) { tween_set(&b->spotY, b->kind == 0 ? HOME_SPOT_DY : 144); tween_set(&b->rowsA, b->kind == 1 ? 0.5f : 1); if (b->kind == 0) { tween_set(&b->spotA, 1); tween_set(&b->topY, ROWS_TOP); } } } /* arriving on the spotlight: the rows start where they will rest (no slide down after) */
  if (app_splash_done) timer_start(&b->fade, 0.5f); /* every switch to this tab: the rows rise in one after another (row_in) */
  for (int i = 0; i < b->n; i++) { slide_row(&b->rows[i]); tween_set(&b->rows[i].off, b->rows[i].off.to); }
  tween_set(&b->hostY, 0); b->fastBg = 1; hero_soon(b, 70);
}
static void board_leave(void) {
  for (int i = 0; i < 3; i++) { Board *b = &boards[i]; if (cur() == b) continue; /* another board: the last title leaves at once (its own will fade in) */
    tween_set(&b->heroA, 0); b->heroShown = 0; b->heroPhase = HP_IDLE; b->heroSet = 0; b->fadeHold = 0; }
}
static void enter_home(const void *p) { (void)p; board_leave(); board_enter_k(&boards[0]); }
static void enter_disc(const void *p) { (void)p; board_leave(); board_enter_k(&boards[1]); }
static void enter_col(const void *p) { if (p) { colFolder = *(const ColFolder *const *)p; boards[2].built = 0; } board_leave(); board_enter_k(&boards[2]); }
static unsigned colGen;
unsigned board_col_gen(void) { return colGen; }
void board_col_tick(void) {
  static int once; if (!once) { once = 1; if (getenv("SB_HTTPDLOG")) { char u[600]; httpd_url("/collections", u, sizeof u); fprintf(stderr, "[httpd] %s\n", u); } }
  char *t = httpd_take_collections(); if (!t) return;
  if (*t) colstore_import(t); else colstore_clear(); free(t);
  colGen++; boards[1].built = 0; if (SCR == SCR_DISC) board_rebuild();
}
static int board_key(Board *b, int k);
static int key_col(int k) { return board_key(&boards[2], k); }
/* the next page when the focus is a few cards from the row's end (the official apps load a catalog's pages the same
   way, with skip); a page that brings nothing new, a 404 or hasMore false ends it; a failed one is asked again later */
static void on_page(ItemList *l, int more, void *ud) {
  uintptr_t t = (uintptr_t)ud; Board *b = &boards[(t >> 28) & 3]; int row = t & 63; unsigned gen = (t >> 6) & 0x3fffff;
  if ((b->gen & 0x3fffff) != gen || row >= b->n) { itemlist_free(l); return; }
  Row *r = &b->rows[row]; if (!r->paging) { itemlist_free(l); return; } r->paging = 0; /* the row was filled again meanwhile */
  if (!l) { r->pageAt = app_now() + 10; return; }
  int add = 0; Item *grow = realloc(r->items.items, (size_t)(r->items.n + l->n) * sizeof(Item));
  if (grow) { r->items.items = grow;
    for (int i = 0; i < l->n; i++) { int dup = 0; for (int k = 0; k < r->items.n && !dup; k++) dup = !strcmp(r->items.items[k].id, l->items[i].id); if (!dup && l->items[i].id[0]) r->items.items[r->items.n++] = l->items[i], add++; } }
  r->skipAt += l->total; if (!l->total || !add || more == 0 || r->items.n >= SEEALL_AT) r->more = 0;
  if (getenv("SB_SLOWLOG")) fprintf(stderr, "[cat] page of %s: %d new, %d in the row%s\n", r->title, add, r->items.n, r->more ? "" : " (the end)");
  itemlist_free(l);
}
static void row_more(Board *b, int ri) {
  if (ri < 0 || ri >= b->n) return; Row *r = &b->rows[ri];
  if (!r->more || r->paging || r->items.n - r->col > 8 || app_now() < r->pageAt) return;
  r->paging = 1; sb_catalog_page(&r->def, r->skipAt, on_page, tok(b, ri));
}
static void move_h(Board *b, int dir) {
  Row *r = &b->rows[b->rowIdx]; int old = r->col;
  r->col = dir > 0 ? (r->col < col_last(r) ? r->col + 1 : r->col) : (r->col > 0 ? r->col - 1 : 0);
  if (r->col == old) { nudge_start(&r->nudge, dir > 0 ? K_RIGHT : K_LEFT); return; } /* the row's end */
  slide_row(r); hero_soon(b, 150); row_more(b, b->rowIdx);
}
static void spot_leave(Board *b) { b->featOn = 0; b->featAt = 0; if (b->scoped[0]) { b->scoped[0] = 0; accent_scope(NULL, NULL, NULL); } }
static int board_key(Board *b, int k) {
  if (spotOn(b) && b->kind == 0) { /* Home's billboard: Play / More info; past them, the next or previous title */
    if (k == K_RIGHT) { if (b->billBtn == 0) b->billBtn = 1; else { b->billBtn = 0; feat_go(b, 1); } sound_focus(); return 1; }
    if (k == K_LEFT) { if (b->billBtn == 1) b->billBtn = 0; else feat_go(b, -1); sound_focus(); return 1; }
    if (k == K_DOWN) { if (b->n) { int n = first_visible_row(b, 0, 1); spot_leave(b); b->rowIdx = n >= 0 ? n : 0; hero_soon(b, 150); } return 1; }
    if (k == K_UP) { topnav_focus(); return 1; }
    if (k == K_OK) { const Item *it = focused_of(b); if (!it) return 1; if (b->billBtn == 1) { app_open_item(it); return 1; }
      DetailParams p; memset(&p, 0, sizeof p); p.it = *it; p.openStreams = 1; app_go(SCR_DETAIL, &p, 0); return 1; } /* Play: its streams (one-click best picks on its own when on) */
    if (k == K_PLAY || k == K_PLAYPAUSE) { const Item *it = focused_of(b); if (it) { DetailParams p; memset(&p, 0, sizeof p); p.it = *it; p.openStreams = 1; app_go(SCR_DETAIL, &p, 0); } return 1; }
    return k != K_BACK; }
  if (spotOn(b)) { feat_fill(b);
    if (k == K_LEFT || k == K_RIGHT) { feat_go(b, k == K_RIGHT ? 1 : -1); sound_focus(); return 1; } /* a move by hand taps; the dots stepping by themselves do not */
    if (k == K_DOWN) { if (b->n) { int n = first_visible_row(b, 0, 1); spot_leave(b); b->rowIdx = n >= 0 ? n : 0; hero_soon(b, 150); } return 1; }
    if (k == K_UP) { topnav_focus(); return 1; }
    if (k == K_OK) { const Item *it = focused_of(b); if (it) app_open_item(it); return 1; }
    return k != K_BACK; }
  if (!b->n || b->rowIdx >= b->n) { if (k == K_UP) topnav_focus(); return k != K_BACK; } /* (no rows: Back still leaves) */
  Row *r = &b->rows[b->rowIdx]; row_load(r); /* (the window has it loaded already: this is for a focus that landed some other way) */
  if (k == K_RIGHT) move_h(b, 1);
  else if (k == K_LEFT) move_h(b, -1);
  else if (k == K_DOWN) { int n = first_visible_row(b, b->rowIdx + 1, 1); if (n >= 0) { b->rowIdx = n; hero_soon(b, 150); } else nudge_start(&r->nudge, K_DOWN); }
  else if (k == K_UP) { int n = first_visible_row(b, b->rowIdx - 1, -1); if (n >= 0) { b->rowIdx = n; hero_soon(b, 150); } else if (spotk(b) && b->nfeat) { b->featOn = 1; b->billBtn = 0; hero_soon(b, 150); feat_arm(b); } else topnav_focus(); }
  else if (k == K_OK) { const Item *it = focused_of(b); if (it) app_open_item(it); else if (r->ntiles && r->col < r->ntiles) { const ColFolder *f = &r->tiles[r->col]; app_go(SCR_COL, &f, 0); } else if (see_all(r) && r->col == r->items.n) see_all_open(r); }
  else if (k == K_BACK) return 0;
  return 1;
}
static int key_home(int k) { return board_key(&boards[0], k); }
static int key_disc(int k) { return board_key(&boards[1], k); }
static const Item *focused_cur(void) { Board *b = cur(); return b ? focused_of(b) : NULL; }
/* the pointer on a card: the highlight moves there, the row follows as for the arrows */
static void card_focus(int r, int c) { Board *b = cur(); if (!b || r >= b->n) return; Row *row = &b->rows[r]; if (c > col_last(row)) return; int was = topNav.active; if (!was && !spotOn(b) && r == b->rowIdx && c == row->col) return;
  topNav.active = 0; if (spotOn(b)) spot_leave(b); if (r == b->rowIdx) { row->col = c; slide_row(row); } else { b->rowIdx = r; row->col = c; slide_row(row); } hero_soon(b, 150); row_more(b, r); }
static void card_open_tick(Board *b, float dt) {
  const Row *r = b->rowIdx < b->n ? &b->rows[b->rowIdx] : NULL; int here = r && r->col < r->items.n && focusOn(b) && !app_overlay;
  if (b->exp.v > 0.001f && (!here || b->expRow != b->rowIdx || b->expCol != r->col)) tween_set(&b->exp, 0); /* moved: shut at once, the row must not lag behind the remote */
  if (here && app_trailer_idle() > (b->cine ? cine_open_after() : CARD_OPEN_AFTER) && (b->cine || strcmp(store_get("bgTrailer", "true"), "false"))) { /* (the setting read only once it could matter: not every frame) */ if (b->exp.to < 1) { b->expRow = b->rowIdx; b->expCol = r->col; tween_to(&b->exp, 1, 0.28f, EASE_STD); } } else if (b->exp.to > 0) tween_set(&b->exp, 0);
  tween_tick(&b->exp, dt);
}
static void board_tick(float dt) {
  Board *b = cur(); if (!b) return;
  card_open_tick(b, dt);
  timer_tick(&b->fade, dt); nudge_tick(&b->nudge, dt);
  if (b->kind == 0) { static unsigned ser; if (store_serial() != ser) { ser = store_serial(); int c = strcmp(store_get("homeLayout", "cinematic"), "classic") != 0; if (c != b->cine) board_rebuild(); } } /* Settings > Home layout */
  if (b->kind == 0 && b->recsVer != recs_version()) { b->recsVer = recs_version(); board_rebuild(); }
  if (b->kind == 0 && b->winRow != b->rowIdx) rows_window(b); /* (the focused row changed) */
  if (b->kind == 0) { /* the focused row's details make room under it; the rows dock under the bar */
    int fo = focusOn(b); for (int i = 0; i < b->n; i++) b->rows[i].extra = b->cine && fo && i == b->rowIdx && b->rows[i].items.n && !b->rows[i].wide && !b->rows[i].ntiles ? CINE_DET_H : 0;
    tween_to(&b->topY, b->cine && !spotOn(b) && !(b->bill && topNav.active && !b->rowIdx) ? CINE_TOP : ROWS_TOP, 0.32f, EASE_WEB); tween_tick(&b->topY, dt); }
  if (spotk(b)) { int sp = spotOn(b), hs = b->heroSet && b->heroSpot;
    /* the rows lower and dim with the spotlight's face (it changes while the hero is invisible); its dots come with it */
    int rs = sp && (hs || b->nfeat > 0 || b->featLoad); /* (the spotlight coming, or its list on its way: lowered already, not after its title has loaded) */ /* the rows follow the focus at once: leaving the dots, they rise and light up with the poster's own focus, not after the hero has changed */
    tween_to(&b->spotY, rs ? (b->kind == 0 ? HOME_SPOT_DY : 144) : 0, 0.45f, EASE_WEB); tween_to(&b->rowsA, rs && b->kind == 1 ? 0.5f : 1, 0.28f, EASE_CSS_EASE); tween_to(&b->spotA, (b->kind == 0 ? sp : hs) ? 1 : 0, 0.4f, EASE_CSS_EASE);
    int dOn = sp && hs && b->nfeat; if (dOn != b->dotsWant) { b->dotsWant = dOn; b->dotsGo = app_now() + (dOn ? 0.1 : 0); } /* in after a 0.1 s pause */
    if (app_now() >= b->dotsGo) tween_to(&b->dotsA, dOn ? 1 : 0, dOn ? 0.35f : 0.3f, EASE_CSS_EASE); tween_tick(&b->dotsA, dt);
    tween_tick(&b->spotY, dt); tween_tick(&b->rowsA, dt); tween_tick(&b->spotA, dt);
    for (int i = 0; i < b->nfeat; i++) { tween_to(&b->dotW[i], i == b->featIdx % b->nfeat ? 34 : 10, 0.45f, EASE_SNAP); tween_tick(&b->dotW[i], dt); }
    /* the spotlight's colours only while it has the focus */
    if (b->scoped[0] && (!sp || topNav.active)) { b->scoped[0] = 0; accent_scope(NULL, NULL, NULL); }
    else if (sp && b->nfeat && !topNav.active && !b->scoped[0] && b->heroSet && b->heroSpot) { const Item *it = &b->hero; char pu[512], lu[512]; accent_scope(it->id, sb_poster_of(it, pu, sizeof pu), sb_logo_of(it, lu, sizeof lu)); snprintf(b->scoped, sizeof b->scoped, "%s", it->id); }
    if (b->featAt && app_now() >= b->featAt) { b->featAt = 0;
      if (sp && !topNav.active) { BgtState bs = bgt_state(); if (app_idle() < 3.5f || (b->kind == 0 && (pace_period() > 20 || bs == BGT_WAIT || bs == BGT_LOAD || bs == BGT_SHOW))) feat_arm(b); /* (its preview plays to the end before the billboard turns) */ else { if (b->kind == 0) b->billBtn = 0; feat_go(b, 1); } } } } /* (Home's billboard waits while frames run late) */
  if (b->heroPending && app_now() >= b->heroAt) { b->heroPending = 0; hero_apply(b); }
  hero_tick(b, dt); logos_ahead(b, 0);
  /* the rows above the focused one scroll out of the way and fade as they go (0.14 s); only the focused row and
     the next one are shown, a row coming into view fades in (0.28 s) */
  int fo = focusOn(b), vi = fo ? b->rowIdx : first_visible_row(b, 0, 1); if (vi < 0) vi = 0;
  float y = 0; if (fo) for (int i = 0; i < b->rowIdx; i++) if (!b->rows[i].hidden) y += row_h(&b->rows[i]);
  tween_to(&b->hostY, -y, 0.22f, EASE_ROW); tween_tick(&b->hostY, dt);
  int nx = first_visible_row(b, vi + 1, 1);
  for (int i = 0; i < b->n; i++) { Row *r = &b->rows[i]; if (r->closing) { float k = (float)((app_now() - r->closeAt) / 0.45); if (k >= 1) { r->closing = 0; r->hidden = 1; r->hk = 1; } else r->hk = 1 - ease_apply(EASE_CSS_EASE, k); }
    tween_tick(&r->off, dt); focus_tick(&r->pop, dt); nudge_tick(&r->nudge, dt);
    int gone = i < vi, show = i == vi || i == nx; if (r->closing) tween_to(&r->a, 0, 0.4f, EASE_CSS_EASEOUT); /* (its last title fades out with it, not in a flash) */ else if (gone) tween_to(&r->a, 0, 0.14f, EASE_CSS_EASEOUT); else tween_to(&r->a, show ? 1 : 0, 0.28f, EASE_CSS_EASE); tween_tick(&r->a, dt); }
}
/* the art a card shows: the translated poster when there is one (a card near the cursor holds its English poster
   back for up to 0.7 s while the translation is on its way) */
static const char *card_art(Row *r, const Item *it, int near, char *buf, size_t cap, int *hold, int *final) {
  *hold = 0; *final = 1; if (r->wide || !tmdb_wanted(it->id)) return NULL;
  const TmdbLoc *L = tmdb_loc(it->type, it->id, 1);
  if (L && L->poster && L->poster[0]) { snprintf(buf, cap, "%s", L->poster); char *p = strstr(buf, "/t/p/w500/"); if (p) memcpy(p, "/t/p/w342/", 10); return buf; }
  if (!L) *final = 0; /* nothing known yet (or nothing at all): asked again each frame, as before */
  if (near && !L && app_now() - r->locAt < TMDB_WAIT_CARD && tmdb_state(it->type, it->id, 1) == TMDB_PENDING) *hold = 1;
  return NULL;
}
/* the cards' resolved pictures and words start again when the items, the lookups or the language change */
static void prep_check(Row *r) {
  if (r->cp && r->ncp == r->items.n && r->cpGen == tmdb_generation() && r->cpLg == i18n_gen()) return;
  if (r->cp && r->ncp < r->items.n && r->cpGen == tmdb_generation() && r->cpLg == i18n_gen()) { /* a page came: the cards made so far stay */
    CardPrep *cp = realloc(r->cp, (size_t)r->items.n * sizeof *cp); unsigned char *st = cp ? realloc(r->cpSt, (size_t)r->items.n) : NULL;
    if (cp) r->cp = cp; if (st) { r->cpSt = st; memset(r->cp + r->ncp, 0, (size_t)(r->items.n - r->ncp) * sizeof *r->cp); memset(r->cpSt + r->ncp, 0, (size_t)(r->items.n - r->ncp)); r->ncp = r->items.n; return; } }
  prep_free(r); if (!r->items.n) return;
  r->cp = calloc((size_t)r->items.n, sizeof *r->cp); r->cpSt = calloc((size_t)r->items.n, 1); if (!r->cp || !r->cpSt) { prep_free(r); return; }
  r->ncp = r->items.n; r->cpGen = tmdb_generation(); r->cpLg = i18n_gen();
}
/* card i's prepared look (made the first time, again only when its translated poster comes); *changed: made now */
static const CardPrep *row_prep(Row *r, int i, int near, int *hold, int *changed) {
  *hold = 0; if (changed) *changed = 0; if (!r->cp || i < 0 || i >= r->ncp) return NULL;
  CardPrep *cp = &r->cp[i]; if (r->cpSt[i] == 1) return cp;
  char lb[512]; int fin; const char *art = card_art(r, &r->items.items[i], near, lb, sizeof lb, hold, &fin);
  if (!r->cpSt[i] || art) { card_prep(cp, &r->items.items[i], r->wide, art); if (changed) *changed = 1; }
  r->cpSt[i] = fin ? 1 : 2; return cp;
}
/* posters asked for before they are needed (a request is a lookup once it is loading): the next cards to the right
   and a couple behind in rows near the focus, and the first screenful of rows just off screen, so moving never
   uncovers an empty card */
/* (asked once per range: a range that stays put asks again only for a card whose translated poster came since) */
static void row_prefetch(Row *r, int slot, int from, int to) {
  if (from < 0) from = 0; if (to > r->items.n - 1) to = r->items.n - 1;
  prep_check(r); int same = r->pf[slot][0] == from && r->pf[slot][1] == to; if (same && !r->pfWait[slot]) return;
  r->pf[slot][0] = from; r->pf[slot][1] = to; int wait = 0;
  int cw = (int)card_w(r), ch = (int)card_h(r), fit = r->wide ? 1 : 0;
  for (int i = from; i <= to; i++) { int hold, ch2; const CardPrep *cp = row_prep(r, i, 0, &hold, &ch2); if (!cp) continue; if (r->cpSt[i] != 1) wait = 1;
    if ((!same || ch2) && cp->pre[0]) img_get_shaped(cp->pre, cw, ch, CARD_R, fit); }
  r->pfWait[slot] = (unsigned char)wait;
}
/* "its trailer is playing": three slim bars rising and falling out of step, on a small glass chip in the card's
   top left corner (it comes and goes with the trailer) */
static void row_draw(Board *b, Row *r, int ri, float y, int near, float rowA) {
  int rtl = app_rtl(); float lh = text_line_height(31);
  int active = focusOn(b) && ri == b->rowIdx;
  /* first launch: each row rises 30 px and fades in, 60 ms after the one above */
  float d = 0.04f + (ri < 3 ? ri : 3) * 0.06f, rx = app_revealing() ? 30 * (1 - ease_apply(EASE_WEB, app_reveal_x(d, 0.5f))) : 0, ra = ease_apply(EASE_CSS_EASE, app_reveal_x(d, 0.4f)) * rowA;
  float ta = ra, ca = ra;
  if (ta <= 0.001f) return;
  if (!r->locAt && r->items.n && near) r->locAt = app_now();
  gfx_offset_push(nudge_x(&r->nudge), nudge_y(&r->nudge) + rx);
  if (ta < 1) gfx_opacity_push(ta);
  text_draw(T(r->title), rtl ? SB_W - 52 : 52, y + 18, 31, 700, RGB(0xe9, 0xe9, 0xee), -0.3f, SB_W - 104, rtl ? ALIGN_RIGHT : ALIGN_LEFT);
  if (ta < 1) gfx_opacity_pop();
  float cy = y + 18 + lh + 26, step = step_of(r), off = r->off.v, cw = card_w(r), ch = card_h(r);
  int i0 = (int)(off / step) - 1, i1 = (int)((off + SB_W - 48) / step) + 1; if (i0 < 0) i0 = 0; if (i1 > r->items.n - 1) i1 = r->items.n - 1;
  float ek = active && ri == b->expRow && r->col == b->expCol && !r->ntiles ? b->exp.v : 0, ex = r->wide ? 0 : (CARD_OPEN_W - cw) * ek; /* the card open: this much wider; the ones after it make room */
  if (ca < 1) gfx_opacity_push(ca);
  prep_check(r);
  float rk = 1; int rem = b->remAt > 0 && ri == b->remRow && (rk = (float)((app_now() - b->remAt) / 0.38)) < 1; if (rem) rk = ease_apply(EASE_CSS_EASEOUT, rk); else if (ri == b->remRow) b->remAt = 0;
  if (rem) { float gx = 48 + b->remIdx * step - off; if (rtl) gx = SB_W - gx - cw; float s = 0.86f + 0.14f * (1 - rk); /* the one taken out: fading and shrinking where it was */
    gfx_opacity_push(1 - rk); gfx_scale_push(s, s, gx + cw / 2, cy + ch / 2); card_draw_wh(&b->remIt, gx, cy, cw, ch, r->wide, r->label, 0); gfx_scale_pop(); gfx_opacity_pop(); }
  float rk0 = r->rank ? RANK_W : 0;
  if (r->rank) for (int i = i0; i <= i1 && i < 10; i++) { float sx = 48 + i * step - off + (i > r->col ? ex : 0); rank_draw(i + 1, rtl ? SB_W - sx : sx + rk0 + 34, cy, ch); }
  for (int i = i0; i <= i1; i++) {
    float x = 48 + i * step - off + rk0 + (rem && i >= b->remIdx ? step * (1 - rk) : 0) + (i > r->col ? ex : 0); if (rtl) x = SB_W - x - cw; /* (the ones after it close the gap) */
    int load = near && i >= r->col - 2 && i < r->col + 9;
    if (active && i == r->col) { hit_add(rtl ? x - ex : x, cy, cw + ex, ch, card_focus, NULL, ri, i); continue; } hit_add(x, cy, cw, ch, card_focus, NULL, ri, i); /* the focused card is drawn last, over its neighbours as it grows */
    int hold; const CardPrep *cp = row_prep(r, i, load, &hold, NULL); unsigned long long lk = lift_key(r, i);
    lift_begin(lk, 0, 0, x, cy, cw, ch, CARD_R); /* (the one the focus just left settles back) */
    card_draw_prep(&r->items.items[i], cp, x, cy, cw, ch, r->wide, r->label, !hold && (load || near));
    lift_end(lk, x, cy, cw, ch, CARD_R);
  }
  if (ca < 1) gfx_opacity_pop();
  if (near) { row_prefetch(r, 0, i1 + 1, i1 + 6); row_prefetch(r, 1, i0 - 2, i0 - 1); }
  if (near && !r->ntiles && r->items.n && (r->pfOpen != (active ? r->col + 1 : 0) || app_now() >= r->wideAt)) { /* the wide pictures: asked as the focus lands and again every half second (an ask nobody repeats for 1.5 s is dropped) */
    int landed = active && r->pfOpen != r->col + 1, fw0 = r->wide ? (int)cw : (int)CARD_OPEN_W; char pu0[512]; r->pfOpen = active ? r->col + 1 : 0; r->wideAt = app_now() + 0.5;
    if (landed && r->col < r->items.n && sb_bg_card(&r->items.items[r->col], pu0, sizeof pu0)[0]) img_urgent_shaped(pu0, fw0, (int)ch, CARD_R, 1); /* the one the card is about to show: ahead of the queue */
    for (int d = active ? -1 : 0; d <= (active ? 6 : 2); d++) { int i = r->col + d; if (i < 0 || i >= r->items.n) continue; const Item *wi = &r->items.items[i];
      if (!r->wide && sb_bg_small(wi, pu0, sizeof pu0)[0]) img_get_shaped(pu0, WIDE_SM_W, WIDE_SM_H, CARD_R * 0.4f, 1); /* (a stand-in of a few KB, for every card it could reach) */
      if ((d == 0 || (active && d <= 2)) && (d == 0 || img_has_room()) && sb_bg_card(wi, pu0, sizeof pu0)[0]) img_get_shaped(pu0, fw0, (int)ch, CARD_R, 1); } }
  if (active && r->col < r->items.n) { float x = 48 + r->col * step - off + rk0 + (rem && r->col >= b->remIdx ? step * (1 - rk) : 0); if (rtl) x = SB_W - x - cw; float ox = rtl ? x - ex : x, ow = cw + ex; /* (mirrored, it opens to the left) */
    int hold; const CardPrep *cp = row_prep(r, r->col, 1, &hold, NULL);
    if (ra < 1) gfx_opacity_push(ra);
    float hp = app_hold_progress(); unsigned long long lk = lift_key(r, r->col);
    lift_begin_z(lk, 1, hp, ox, cy, ow, ch, CARD_R, 0.06f * (1 - ek)); /* (still when open: the video's rectangle cannot scale with it) */
    if (ex > 0) gfx_rrect(ox, cy, ow, ch, CARD_R, RGB(0x0c, 0x0b, 0x11));
    card_draw_prep(&r->items.items[r->col], cp, x, cy, cw, ch, r->wide, r->label, 1);
    if (ek > 0.001f) { char bu[512]; int fw = r->wide ? (int)cw : (int)CARD_OPEN_W; if (sb_bg_card(&r->items.items[r->col], bu, sizeof bu)[0]) { GLuint bt = img_get_shaped(bu, fw, (int)ch, CARD_R, 1); /* a size of its own, loaded once: it is uncovered as the card widens */
        char su[512]; GLuint sm = !r->wide && sb_bg_small(&r->items.items[r->col], su, sizeof su)[0] ? img_get_shaped(su, WIDE_SM_W, WIDE_SM_H, CARD_R * 0.4f, 1) : 0; float wx = rtl ? ox + ow - fw : ox; double wn = app_now();
        if (sm && !b->wideSt) b->wideSt = app_trailer_idle() + 1e-3; /* (how long after the key each came: the log below) */
        if (bt && !b->wideBt) { b->wideBt = wn - (ek < 0.3f ? 1 : 0); fprintf(stderr, "[card] wide picture %.2f s after the key (stand-in %.2f s, card %s)\n", app_trailer_idle(), b->wideSt, ek >= 0.999f ? "already open" : "opening"); }
        float sk = bt ? clampf((float)((wn - b->wideBt) / 0.18), 0, 1) : 0; /* the sharp one: over the stand-in, softly; there before the card opened: nothing to see change */
        if (sm || bt) { gfx_clip_push(ox, cy, ow, ch);
          if (sm && sk < 1) { gfx_opacity_push(ek); ph_picture(sm, wx, cy, (float)fw, ch, CARD_R, WHITE_A(255)); gfx_opacity_pop(); }
          if (bt) { gfx_opacity_push(sm ? ek * sk : ek * ek); ph_picture(bt, wx, cy, (float)fw, ch, CARD_R, WHITE_A(255)); gfx_opacity_pop(); }
          gfx_clip_pop(); } } }
    else b->wideBt = b->wideSt = 0;
    if (hp > 0) hold_trace_draw(x, cy, cw, ch, CARD_R, ease_apply(EASE_IN_OUT, hp));
    lift_end(lk, ox, cy, ow, ch, CARD_R);
    if (b->cine && ek > 0.001f) cine_card_art(&r->items.items[r->col], ox, cy, ow, ch, ek, rtl);
    if (ek >= 0.999f) { btr_card_rect(ox, cy + b->hostY.v + (spotk(b) ? b->spotY.v : 0), ow, ch); float va = btr_card_alpha(); if (va > 0.001f) { gfx_hole(ox, cy, ow, ch, CARD_R, va); gfx_ring(ox, cy, ow, ch, CARD_R, 3, WHITE_A((int)(235 * va))); } } /* the video shows through, under the card's own edge */
    if (b->cine && r->extra > 0) cine_details(b, rtl ? ox + ow : ox, cy + ch + 24, rtl);
    if (ra < 1) gfx_opacity_pop(); }
  if (r->ntiles) { int a = (int)(off / step) - 1, z = (int)((off + SB_W - 48) / step) + 1, fit = r->wide ? 1 : 0; if (a < 0) a = 0; if (z > r->ntiles - 1) z = r->ntiles - 1; if (ca < 1) gfx_opacity_push(ca); /* a tile's own picture, with its name only when it asks for one or has none */
    for (int pass = 0; pass < 2; pass++) for (int i = a; i <= z; i++) { int on = active && i == r->col; if (on != pass) continue; const ColFolder *f = &r->tiles[i]; float x = 48 + i * step - off; if (rtl) x = SB_W - x - cw; unsigned long long lk = lift_key(r, i);
      hit_add(x, cy, cw, ch, card_focus, NULL, ri, i);
      lift_begin(lk, on, 0, x, cy, cw, ch, CARD_R); GLuint tx = f->cover[0] && near ? img_get_shaped(f->cover, (int)cw, (int)ch, CARD_R, fit) : 0; int pic = tx != 0;
      if (pic) ph_picture(tx, x, cy, cw, ch, CARD_R, WHITE_A(255)); else ph_surface(x, cy, cw, ch, CARD_R);
      if (!f->hideTitle || !f->cover[0] || (!pic && f->cover[0] && img_shaped_failed(f->cover, (int)cw, (int)ch, CARD_R, fit))) text_draw(f->title, x + cw / 2, cy + (ch - lh) / 2, 31, 700, WHITE_A(on ? 255 : 217), 0, cw - 32, ALIGN_CENTER);
      lift_end(lk, x, cy, cw, ch, CARD_R); }
    if (ca < 1) gfx_opacity_pop(); }
  if (see_all(r)) { int i = r->items.n, on = active && r->col == i; float x = 48 + i * step - off + ex; if (rtl) x = SB_W - x - cw;
    if (x + cw > 0 && x < SB_W) { unsigned long long lk = lift_key(r, i); hit_add(x, cy, cw, ch, card_focus, NULL, ri, i); if (ca < 1) gfx_opacity_push(ca);
      lift_begin(lk, on, 0, x, cy, cw, ch, CARD_R); gfx_rrect(x, cy, cw, ch, CARD_R, WHITE_A(on ? 40 : 18));
      text_draw(T("See All"), x + cw / 2, cy + (ch - lh) / 2, 31, 700, WHITE_A(on ? 255 : 191), 0, cw - 32, ALIGN_CENTER);
      lift_end(lk, x, cy, cw, ch, CARD_R); if (ca < 1) gfx_opacity_pop(); } }
  gfx_offset_pop();
}
static void board_draw(void) {
  Board *b = cur(); if (!b) return;
  hero_draw(b);
  { const ColFolder *tf = focusOn(b) ? tile_cur(b) : NULL; /* a tile in focus: its logo, or its name, where a title's would be */
    if (tf && !(b->cine && !spotOn(b))) { int rtl = app_rtl(); float lw, lh, x = rtl ? SB_W - HERO_X : HERO_X; GLuint lt; if (tf->logo[0] && logo_fit(tf->logo, 480, 150, &lw, &lh, &lt)) gfx_image(lt, rtl ? x - lw : x, HERO_Y, lw, lh, 0, WHITE_A(255), 0, 0, 1, 1); else if (!tf->logo[0] || img_failed(tf->logo)) text_draw(tf->title, x, HERO_Y, 76, 800, WHITE_A(255), -1.6f, HERO_W, rtl ? ALIGN_RIGHT : ALIGN_LEFT); else img_get(tf->logo, 520, 175, NULL); } }
  if (b->kind == 2 && !b->n && colFolder && colFolder->nsrc) { char m[220]; snprintf(m, sizeof m, "%s: %s", T("This needs an addon that is not installed"), colFolder->src[0].addonId); text_draw(m, SB_W / 2, SB_H / 2, 31, 600, WHITE_A(204), 0, SB_W - 96, ALIGN_CENTER); } /* no row could be made: why, rather than nothing found */
  if (b->kind == 2 && app_now() - colAt > 6) { int any = 0; for (int i = 0; i < b->n && !any; i++) any = !b->rows[i].hidden; if (!any) text_draw(T("Nothing found."), SB_W / 2, SB_H / 2, 31, 600, WHITE_A(204), 0, SB_W - 96, ALIGN_CENTER); }
  float top = b->kind == 0 ? b->topY.v : ROWS_TOP;
  gfx_clip_push(0, top, SB_W, SB_H - top);
  float sy = spotk(b) ? b->spotY.v : 0, ra0 = spotk(b) ? b->rowsA.v : 1;
  gfx_offset_push(0, b->hostY.v + sy);
  float y = top;
  int vis = 0;
  for (int i = 0; i < b->n; i++) { Row *r = &b->rows[i]; if (r->hidden) continue; float h = row_h(r);
    /* a tab switch: each row in view rises 28 px and fades in, 50 ms after the one above (0.32 s each, the glide's curve) */
    float rk = 1, rdy = 0; if (b->fade.on) { float t = (timer_x(&b->fade) * 0.5f - 0.05f * (vis < 4 ? vis : 4)) / 0.38f; rk = ease_apply(EASE_CSS_EASEOUT, t < 0 ? 0 : t > 1 ? 1 : t); rdy = 28 * (1 - rk); }
    int inView = r->a.v > 0.001f && y + b->hostY.v + sy < SB_H && y + h + b->hostY.v + sy > top;
    if (inView) { vis++; if (rdy > 0.01f) gfx_offset_push(0, rdy); row_draw(b, r, i, y, abs(i - b->rowIdx) <= 3, ra0 * rk * r->a.v); if (rdy > 0.01f) gfx_offset_pop(); }
    else if (abs(i - b->rowIdx) <= 2) { int c0 = (int)(r->off.v / step_of(r)); row_prefetch(r, 2, c0 - 1, c0 + 7); } /* off screen but one or two presses away */
    y += h; }
  gfx_offset_pop();
  gfx_clip_pop();
}
/* the first cards of the first rows (as drawn): asked for; whether all are in */
static int first_cards(Board *b, int nrows) {
  int seen = 0, want = 0, got = 0; if (!b->n) return 0;
  for (int r = 0; r < b->n && seen < nrows; r++) { Row *row = &b->rows[r]; if (row->hidden || !row->items.n) continue; seen++;
    prep_check(row);
    for (int c = 0; c < 6 && c < row->items.n; c++) { want++; int hold; const CardPrep *cp = row_prep(row, c, 0, &hold, NULL); const char *src = cp ? cp->pre : "";
      if (!*src) { got++; continue; } int cw = (int)card_w(row), ch = (int)card_h(row), fit = row->wide ? 1 : 0; /* the same shaped picture the card draws */
      img_get_shaped(src, cw, ch, CARD_R, fit); if (img_shaped_loaded(src, cw, ch, CARD_R, fit) || img_shaped_failed(src, cw, ch, CARD_R, fit)) got++; } }
  return seen && got >= want;
}
int board_home_ready(void) {
  Board *b = &boards[0];
  if (!first_cards(b, 2)) return 0;
  if (!b->heroSet || !b->heroShown) return 0;
  if (b->cine) return 1; /* (no full-screen backdrop to wait for) */
  return bg_current()[0] && img_loaded(bg_current());
}
/* a tab not on screen made ready in the background: its rows (and Discover's spotlight) asked for, then what its
   first screen shows: the first title's details, translation, logo and backdrop, the first cards. 1 when all is in */
static int board_warm(Board *b) {
  if (cur() == b) return 1;
  if (!b->built) { b->built = 1; b->libVer = lib_version(); load_catalog_rows(b, b->kind == 1, 0); if (b->kind == 1) feat_fill(b); return 0; }
  const Item *it = focused_of(b); if (!it) return tile_cur(b) != NULL;
  if (b->kind == 1 && spotOn(b) && !b->nfeat) return 0; /* the spotlight's titles first */
  int sp = spotOn(b), row = sp ? SPOT_ROW : b->rowIdx, col = sp ? b->featIdx % b->nfeat : b->rows[b->rowIdx].col;
  if (!sb_meta_cached(it->type, it->id)) meta_ask(b, row, col);
  int loc = !tmdb_wanted(it->id) || tmdb_loc(it->type, it->id, 1) != NULL; /* (asks for it) */
  char u[512]; int pics = 1, cards = 1;
  /* pictures only where memory allows: on a small budget another tab's pictures would push out the ones on screen
     (they loaded again in view) */
  if (img_budget_mb() >= 80) {
    logo_src(it, u, sizeof u); if (u[0]) { img_prefetch_pri(u, 520, 175); pics = pics && (img_loaded(u) || img_failed(u)); }
    if (sb_bg_mid(it, u, sizeof u)[0]) { img_get(u, 1280, 720, NULL); pics = pics && (img_loaded(u) || img_failed(u)); } /* at the size the backdrop draws it */
    cards = first_cards(b, b->kind == 1 ? 1 : 2); }
  return loc && pics && cards && sb_meta_cached(it->type, it->id) != NULL;
}
/* right after start: Discover, Home and the Library get their first screen ready one after the other, a little
   at a time (every quarter second, never while the remote moves), so a tab switch never shows a first load; each
   gets 10 s at most */
void tabs_warm_tick(void) {
  static double t0, next, tabAt; static int step;
  if (step >= 3 || !app_splash_done || SCR == SCR_LOGIN || player_open()) return;
  double now = app_now(); if (!t0) { t0 = now + 1.5; return; } /* the first screen settles first */
  if (now < t0 || now < next || app_idle() < 0.6f) return; next = now + 0.25;
  if (!tabAt) tabAt = now;
  int done = step == 0 ? board_warm(&boards[1]) : step == 1 ? board_warm(&boards[0]) : lib_warm();
  if (done || now - tabAt > 10) { static const char *const nm[3] = { "Discover", "Home", "Library" }; fprintf(stderr, "[warm] %s %s in %.1f s\n", nm[step], done ? "ready" : "gave up", now - tabAt); step++; tabAt = 0; }
}
void board_invalidate(void) { boards[0].built = 0; }
void board_invalidate_all(void) { boards[0].built = boards[1].built = 0; }
/* the bar hands the focus down: the first row (Discover: its spotlight) */
void board_from_bar(void) { Board *b = cur(); if (!b) return; int n = first_visible_row(b, 0, 1); b->rowIdx = n >= 0 ? n : 0;
  if (spotk(b) && b->nfeat) { b->featOn = 1; b->billBtn = 0; feat_arm(b); } hero_soon(b, 150); }
/* the titles of the moment for the screensaver: the spotlight, then the first rows of Home and Discover (6 of each) */
int board_saver_items(Item *out, int max) {
  int n = 0; Board *d = &boards[1];
  #define TAKE(it) do { const Item *x_ = (it); char bu_[512]; int dup_ = 0; if (!x_->id[0] || n >= max) break; for (int q_ = 0; q_ < n; q_++) if (!strcmp(out[q_].id, x_->id)) dup_ = 1; if (!dup_ && sb_bg_mid(x_, bu_, sizeof bu_)[0]) out[n++] = *x_; } while (0)
  for (int j = 0; j < d->nfeat && j < 6; j++) TAKE(&d->feat[j]);
  for (int bi = 0; bi < 2; bi++) { Board *b = &boards[bi]; for (int r = 0, took = 0; r < b->n && took < 4; r++) { if (!b->rows[r].items.n) continue; /* (Discover's collections: tiles, no titles) */ took++; for (int j = 0; j < b->rows[r].items.n && j < 6; j++) TAKE(&b->rows[r].items.items[j]); } }
  #undef TAKE
  return n;
}
/* where the focused card is (a title page grows out of it) */
static int board_origin(float *x, float *y) {
  Board *b = cur(); if (!b || spotOn(b) || b->rowIdx >= b->n || topNav.active) return 0; Row *r = &b->rows[b->rowIdx]; if (r->col > col_last(r)) return 0;
  float cy = (b->kind == 0 ? b->topY.v : ROWS_TOP) + b->hostY.v + (spotk(b) ? b->spotY.v : 0); for (int i = 0; i < b->rowIdx; i++) if (!b->rows[i].hidden) cy += row_h(&b->rows[i]);
  float cw = card_w(r), ch = card_h(r), cx = 48 + r->col * step_of(r) - r->off.v + (r->rank ? RANK_W : 0); if (app_rtl()) cx = SB_W - cx - cw;
  *x = cx + cw / 2; *y = cy + 18 + text_line_height(31) + 26 + ch / 2; return 1;
}
/* the library or the addon list changed under the board on screen: its rows reload where they stand, the
   highlight stays on the same row (found by its title: Continue Watching comes and goes) and card, and no
   entrance is replayed */
void board_rebuild(void) {
  Board *b = cur(); if (!b) return;
  static char titles[MAXROWS][96]; int cols[MAXROWS], n = b->n, ri = b->rowIdx;
  for (int i = 0; i < n; i++) { snprintf(titles[i], sizeof titles[i], "%s", b->rows[i].title); cols[i] = b->rows[i].col; }
  b->built = 1; b->libVer = lib_version();
  load_catalog_rows(b, b->kind == 1, 0);
  b->rowIdx = 0;
  for (int j = 0; j < b->n; j++) { Row *r = &b->rows[j];
    for (int i = 0; i < n; i++) if (!strcmp(titles[i], r->title)) { r->col = cols[i]; if (row_n(r) && r->col >= row_n(r)) r->col = row_n(r) - 1; if (r->ntiles && r->col >= r->ntiles) r->col = r->ntiles - 1; if (i == ri) b->rowIdx = j; break; } }
  rows_window(b);
  for (int i = 0; i < b->n; i++) { slide_row(&b->rows[i]); tween_set(&b->rows[i].off, b->rows[i].off.to); }
  hero_soon(b, 70);
}
void board_refresh_current(void) { Board *b = cur(); if (b) { b->built = 0; board_enter_k(b); } }
/* the library changed under the page (watched, added, removed): its rows take the change in place, nothing replays (the
   card that changed says so itself) */
void board_refresh_quiet(void) { Board *b = cur(); if (!b) return; b->built = 1; b->libVer = lib_version(); load_catalog_rows(b, b->kind == 1, b->n > 0); }
Screen homeScreen = { enter_home, NULL, key_home, board_tick, board_draw, focused_cur, NULL, NULL, board_origin };
Screen colScreen = { enter_col, NULL, key_col, board_tick, board_draw, focused_cur, NULL, NULL, board_origin };
Screen discScreen = { enter_disc, NULL, key_disc, board_tick, board_draw, focused_cur, NULL, NULL, board_origin };
__attribute__((constructor)) static void board_kinds(void) { boards[0].kind = 0; boards[1].kind = 1; boards[2].kind = 2; boards[1].featOn = 1; tween_set(&boards[1].rowsA, 1); tween_set(&boards[0].rowsA, 1); tween_set(&boards[0].topY, ROWS_TOP); for (int i = 0; i < 2; i++) tween_set(&boards[i].logoIn, 1); } /* Discover opens on its spotlight */
int board_sample(Item *out, int max, char *rowTitle, int cap) {
  int n = 0; if (cap > 0) rowTitle[0] = 0;
  for (int k = 0; k < 2 && !n; k++) { Board *b = &boards[k];
    for (int i = 0; i < b->n && n < max; i++) { const Row *r = &b->rows[i]; if (r->wide || r->hidden) continue;
      if (cap > 0 && !rowTitle[0] && r->items.n) snprintf(rowTitle, (size_t)cap, "%s", T(r->title));
      for (int j = 0; j < r->items.n && n < max; j++) if (r->items.items[j].poster[0]) out[n++] = r->items.items[j]; } }
  return n;
}
