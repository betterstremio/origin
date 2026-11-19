#include "core/app.h"
#include "platform/plat.h"
#include "core/bgtrailer.h"
#include "core/best.h"
#include "core/keys.h"
#include "core/store.h"
#include "core/i18n.h"
#include "ui/shared.h"
#include "ui/orbs.h"
#include "ui/widgets.h"
#include "ui/sound.h"
#include "screens/revpanel.h"
#include "ui/glass.h"
#include "screens/welcome.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/img.h"
#include "ui/icon.h"
#include "ui/palette.h"
#include "screens/player.h"
#include "screens/saver.h"
#include "core/hit.h"
#include "core/nuvio.h"
#include "screens/profiles.h"
#include "ui/qrcard.h"
#include "core/recs.h"
#include "core/sync.h"
#include "core/update.h"
#include "screens/btrailer.h"
#include "screens/board.h"
#include "screens/login.h"
#include "screens/splash.h"
#include "core/castlink.h"
#include <SDL2/SDL.h>
#include "core/form.h"
#include "core/touch.h"
#include "screens/phone.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

Scr SCR = SCR_HOME;
Screen *screens[SCR_N];
Screen *app_overlay;
static Screen *seenOv[8];
static int nseenOv; /* overlays that have shown: drawn and ticked while they close */
void app_overlay_keep(Screen *s) {
  for (int k = 0; k < nseenOv; k++)
    if (seenOv[k] == s)
      return;
  if (nseenOv < 8)
    seenOv[nseenOv++] = s;
} /* drawn and ticked from now on, shown or not */
int app_splash_done;
int (*app_resume_hook)(const Item *it);
void (*app_card_menu_hook)(const Item *it);
void (*app_play_hook)(const Stream *s, const char *type, const char *id, const char *vid, int fromStart);
void app_play(const Stream *s, const char *type, const char *id, const char *vid, int fromStart) {
  if (!s->url[0]) {
    app_open_external(s->ext, s->name);
    return;
  }
  btr_release(); /* one decoder: the background trailer lets go first */
  if (app_play_hook)
    app_play_hook(s, type, id, vid, fromStart);
  else
    toast(T("Could not play"), 2000, NULL);
}
static char fromStartOnce[160];
/* "Watch from the beginning": the next play of this title/episode ignores the saved position */
void app_play_from_start(const Item *it) {
  CwInfo ci;
  const char *vid = it->videoId[0] ? it->videoId : (lib_cw_info(it, &ci) ? ci.vid : it->id);
  snprintf(fromStartOnce, sizeof fromStartOnce, "%s|%s", it->id, vid);
  Stream st;
  if (sb_resume_stream(it->id, vid, &st)) {
    app_play(&st, it->type, it->id, vid, 1);
    return;
  }
  if (it->ep) {
    extern void detail_open_streams(const char *vid);
    detail_open_streams(vid);
    return;
  }
  DetailParams p;
  memset(&p, 0, sizeof p);
  p.it = *it;
  if (strcmp(vid, it->id))
    snprintf(p.it.videoId, sizeof p.it.videoId, "%s", vid);
  p.openStreams = 1;
  app_go(SCR_DETAIL, &p, 0);
}
static Scr backStack[8];
static float backX[8], backY[8];
static int nback; /* with where each screen was left from */
static Timer1 xin; /* the entrance fade of the screen just entered */
/* where the screen coming in comes from: a tab slides in from the side you moved to, a title page grows out of the
   poster you pressed (Similar, a person's films: out of the button or the face), Search and Settings out of their
   icons, and Back shrinks the page toward that same spot; one short transform of the incoming screen, nothing more */
enum { NAV_NONE, NAV_ZOOM, NAV_BACKZ, NAV_SLIDE };
static struct {
  int kind;
  float x, y, dx;
} navf, xmv;
static Timer1 xmvT;
static Scr xoutScr;
static Timer1 xoutT; /* the page left by Back, leaving over the next one (its own exit) */
#define XOUT_DUR 0.2f
static float DT;
static double NOW;
static unsigned FRAME;
static double lastIn;
static int quitting;

float app_dt(void) {
  return DT;
}
double app_now(void) {
  return NOW;
}
unsigned app_frame(void) {
  return FRAME;
}
unsigned anim_moves; /* (anim.h) */
float app_idle(void) {
  return (float)(NOW - lastIn);
}
static int rtlHeld; /* a picture drawn left to right and mirrored as a whole (the guide's) */
int app_rtl(void) {
  return rtlHeld ? 0 : i18n_rtl();
}
void app_rtl_hold(int on) {
  rtlHeld = on;
}
int app_mirror_key(int k) {
  if (!i18n_rtl())
    return k;
  return k == K_LEFT ? K_RIGHT : k == K_RIGHT ? K_LEFT : k;
}
int app_is_board(Scr s) {
  return s == SCR_HOME || s == SCR_DISC || s == SCR_LIB;
}
static int xinLib; /* the Library's entrance runs 0.42 s on the page's main curve; the others 0.3 s ease-out */
float app_screen_alpha(void) {
  return xin.on ? ease_apply(EASE_CSS_EASEOUT, timer_x(&xin)) : 1;
} /* an even ease-out: the front-loaded curves read as a pop */
static int scr_origin(float *x, float *y) {
  if (screens[SCR] && screens[SCR]->origin && screens[SCR]->origin(x, y))
    return 1;
  *x = SB_W / 2;
  *y = SB_H / 2;
  return 0;
}
static void nav_slide(int dir) {
  navf.kind = NAV_SLIDE;
  navf.dx = dir * (i18n_rtl() ? -1 : 1);
} /* the bar is mirrored in RTL: so is the side */
void app_quit(void) {
  if (!quitting)
    fprintf(stderr, "[exit] app_quit\n");
  quitting = 1;
}
int app_quitting(void) {
  return quitting;
}
/* first launch: everything stays hidden behind the splash, then rises in, rows staggered */
static double revealAt;
void app_reveal_start(void) {
  revealAt = NOW;
}
float app_reveal_x(float delay, float dur) {
  if (!revealAt)
    return 1;
  float t = (float)(NOW - revealAt) - delay;
  if (t <= 0)
    return 0;
  if (t >= dur)
    return 1;
  return t / dur;
}
int app_revealing(void) {
  return revealAt && NOW - revealAt < 1.2;
}
void app_register(Scr s, Screen *sc) {
  screens[s] = sc;
}

/* ---- nudge ---- */
void nudge_start(Nudge *n, int k) {
  k = app_mirror_key(k);
  float dx = k == K_LEFT ? -1 : k == K_RIGHT ? 1 : 0, dy = k == K_UP ? -1 : k == K_DOWN ? 1 : 0;
  if (!dx && !dy)
    return;
  if (n->t.on)
    return; /* a held key lets the push finish instead of restarting it */
  n->dx = dx * 14;
  n->dy = dy * 10;
  timer_start(&n->t, 0.36f);
}
void nudge_tick(Nudge *n, float dt) {
  timer_tick(&n->t, dt);
}
static float nudge_k(const Nudge *n) {
  if (!n->t.on)
    return 0;
  float x = timer_x(&n->t);
  x = 1 - powf(1 - x, 2); /* ease-out over the keyframes */
  if (x < 0.32f)
    return x / 0.32f;
  if (x < 0.7f)
    return 1 - (x - 0.32f) / 0.38f * 1.16f;
  return -0.16f * (1 - (x - 0.7f) / 0.3f);
}
float nudge_x(const Nudge *n) {
  return n->dx * nudge_k(n);
}
float nudge_y(const Nudge *n) {
  return n->dy * nudge_k(n);
}

/* ---- accent ---- */
uint32_t ACC, ACC2, ACC3, ACC_D, ACC_FG;
const char *ACCENT_KEYS[18] = { "purple", "violet", "fuchsia", "pink", "rose", "red", "orange", "amber",  "gold",
                                "lime",   "green",  "emerald", "teal", "cyan", "sky", "blue",   "indigo", "graphite" };
static const uint32_t ACCENTS[18][4] = {
  { 0x8c5cff, 0xb794ff, 0xc9b8ff, 0x5b34e6 }, { 0xa855f7, 0xd8b4fe, 0xe9d5ff, 0x7e22ce },
  { 0xd946ef, 0xf0abfc, 0xf5d0fe, 0xa21caf }, { 0xec4899, 0xf9a8d4, 0xfbcfe8, 0xbe185d },
  { 0xf43f5e, 0xfda4af, 0xfecdd3, 0xbe123c }, { 0xef4444, 0xfca5a5, 0xfecaca, 0xb91c1c },
  { 0xf97316, 0xfdba74, 0xfed7aa, 0xc2410c }, { 0xf59e0b, 0xfcd34d, 0xfde68a, 0xb45309 },
  { 0xeab308, 0xfde047, 0xfef08a, 0xa16207 }, { 0x84cc16, 0xbef264, 0xd9f99d, 0x4d7c0f },
  { 0x22c55e, 0x86efac, 0xbbf7d0, 0x15803d }, { 0x10b981, 0x6ee7b7, 0xa7f3d0, 0x047857 },
  { 0x14b8a6, 0x5eead4, 0xb5f5ea, 0x0f766e }, { 0x06b6d4, 0x67e8f9, 0xcffafe, 0x0e7490 },
  { 0x0ea5e9, 0x7dd3fc, 0xbae6fd, 0x0369a1 }, { 0x3b82f6, 0x93b4ff, 0xc4d6ff, 0x1d4ed8 },
  { 0x6366f1, 0xa5b4fc, 0xc7d2fe, 0x4338ca }, { 0x9aa3b5, 0xd3d8e2, 0xeef0f4, 0x525a6c }
}; /* (round the wheel, then graphite: a cool glassy grey; purple, the first, is the dynamic modes' default) */
#define ACC_DEFAULT ACCENTS[0]
const uint32_t *settings_accent_rgb(int i) {
  return ACCENTS[i < 0 ? 0 : i > 17 ? 17 : i];
}
static const uint32_t TINT_GREY[4] = { 0x9aa6bd, 0xc1c9d8, 0xe1e5ee, 0x5d6880 };
static uint32_t accFrom[5], accTo[5];
static Timer1 accFade;
static int accSet;
static float lum_of(uint32_t c) {
  float v[3] = { ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f };
  for (int i = 0; i < 3; i++)
    v[i] = v[i] <= 0.03928f ? v[i] / 12.92f : powf((v[i] + 0.055f) / 1.055f, 2.4f);
  return 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
}
static uint32_t fg_on(uint32_t a, uint32_t b) {
  float L = (lum_of(a) + lum_of(b)) / 2;
  return (1.05f / (L + 0.05f)) >= ((L + 0.05f) / 0.05f) ? 0xffffffffu : 0xff14111cu;
}
static void set_accent(const uint32_t c[4]) {
  uint32_t to[5] = { 0xff000000u | c[0], 0xff000000u | c[1], 0xff000000u | c[2], 0xff000000u | c[3],
                     fg_on(c[0], c[1]) };
  if (accSet && to[0] == accTo[0])
    return;
  if (accSet) {
    for (int i = 0; i < 5; i++)
      accFrom[i] = col_mix(accFrom[i], accTo[i], accFade.on ? timer_x(&accFade) : 1);
    timer_start(&accFade, 0.75f);
  } /* a new colour fades in, never snaps */
  else
    for (int i = 0; i < 5; i++)
      accFrom[i] = to[i];
  memcpy(accTo, to, sizeof to);
  accSet = 1;
}
static void accent_tick(float dt) {
  float k = accFade.on ? timer_x(&accFade) : 1;
  timer_tick(&accFade, dt);
  k = 1 - powf(1 - k, 2);
  ACC = col_mix(accFrom[0], accTo[0], k);
  ACC2 = col_mix(accFrom[1], accTo[1], k);
  ACC3 = col_mix(accFrom[2], accTo[2], k);
  ACC_D = col_mix(accFrom[3], accTo[3], k);
  ACC_FG = col_mix(accFrom[4], accTo[4], k);
}
uint32_t acc_a(float a) {
  return col_alpha(ACC, a);
}
static const char *accent_mode(void) {
  const char *a = store_get("accent", "logo");
  if (!strcmp(a, "duo"))
    a = "adaptive";
  return a;
}
int accent_dynamic(void) {
  const char *a = accent_mode();
  return !strcmp(a, "adaptive") || !strcmp(a, "logo");
}
static const uint32_t *fixed_accent(void) {
  const char *a = accent_mode();
  for (int i = 0; i < 18; i++)
    if (!strcmp(a, ACCENT_KEYS[i]))
      return ACCENTS[i];
  return ACCENTS[0];
}
/* a swatch made usable as an accent: its own hue and character, lightness/saturation brought into range */
static void accent_of(const Swatch *w, const Swatch *w2, uint32_t out[4]) {
  float s = clampf(w->s * 1.1f, .38f, .92f), h = w->h, l = clampf(w->l, .48f, .64f);
  out[0] = hsl2rgb(h, s, l) & 0xffffff;
  if (w2 && w2->ok && fabsf(fmodf(w2->h - h + 540, 360) - 180) > 20)
    out[1] = hsl2rgb(w2->h, clampf(w2->s * 1.1f, .38f, .92f), clampf(w2->l, .55f, .72f)) & 0xffffff;
  else
    out[1] = hsl2rgb(h, s, fminf(.78f, l + .15f)) & 0xffffff;
  out[2] = hsl2rgb(h, fminf(.9f, s), .86f) & 0xffffff;
  out[3] = hsl2rgb(h, s, .38f) & 0xffffff;
}
static const Swatch *first_ok(const Palette *p, int withMuted) {
  if (p->vibrant.ok)
    return &p->vibrant;
  if (p->light.ok)
    return &p->light;
  if (p->dark.ok)
    return &p->dark;
  if (withMuted) {
    if (p->muted.ok)
      return &p->muted;
    if (p->darkMuted.ok)
      return &p->darkMuted;
  }
  return NULL;
}
/* the colours found for a title, kept across runs (store node "tintMem2": id -> {cover:[..], logo:[..]}) */
typedef struct {
  char id[64], poster[512], logo[512];
  int artDone, logoDone;
  Palette art, logo_;
  double at;
} TintJob;
static TintJob tintJobs[8];
static cJSON *tint_mem(void) {
  cJSON *m = store_node("tintMem2");
  if (!m) {
    m = cJSON_CreateObject();
    store_set_node("tintMem2", m);
  }
  return m;
}
static int tint_for(const char *id, uint32_t out[4]) {
  cJSON *m = cJSON_GetObjectItem(tint_mem(), id);
  if (!m)
    return 0;
  cJSON *a = !strcmp(accent_mode(), "logo") ? cJSON_GetObjectItem(m, "logo") : NULL;
  if (!a)
    a = cJSON_GetObjectItem(m, "cover");
  if (!a || cJSON_GetArraySize(a) < 4)
    return 0;
  for (int i = 0; i < 4; i++)
    out[i] = (uint32_t)cJSON_GetArrayItem(a, i)->valuedouble;
  return 1;
}
static char scopeId[64], scopePoster[512], scopeLogo[512];
static void tint_settle(TintJob *j) {
  const Swatch *ca = first_ok(&j->art, 1), *cl = first_ok(&j->logo_, 0);
  cJSON *e = cJSON_CreateObject();
  uint32_t c[4];
  if (ca)
    accent_of(ca, NULL, c);
  else
    memcpy(c, TINT_GREY, sizeof c);
  cJSON *arr = cJSON_CreateArray();
  for (int i = 0; i < 4; i++)
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(c[i]));
  cJSON_AddItemToObject(e, "cover", arr);
  if (cl && cl->s >= .3f) {
    accent_of(cl, NULL, c);
    arr = cJSON_CreateArray();
    for (int i = 0; i < 4; i++)
      cJSON_AddItemToArray(arr, cJSON_CreateNumber(c[i]));
    cJSON_AddItemToObject(e, "logo", arr);
  } /* a logo in white or grey says nothing: the artwork then */
  cJSON *m = tint_mem();
  cJSON_DeleteItemFromObject(m, j->id);
  cJSON_AddItemToObject(m, j->id, e);
  while (cJSON_GetArraySize(m) > 200)
    cJSON_DeleteItemFromArray(m, 0); /* oldest out */
  store_set_node("tintMem2", cJSON_Duplicate(m, 1));
  char id[64];
  snprintf(id, sizeof id, "%s", j->id);
  j->id[0] = 0;
  if (scopeId[0] && !strcmp(scopeId, id))
    accent_scope(scopeId, scopePoster, scopeLogo); /* the page waiting for it */
}
void accent_tint_fetch(const char *id, const char *poster, const char *logo) {
  if (!accent_dynamic() || !id || !*id || !poster || !*poster)
    return;
  if (cJSON_GetObjectItem(tint_mem(), id))
    return;
  for (int i = 0; i < 8; i++)
    if (!strcmp(tintJobs[i].id, id)) {
      if (logo && *logo && !tintJobs[i].logo[0])
        snprintf(tintJobs[i].logo, sizeof tintJobs[i].logo, "%s", logo);
      return;
    }
  int slot = -1;
  for (int i = 0; i < 8; i++)
    if (!tintJobs[i].id[0]) {
      slot = i;
      break;
    }
  if (slot < 0) {
    slot = 0;
    for (int i = 1; i < 8; i++)
      if (tintJobs[i].at < tintJobs[slot].at)
        slot = i;
  }
  TintJob *j = &tintJobs[slot];
  memset(j, 0, sizeof *j);
  snprintf(j->id, sizeof j->id, "%s", id);
  snprintf(j->poster, sizeof j->poster, "%s", poster);
  if (logo && *logo)
    snprintf(j->logo, sizeof j->logo, "%s", logo);
  else
    j->logoDone = 1;
  if (!sb_is_imdb(id))
    j->logoDone = 1;
  j->at = NOW;
}
static void tint_tick(void) {
  for (int i = 0; i < 8; i++) {
    TintJob *j = &tintJobs[i];
    if (!j->id[0])
      continue;
    if (!j->artDone) {
      int r = img_palette(j->poster, 264, 396, 0, &j->art);
      if (r)
        j->artDone = 1;
      if (r < 0)
        memset(&j->art, 0, sizeof j->art);
    }
    if (!j->logoDone) {
      int r = img_palette(j->logo, 520, 175, 1, &j->logo_);
      if (r)
        j->logoDone = 1;
      if (r < 0)
        memset(&j->logo_, 0, sizeof j->logo_);
    }
    if (j->artDone && j->logoDone) {
      if (!first_ok(&j->art, 1) && !first_ok(&j->logo_, 0)) {
        j->id[0] = 0;
        continue;
      }
      tint_settle(j);
    }
  }
}
/* a title's own accent (Settings' preview of the dynamic modes): 1 with it in out, 0 while it is worked out */
int accent_tint_of(const char *id, const char *poster, const char *logo, uint32_t out[4]) {
  if (tint_for(id, out))
    return 1;
  accent_tint_fetch(id, poster, logo);
  return 0;
}
static int tint_busy(const char *id) {
  for (int i = 0; i < 8; i++)
    if (!strcmp(tintJobs[i].id, id))
      return 1;
  return 0;
}
void accent_apply(void) {
  if (accent_dynamic())
    accent_scope(scopeId[0] ? scopeId : NULL, scopePoster, scopeLogo);
  else
    set_accent(fixed_accent());
}
/* Dynamic accents ("From the logo", "From the artwork") belong to ONE title: they show on that title's
   page and in its player; everywhere else the app keeps the default (purple). */
void accent_scope(const char *id, const char *poster, const char *logo) {
  if (id != scopeId) {
    if (id && *id) {
      snprintf(scopeId, sizeof scopeId, "%s", id);
      snprintf(scopePoster, sizeof scopePoster, "%s", poster ? poster : "");
      snprintf(scopeLogo, sizeof scopeLogo, "%s", logo ? logo : "");
    } else
      scopeId[0] = 0;
  }
  if (!accent_dynamic()) {
    set_accent(fixed_accent());
    return;
  }
  uint32_t c[4];
  if (scopeId[0] && tint_for(scopeId, c)) {
    set_accent(c);
    return;
  }
  if (scopeId[0]) {
    accent_tint_fetch(scopeId, scopePoster, scopeLogo);
    if (tint_busy(scopeId))
      return;
  } /* coming in a moment: keep what is shown, no purple flash */
  set_accent(ACC_DEFAULT);
}

/* ---- toast ---- */
static char toastMsg[256], toastKey[32];
static double toastUntil;
static Tween toastA;
static int toastOn, toastKind;
static Timer1 toastNew;
/* the icon a notice carries: working on it (ends in an ellipsis), something went wrong, or done. Read from the
   English text (screens pass it translated: looked up back) */
enum { TK_NONE, TK_WORK, TK_ERR, TK_OK };
static int has_ci(const char *s, const char *w) {
  size_t n = strlen(w);
  for (; *s; s++)
    if (!strncasecmp(s, w, n))
      return 1;
  return 0;
}
static int toast_kind_of(const char *m) {
  size_t n = strlen(m);
  if ((n >= 3 && !strcmp(m + n - 3, "\xe2\x80\xa6")) || (n >= 3 && !strcmp(m + n - 3, "...")))
    return TK_WORK;
  const char *en = i18n_key_of(m);
  if (en)
    m = en;
  static const char *er[] = { "fail",        "could not", "couldn", "unavailable", "no trailer",
                              "no embedded", "needs a",   "error",  "nothing to" };
  static const char *ok[] = { "updated",        "signed in", "in english", "in your language",
                              "latest version", "synced",    "saved" };
  for (int i = 0; i < 9; i++)
    if (has_ci(m, er[i]))
      return TK_ERR;
  for (int i = 0; i < 7; i++)
    if (has_ci(m, ok[i]))
      return TK_OK;
  return TK_NONE;
}
void toast(const char *msg, int ms, const char *key) {
  fprintf(stderr, "[toast] %s\n", msg); /* the kept log shows what the screen said */
  if (toastOn && strcmp(toastMsg, msg))
    timer_start(&toastNew, 0.25f); /* a new message over an old one: the words fade in, the card stays */
  snprintf(toastMsg, sizeof toastMsg, "%s", msg);
  snprintf(toastKey, sizeof toastKey, "%s", key ? key : "");
  toastKind = key && *key ? TK_NONE : toast_kind_of(msg);
  toastUntil = NOW + (ms > 0 ? ms : 2600) / 1000.0;
  if (!toastOn) {
    toastOn = 1;
    tween_to(&toastA, 1, 0.4f, EASE_CSS_EASE);
  }
}
/* notices (one style everywhere): an opaque card, bottom centre; it comes up from below and goes back down (a calm
   'ease': a notice is read, not chased); a leading mark says what it is (working, done, failed); a key badge in the
   keycap style of the hints */
static void toast_tick(float dt) {
  tween_tick(&toastA, dt);
  timer_tick(&toastNew, dt);
  if (toastOn && NOW >= toastUntil) {
    toastOn = 0;
    tween_to(&toastA, 0, 0.3f, EASE_CSS_EASE);
  }
}
static void toast_draw(void) {
  float a = toastA.v;
  if (a <= 0.001f)
    return;
  int rtl = app_rtl();
  float tw = text_width(toastMsg, 26, 600, -0.1f), keyW = 0, kindW = toastKind ? 26 + 16 - 6 : 0;
  int keyG = !strcmp(toastKey, "\001back"); /* the remote's Back key, drawn */
  if (toastKey[0])
    keyW = keyG ? 38 + 14 - 6 : 24 + text_width(toastKey, 21, 600, 0) + 14 - 6;
  float w = 56 + kindW + keyW + tw, h = 72, x = SB_W / 2 - w / 2, y = SB_H - 64 - h + 36 * (1 - a);
  if (form_touch() &&
      !player_open()) { /* a phone: a smaller pill above the tab bar, its words as wide as the screen allows */
    float pw = text_width(toastMsg, 14, 600, 0) + 36;
    if (pw > SB_W - 32)
      pw = SB_W - 32;
    float px = SB_W / 2 - pw / 2, py = phone_bottom() - 60 + 16 * (1 - a);
    gfx_opacity_push(a);
    gfx_shadow(px, py + 8, pw, 44, 22, 24, BLACK_A(110));
    gfx_rrect(px, py, pw, 44, 22, RGB(0x2a, 0x27, 0x35));
    text_draw(toastMsg, SB_W / 2, py + 22 - text_line_height(14) / 2.0f, 14, 600, WHITE_A(240), 0, pw - 28,
              ALIGN_CENTER);
    gfx_opacity_pop();
    return;
  }
  gfx_opacity_push(a);
  gfx_shadow(x, y + 16, w, h, 18, 40, BLACK_A(115));
  gfx_rrect(x, y, w, h, 18, RGB(0x1f, 0x1c, 0x28));
  gfx_ring(x, y, w, h, 18, 1, WHITE_A(15));
  float cx = rtl ? x + w - 28 : x + 28; /* the pen: left to right, or right to left */
  if (toastKind) {
    float ix = rtl ? cx + 6 - 26 : cx - 6, iy = y + h / 2 - 13;
    if (toastKind == TK_WORK) { /* a faint ring whose top quarter turns */
      gfx_ring(ix, iy, 26, 26, 13, 3, WHITE_A(41));
      float a0 = fmodf((float)NOW / 0.8f, 1) * 360;
      for (int k = 0; k <= 12; k++) {
        float an = (a0 + k * 7.5f - 45) * (float)M_PI / 180;
        gfx_circle(ix + 13 + 11.5f * sinf(an), iy + 13 - 11.5f * cosf(an), 1.5f, ACC3);
      }
    } else if (toastKind == TK_ERR) {
      gfx_circle(ix + 13, iy + 13, 13, RGBA(239, 143, 138, 46));
      gfx_rrect(ix + 11, iy + 6, 4, 9, 2, RGB(0xef, 0x8f, 0x8a));
      gfx_rrect(ix + 11, iy + 17, 4, 4, 2, RGB(0xef, 0x8f, 0x8a));
    } else {
      gfx_circle(ix + 13, iy + 13, 13, ACC);
      icon_draw("check", ix + 4, iy + 4, 18, ACC_FG);
    }
    cx += rtl ? -kindW : kindW;
  }
  if (keyG) {
    key_hint(rtl ? cx + 6 : cx - 6, y + h / 2, K_BACK, NULL, WHITE_A(235), rtl);
    cx += rtl ? -keyW : keyW;
  } else if (toastKey[0]) {
    float kw = 24 + text_width(toastKey, 21, 600, 0), kh = 8 + text_line_height(21), kx = rtl ? cx + 6 - kw : cx - 6;
    gfx_rrect(kx, y + h / 2 - kh / 2, kw, kh, 9, WHITE_A(31));
    text_draw(toastKey, kx + kw / 2, y + h / 2 - text_line_height(21) / 2.0f, 21, 600, WHITE_A(235), 0, 0,
              ALIGN_CENTER);
    cx += rtl ? -keyW : keyW;
  }
  float na = toastNew.on ? ease_apply(EASE_CSS_EASE, timer_x(&toastNew)) : 1;
  if (na < 1)
    gfx_opacity_push(na); /* a new message over an old one: the words fade in, the card stays */
  text_draw(toastMsg, cx, y + h / 2 - text_line_height(26) / 2.0f, 26, 600, WHITE_A(240), -0.1f, 0,
            rtl ? ALIGN_RIGHT : ALIGN_LEFT);
  if (na < 1)
    gfx_opacity_pop();
  gfx_opacity_pop();
}
static double exitAt = -10;
void exit_ask(void) {
  if (NOW - exitAt < 2.5) {
    app_quit();
    return;
  }
  exitAt = NOW;
  toast(T("Press again to exit"), 2500, "\001back");
}

/* ---- the clock ---- */
static char clockTxt[8], clockOld[8];
static Timer1 clockAnim;
static double clockAt;
static void clock_tick(void) {
  timer_tick(&clockAnim, DT);
  if (NOW - clockAt < 1)
    return;
  clockAt = NOW;
  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  char s[8];
  snprintf(s, sizeof s, "%02d:%02d", tm.tm_hour, tm.tm_min);
  if (strcmp(s, clockTxt)) {
    int first = !clockTxt[0];
    snprintf(clockOld, sizeof clockOld, "%s", clockTxt);
    snprintf(clockTxt, sizeof clockTxt, "%s", s);
    if (!first)
      timer_start(&clockAnim, 0.6f);
  }
}
/* a new minute: only the digits that changed crossfade, drifting a few px (the rest of the time stays still); the
   time sits from a fixed left edge so nothing beside the changing digits moves */
#if SB_WINDOWED
#define FS_ROOM (76.0f + 16) /* the fullscreen button right of the clock (app_fs_button) */
#else
#define FS_ROOM 0.0f
#endif
static float lg_drift_at(float ph);
void app_clock_draw(float right, float y, float alpha) {
  static float W;
  if (W <= 0)
    W = text_width("00:00", 38, 600, -0.6f);
  float x0 = right - W;
  if (alpha < 1)
    gfx_opacity_push(alpha);
  {
    float ph = 76, pw = W + 52, px = x0 - 26,
          py = y + text_line_height(38) / 2 - ph / 2; /* a glass pill behind it, the bar's circles' sister */
    lglass_light(lg_drift_at(113.0f));
    lglass_shell(px, py, pw, ph, ph / 2, RGBA(14, 14, 22, 120), 0.85f);
    lglass_light(0);
  }
  {
    static float cw = -1, cwTo = -1;
    float tw = text_width(clockTxt, 38, 600, -0.6f);
    if (cw < 0)
      cw = tw;
    cwTo = tw;
    cw += (cwTo - cw) * fminf(1, DT * 8);
    x0 += (W - cw) / 2;
  } /* the time centred in its pill (gliding when its width changes) */
  float k = clockAnim.on ? ease_apply(EASE_IN_OUT, timer_x(&clockAnim)) : 1;
  size_t n = 0;
  if (k < 1)
    while (clockTxt[n] && clockTxt[n] == clockOld[n])
      n++;
  else
    n = strlen(clockTxt);
  char pre[8];
  snprintf(pre, sizeof pre, "%.*s", (int)n, clockTxt);
  float pw = n ? text_width(pre, 38, 600, -0.6f) : 0;
  if (n)
    text_draw(pre, x0, y, 38, 600, WHITE_A(230), -0.6f, 0, ALIGN_LEFT);
  if (k < 1) {
    gfx_opacity_push(1 - k);
    text_draw(clockOld + n, x0 + pw, y - 5 * k, 38, 600, WHITE_A(230), -0.6f, 0, ALIGN_LEFT);
    gfx_opacity_pop();
    gfx_opacity_push(k);
    text_draw(clockTxt + n, x0 + pw, y + 5 * (1 - k), 38, 600, WHITE_A(230), -0.6f, 0, ALIGN_LEFT);
    gfx_opacity_pop();
  }
  if (alpha < 1)
    gfx_opacity_pop();
}
/* ---- the top bar (3.0.0, so/header-SPEC.md): the profile picture at the start, Search and the four tabs as plain
   words centred, one light pill that glides to the focused item, nothing at the end (a window: its fullscreen
   button). No glass, no accent: the page shows through ---- */
#define BAR_Y 56.0f /* the bar's centre line */
#define BAR_H 60.0f /* an item (its pill) */
#define PIC_X 68.0f /* the profile picture: 48 square, rounded */
#define PIC_D 48.0f
#define PILL_ON RGB(0xd9, 0xd9, 0xd9) /* the focus: a light pill, dark words */
#define INK_ON RGB(0x14, 0x14, 0x14)
static float lg_drift_at(float ph) {
  float t = (float)NOW + ph;
  return 0.018f * sinf(t * 0.157f) + 0.012f * sinf(t * 0.263f + 1.3f) + 0.008f * sinf(t * 0.411f + 4.1f);
} /* (the player's clock: its glass's light, three slow waves that never line up) */
TopNav topNav = { .idx = NAV_HOME };
static int tabPx = 28; /* the words' size (24 when a language's words would crowd the bar) */
static const char *chipLabel(int i) {
  return i == NAV_HOME ? T("Home") : i == NAV_SHOWS ? T("Shows") : i == NAV_MOVIES ? T("Movies") : T("My List");
}
/* the tab a page belongs to: a collection tile's page is the Shows / Movies board's, the other pages under a tab are
   counted as Home's */
static Scr tab_scr(void) {
  return SCR == SCR_COL ? SCR_DISC : SCR == SCR_DETAIL || SCR == SCR_SIM || SCR == SCR_CAT ? SCR_HOME : SCR;
}
static int tab_of(Scr s) { /* the bar's item for a page, -1 none */
  return s == SCR_SEARCH ? NAV_SEARCH
         : s == SCR_HOME ? NAV_HOME
         : s == SCR_DISC ? (board_disc_type() == 2 ? NAV_MOVIES : NAV_SHOWS)
         : s == SCR_LIB  ? NAV_LIB
         : s == SCR_SET  ? NAV_SET
                         : -1;
}
static Scr chipScr(int i) {
  return i == NAV_SEARCH                     ? SCR_SEARCH
         : i == NAV_HOME                     ? SCR_HOME
         : i == NAV_SHOWS || i == NAV_MOVIES ? SCR_DISC
         : i == NAV_LIB                      ? SCR_LIB
                                             : SCR_SET;
}
static int chipCur(int i) {
  return tab_of(tab_scr()) == i;
}
static int chipFocused(int i) {
  return topNav.active && topNav.idx == i && !update_pill_focused() && !profpanel_is_open();
} /* the notice holds the highlight while it has it */
static void topnav_layout(void) {
  float pad = 36, w[NAV_N] = { 0 }, tot = 0;
  for (int pass = 0; pass < 3; pass++) {
    tot = BAR_H;
    w[NAV_SEARCH] = BAR_H;
    for (int i = NAV_HOME; i <= NAV_LIB; i++) {
      w[i] = text_width(chipLabel(i), tabPx, 500, 0) + 2 * pad;
      tot += 8 + w[i];
    }
    if (tot <= SB_W - 400)
      break;
    if (pad > 24)
      pad = 24;
    else if (tabPx > 24)
      tabPx = 24;
  } /* a long language: less room round the words first, then smaller words */
  int rtl = app_rtl();
  float x = SB_W / 2 - tot / 2;
  for (int k = 0; k <= NAV_LIB; k++) {
    int i = rtl ? NAV_LIB - k : k; /* mirrored: Search at the right end */
    topNav.chipX[i] = x;
    topNav.chipW[i] = w[i];
    x += w[i] + 8;
  }
  topNav.chipX[NAV_SET] = rtl ? SB_W - PIC_X - PIC_D : PIC_X; /* the picture: Settings opens out of it */
  topNav.chipW[NAV_SET] = PIC_D;
  topNav.laidOut = 1;
}
static void topnav_paint(void) {
  topnav_layout();
  int fc = topNav.active && topNav.idx >= 0 && topNav.idx <= NAV_LIB && !update_pill_focused() && !profpanel_is_open()
               ? topNav.idx
               : -1; /* (the profile menu out: its own pill is the focus) */
  if (fc >= 0) {
    float x = topNav.chipX[fc], w = topNav.chipW[fc];
    if (topNav.pillShown < 0.5f && !topNav.pill.on) {
      tween_set(&topNav.px, x);
      tween_set(&topNav.pw, w);
    } /* appears in place */
    else {
      tween_to(&topNav.px, x, GLIDE_DUR, GLIDE_EASE);
      tween_to(&topNav.pw, w, GLIDE_DUR, GLIDE_EASE);
    }
    tween_to(&topNav.pill, 1, 0.2f, EASE_CSS_EASE);
    topNav.pillShown = 1;
  } else {
    tween_to(&topNav.pill, 0, 0.2f, EASE_CSS_EASE);
    topNav.pillShown = 0;
  }
}
void topnav_focus(void) {
  topNav.active = 1;
  if (topNav.idx < 0 || topNav.idx > NAV_LIB)
    topNav.idx = NAV_HOME; /* Settings has no item: up from it lands on Home */
  topnav_paint();
}
void topnav_leave(void) {
  if (!topNav.active)
    return;
  topNav.active = 0;
  topnav_paint();
} /* the pointer went into the page: the bar's pill lets go too */
void topnav_switch(int i) {
  i = i < 0 ? 0 : i > NAV_SET ? NAV_SET : i;
  if (i == NAV_SHOWS || i == NAV_MOVIES)
    board_disc_type_set(i == NAV_SHOWS ? 1 : 2); /* one board, the kind of title it holds set by the tab */
  if (i == topNav.idx && (app_is_board(SCR) || SCR == SCR_SEARCH || SCR == SCR_SET) && tab_of(tab_scr()) == i)
    return;
  {
    int cs = tab_of(tab_scr());
    if (i == NAV_SEARCH || i == NAV_SET) {
      navf.kind = NAV_ZOOM;
      navf.x = topNav.chipX[i] + topNav.chipW[i] / 2;
      navf.y = BAR_Y;
    } else
      nav_slide(i >= cs ? 1 : -1);
  }
  topNav.idx = i;
  nback = 0;
  topNav.active = 1;
  app_go(chipScr(i), NULL, 1);
  topNav.active = 1;
  topnav_paint();
}
static void topnav_flush(int drop) {
  if (topNav.want < 0)
    return;
  int w = topNav.want;
  topNav.want = -1;
  if (drop)
    return;
  topNav.idx = -1;
  topnav_switch(w);
}
static void topnav_enter_content(void) {
  topNav.active = 0;
  extern void board_from_bar(void), lib_from_bar(void);
  if (SCR == SCR_HOME || SCR == SCR_DISC)
    board_from_bar(); /* a board: its first row (its spotlight) */
  else if (SCR == SCR_LIB)
    lib_from_bar(); /* My List: its categories */
  else if (SCR != SCR_SEARCH && SCR != SCR_SET && screens[SCR] && screens[SCR]->key)
    screens[SCR]->key(K_DOWN); /* Search and Settings keep their own top highlight */
  topnav_paint();
}
static void topnav_nudge(void) {
  nudge_start(&topNav.nudge, K_UP);
}
/* the pill leaves on the key press; the screen follows once the remote rests (150 ms): building a screen in
   the same moment starves the slide, and passing through a tab on the way to another one no longer builds it */
static int topnav_key(int k) {
  if (update_pill_key(k, topNav.idx == NAV_SEARCH)) {
    topnav_paint();
    return 1;
  } /* a notice out beside the picture: Left from Search lands on it */
  topnav_paint();
  if (k == K_LEFT && topNav.idx == NAV_SEARCH) {
    topnav_flush(1);
    if (!profpanel_open())
      nudge_start(&topNav.nudge, k);
    return 1;
  } /* past Search: the profiles and the app's own items, out of the left edge */
  if (k == K_LEFT || k == K_RIGHT) {
    int i = topNav.idx + (k == K_RIGHT ? 1 : -1);
    if (i < 0)
      i = 0;
    if (i > NAV_LIB)
      i = NAV_LIB;
    if (i == topNav.idx) {
      nudge_start(&topNav.nudge, k);
      return 1;
    } /* the end of the bar */
    if (i == NAV_SEARCH && SCR != SCR_SEARCH) {
      topnav_flush(1);
      topNav.idx = i;
      topnav_paint();
      return 1;
    } /* Search opens on OK only */
    topNav.idx = i;
    topnav_paint();
    topNav.want = i;
    topNav.swAt = NOW + 0.15;
  } else if (k == K_DOWN || k == K_OK) {
    topnav_flush(0);
    if (topNav.idx == NAV_SEARCH && SCR != SCR_SEARCH) {
      if (k == K_OK) {
        topNav.active = 1;
        navf.kind = NAV_ZOOM;
        navf.x = topNav.chipX[NAV_SEARCH] + BAR_H / 2;
        navf.y = BAR_Y;
        app_go(SCR_SEARCH, NULL, 1);
        topnav_enter_content();
        return 1;
      }
      int c = tab_of(tab_scr()); /* Down from Search: into the page you are on */
      if (c >= 0 && c <= NAV_LIB)
        topNav.idx = c;
    }
    topnav_enter_content();
  } else if (k == K_UP)
    topnav_nudge();
  return 1;
}
void topnav_tick(float dt) {
  if (topNav.want >= 0 && NOW >= topNav.swAt)
    topnav_flush(0);
  tween_tick(&topNav.px, dt);
  tween_tick(&topNav.pw, dt);
  tween_tick(&topNav.pill, dt);
  nudge_tick(&topNav.nudge, dt);
  if (!topNav.laidOut)
    topnav_paint();
}
static void chip_focus(int i, int b) {
  (void)b;
  if (i == NAV_SET)
    return; /* the picture: a ring while pointed at (drawn), the panel on a click */
  if (!topNav.active || topNav.idx != i) {
    topnav_flush(1);
    topNav.active = 1;
    topNav.idx = i;
    topnav_paint();
  }
}
static void chip_act(int i, int b) {
  (void)b;
  if (app_overlay) {
    app_overlay->key(K_BACK);
    app_overlay = NULL;
  }
  if (i == NAV_SET) {
    profpanel_open();
    return;
  }
  topnav_flush(1);
  topNav.active = 1;
  topNav.idx = i;
  if (i == NAV_SEARCH)
    topnav_key(K_OK); /* opens it, the highlight inside */
  else if (i != tab_of(tab_scr()) || !app_is_board(SCR)) {
    topNav.idx = -1;
    topnav_switch(i);
  } else
    topnav_enter_content();
}
/* the profile in use (a Nuvio profile: its picture, or its colour and first letter; a Stremio account: the first
   letter of its address), rounded as Netflix's, at (x, y) in a square of D */
static void initial_draw(const char *name, float cx, float cy, float D) {
  char ini[8] = { 0 };
  const unsigned char *u = (const unsigned char *)(name ? name : "");
  int len = !*u ? 0 : (*u & 0x80) ? ((*u & 0xe0) == 0xc0 ? 2 : (*u & 0xf0) == 0xe0 ? 3 : 4) : 1;
  memcpy(ini, name, (size_t)len);
  if (ini[0] >= 'a' && ini[0] <= 'z')
    ini[0] -= 32;
  float px = D * 0.5f;
  text_draw(ini, cx, cy - text_line_height((int)px) / 2.0f, (int)px, 600, WHITE_A(255), 0, 0, ALIGN_CENTER);
}
void app_profile_pic(float x, float y, float D, float r) {
  if (acct_is_nuvio() && nv_profile_current()) {
    NvProfile p;
    for (int i = 0; i < nv_profile_count(); i++)
      if (nv_profile_get(i, &p) && p.index == nv_profile_current()) {
        gfx_rrect(x, y, D, D, r, RGB((p.color >> 16) & 255, (p.color >> 8) & 255, p.color & 255));
        if (!profile_avatar_draw_r(p.avatar, x, y, D, r))
          initial_draw(p.name, x + D / 2, y + D / 2, D);
        return;
      }
  }
  gfx_rrect(x, y, D, D, r, RGB(0x3a, 0x3a, 0x44));
  initial_draw(sb_user_email(), x + D / 2, y + D / 2, D);
}
static void caret_draw(float x, float cy, uint32_t c) {
  float px[4] = { x, x + 12, x + 6, x + 6 }, py[4] = { cy - 3.5f, cy - 3.5f, cy + 3.5f, cy + 3.5f };
  uint32_t cc[4] = { c, c, c, c };
  gfx_quad_pts(px, py, cc);
} /* a small down caret beside the picture */
void topnav_draw(void) {
  if (!topNav.laidOut)
    topnav_paint();
  int signin = SCR == SCR_LOGIN;
  float pg = revpanel_page_k(); /* the reviews risen into the title page: the bar goes up and away for its buttons */
  if (pg >= 0.999f)
    return;
  int rtl = app_rtl();
  if (!signin && pg < 0.3f) {
    for (int i = 0; i <= NAV_LIB; i++)
      hit_add_nav(topNav.chipX[i], BAR_Y - BAR_H / 2, topNav.chipW[i], BAR_H, chip_focus, chip_act, i, 0);
    hit_add_nav(rtl ? topNav.chipX[NAV_SET] - 34 : topNav.chipX[NAV_SET], BAR_Y - 30, PIC_D + 34, BAR_H, chip_focus,
                chip_act, NAV_SET, 0); /* the picture and its caret: one target */
  }
  float rv = ease_apply(EASE_CSS_EASE, app_reveal_x(0, 0.4f)) * (1 - pg);
  if (rv < 1)
    gfx_opacity_push(rv);
  float lift = -70 * pg;
  if (lift < 0)
    gfx_offset_push(0, lift);
  {
    static Tween fade;
    tween_to(&fade, !signin && board_rows_docked() ? 1 : 0, 0.3f, EASE_CSS_EASE);
    tween_tick(&fade, DT);
    if (fade.v > 0.001f)
      gfx_rrect_v(0, 0, SB_W, 160, 0, BLACK_A((int)(178 * fade.v)), BLACK_A(0));
  } /* rows under the bar: a soft dark fade at the top keeps the words readable (none over the billboard at rest) */
  float nx = nudge_x(&topNav.nudge), ny = nudge_y(&topNav.nudge);
  if (!signin) {
    /* the page you are on, the focus elsewhere: a dark grey pill */
    static Tween mk[NAV_LIB + 1], em[NAV_LIB + 1];
    for (int i = 0; i <= NAV_LIB; i++) {
      tween_to(&mk[i], chipCur(i) && !topNav.active ? 1 : 0, 0.2f, EASE_CSS_EASE);
      tween_tick(&mk[i], DT);
      if (mk[i].v > 0.01f)
        gfx_rrect(topNav.chipX[i], BAR_Y - BAR_H / 2, topNav.chipW[i], BAR_H, BAR_H / 2, WHITE_A((int)(46 * mk[i].v)));
    }
    /* the focus: one flat light pill gliding between the items */
    float pa = topNav.pill.v;
    if (pa > 0.001f) {
      gfx_offset_push(nx, ny);
      gfx_rrect(topNav.px.v, BAR_Y - BAR_H / 2, topNav.pw.v, BAR_H, BAR_H / 2, col_alpha(PILL_ON, pa));
      gfx_offset_pop();
    }
    for (int i = 0; i <= NAV_LIB; i++) {
      int f = chipFocused(i);
      tween_to(&em[i], f ? 1 : 0, 0.2f, EASE_CSS_EASE);
      tween_tick(&em[i], DT); /* the words go dark as the pill arrives under them */
      uint32_t rest = chipCur(i) && !topNav.active ? WHITE_A(255) : WHITE_A(230);
      uint32_t col = col_mix(rest, INK_ON, em[i].v);
      float x = topNav.chipX[i], w = topNav.chipW[i];
      if (f)
        gfx_offset_push(nx, ny);
      if (i == NAV_SEARCH)
        icon_draw("nav_search", x + (w - 32) / 2, BAR_Y - 16, 32, col);
      else
        text_draw(chipLabel(i), x + w / 2, BAR_Y - text_line_height(tabPx) / 2.0f, tabPx, 500, col, 0, 0, ALIGN_CENTER);
      if (f)
        gfx_offset_pop();
    }
    /* the start: a notice out of the picture, then the picture, its caret and (an update waiting) a dot */
    float px = topNav.chipX[NAV_SET], py = BAR_Y - PIC_D / 2, nk = update_dot_hide();
    update_pill_draw(px - 14);
    float pk = profpanel_k(); /* the menu out: its card holds the picture */
    if (pk < 0.001f)
      app_profile_pic(px, py, PIC_D, 8);
    if (app_ptr_over(rtl ? px - 34 : px, BAR_Y - 30, PIC_D + 34, BAR_H))
      gfx_ring(px - 4, py - 4, PIC_D + 8, PIC_D + 8, 12, 2, WHITE_A(204));
    if (nk < 0.999f && pk < 0.999f) {
      gfx_opacity_push((1 - nk) * (1 - pk));
      caret_draw(rtl ? px - 20 - 12 : px + PIC_D + 20, BAR_Y, WHITE_A(217));
      if (update_avail()) {
        float dx = rtl ? px + 2 : px + PIC_D - 2, dy = py + 2;
        gfx_circle(dx, dy, 7, PAGE);
        gfx_circle(dx, dy, 5, UPD_DOT);
      }
      gfx_opacity_pop();
    }
  }
  if (FS_ROOM > 0 && !welcome_is_open())
    app_fs_button(rtl ? 22 + 60 : SB_W - 22, BAR_Y);
  if (lift < 0)
    gfx_offset_pop();
  if (rv < 1)
    gfx_opacity_pop();
}

/* ---- the backdrop: two layers, the new one fades in over the old (never a dip to black) ---- */
#define BGW 1280
#define BGH 720
typedef struct {
  char url[512];
  GLuint tex;
  float aspect;
  GfxTarget baked;
  char bakedUrl[512];
  int mode, bakedMode, bakedRtl, bakedOk;
  float bakedW, bakedH;
} BgLayer; /* baked = the picture with its shading, made once per change */
static BgLayer bgL[2];
static int bgTop; /* bgL[bgTop] is above */
static Timer1 bgFade;
static int bgFadingIn, bgFadingOut;
static unsigned bgGen; /* bumps each time a new backdrop takes over */
static char bgCur[512], bgWant[512], bgAlt[512];
static double bgNotBefore;
static int bgWaiting;
static int bgMode; /* 0 = the main pages' shading, 1 = a title page's */
const char *bg_current(void) {
  return bgCur;
}
/* a change of shading is a crossfade of the same picture into a layer baked the new way (a mix of two shaded
   pictures is the mix of the shadings): no layer is baked again while the change runs (that was a full bake every
   frame for 0.3 s, the source picture reloaded for it) */
void bg_set_mode(int detail) {
  int m = detail ? 1 : 0;
  if (m == bgMode)
    return;
  bgMode = m;
  if (bgWaiting || !bgL[bgTop].url[0])
    return; /* the next picture comes with the new shading anyway */
  snprintf(bgWant, sizeof bgWant, "%s", bgL[bgTop].url);
  bgAlt[0] = 0;
  bgNotBefore = NOW;
  bgWaiting = 1;
}
static void bg_fade_away(void) {
  if (!bgL[bgTop].url[0] && !bgL[!bgTop].url[0])
    return;
  if (bgFade.on && bgFadingIn) {
    bgL[!bgTop].url[0] = 0;
    bgL[!bgTop].tex = 0;
  } /* the one still arriving is dropped */
  bgFadingIn = 0;
  bgFadingOut = 1;
  timer_start(&bgFade, 0.3f);
}
void bg_clear(void) {
  bgCur[0] = 0;
  bgWaiting = 0;
  bg_fade_away();
}
void bg_set(const char *u, int delayMs, const char *alt) {
  if (!u || !*u) {
    if (bgCur[0] || bgWaiting)
      bg_clear();
    return;
  }
  if (!strcmp(u, bgCur)) {
    if (bgWaiting && strcmp(bgWant, bgCur))
      bgWaiting = 0;
    return;
  } /* back on the one shown (A, B, A quickly): the B still on its way must not land after (a change of shading of A
       itself stays) */
  if (bgWaiting && !strcmp(u, bgWant))
    return;
  snprintf(bgWant, sizeof bgWant, "%s", u);
  snprintf(bgAlt, sizeof bgAlt, "%s", alt ? alt : "");
  bgNotBefore = NOW + delayMs / 1000.0;
  bgWaiting = 1;
}
static void bg_show(const char *u, GLuint tex, float aspect) {
  if (bgFade.on) { /* a fade still running ends now */
    if (bgFadingOut) {
      bgL[bgTop].url[0] = 0;
      bgL[bgTop].tex = 0;
      bgL[!bgTop].url[0] = 0;
      bgL[!bgTop].tex = 0;
    }
    bgFade.on = 0;
  }
  int nx = !bgTop; /* the hidden layer takes the picture and fades in above */
  if (!bgL[bgTop].url[0] && bgL[nx].url[0]) {
    nx = bgTop;
  }
  snprintf(bgL[nx].url, sizeof bgL[nx].url, "%s", u);
  bgL[nx].tex = tex;
  bgL[nx].aspect = aspect;
  bgL[nx].mode = bgMode;
  bgTop = nx;
  bgFadingIn = 1;
  bgFadingOut = 0;
  timer_start(&bgFade, 0.3f);
  bgGen++;
  snprintf(bgCur, sizeof bgCur, "%s", u);
}
static int bgBakeBroken; /* the driver cannot draw offscreen: draw it all every frame */
static int bg_baked_current(const BgLayer *l) {
  return !bgBakeBroken && l->bakedOk && !strcmp(l->bakedUrl, l->url) && l->bakedMode == l->mode &&
         l->bakedRtl == app_rtl() && l->bakedW == SB_W && l->bakedH == SB_H;
}
void bg_tick(float dt) {
  if (timer_tick(&bgFade, dt) >= 1 && !bgFade.on && (bgFadingIn || bgFadingOut)) {
    if (bgFadingOut) {
      bgL[0].url[0] = bgL[1].url[0] = 0;
      bgL[0].tex = bgL[1].tex = 0;
    } else {
      bgL[!bgTop].url[0] = 0;
      bgL[!bgTop].tex = 0;
    } /* the old one is dropped once the fade really ended */
    bgFadingIn = bgFadingOut = 0;
  }
  if (bgWaiting) {
    float aspect;
    img_urgent(bgWant, BGW, BGH);
    GLuint tex = img_get(bgWant, BGW, BGH, &aspect); /* decoded before anything asked after it */
    /* a crossfade running plays out first (cut short, its picture jumped in whole): the newest wanted follows it */
    if (tex) {
      if (NOW >= bgNotBefore && !(bgFade.on && bgFadingIn)) {
        bgWaiting = 0;
        bg_show(bgWant, tex, aspect);
      }
    } else if (img_failed(bgWant)) {
      if (bgAlt[0] && strcmp(bgAlt, bgWant)) {
        snprintf(bgWant, sizeof bgWant, "%s", bgAlt);
        bgAlt[0] = 0;
      } else {
        bgWaiting = 0;
        bgCur[0] = 0;
        bg_fade_away();
      }
    }
  }
  /* the shown pictures stay asked for (kept from eviction): the player opens on the same picture at the same size, and
     a change of shading bakes from it again */
  for (int i = 0; i < 2; i++)
    if (bgL[i].url[0]) {
      GLuint t = img_get(bgL[i].url, BGW, BGH, &bgL[i].aspect);
      bgL[i].tex = t;
    }
  if (!bgL[0].url[0] && !bgL[1].url[0])
    for (int i = 0; i < 2; i++)
      if (bgL[i].baked.fbo) {
        gfx_target_free(&bgL[i].baked);
        bgL[i].bakedOk = 0;
        bgL[i].bakedUrl[0] = 0;
      } /* nothing shown: the memory goes back */
}
/* the shading as the page draws it: made ONCE per kind (and side) as a small alpha picture in the page colour
   and stretched over each backdrop. The main pages: art in the upper part, a soft fade into the rows (solid from
   84% down), a darker left side and a soft oval vignette around (62%, 22%); a title page: the cover shows, with
   enough shade for the text on the left and the rows below. Layers of one colour stack as 1-(1-a)(1-b) */
#define VIGW 640
#define VIGH 360
static float lin_at(const float (*st)[2], int n, float t) {
  if (t <= st[0][0])
    return st[0][1];
  for (int i = 0; i + 1 < n; i++)
    if (t <= st[i + 1][0]) {
      float d = st[i + 1][0] - st[i][0];
      return d > 0 ? st[i][1] + (st[i + 1][1] - st[i][1]) * (t - st[i][0]) / d : st[i + 1][1];
    }
  return st[n - 1][1];
}
#define VTX 16
#define VTY 9
static unsigned char vigUsed[4][VTY]
                            [VTX]; /* per tile of the shading: 1 when any texel there (or one beside it) is not clear */
/* the shading's darkness at (u, v) of the screen: the page's gradients down and across, and (on the boards) its oval */
static const float mV[][2] = { { 0, .26f },    { .10f, 0 },    { .32f, 0 }, { .46f, .16f }, { .58f, .46f },
                               { .68f, .74f }, { .77f, .92f }, { .84f, 1 }, { 1, 1 } };
static const float mH[][2] = { { 0, .84f }, { .32f, .5f }, { .62f, .12f }, { 1, 0 } };
static const float dV[][2] = { { 0, .18f }, { .12f, 0 }, { .5f, 0 }, { .7f, .32f }, { .86f, .7f }, { 1, .92f } };
static const float dH[][2] = { { 0, .78f }, { .34f, .42f }, { .6f, .06f }, { 1, 0 } };
static float vig_at(int detail, int rtl, float u, float v) {
  float hx = rtl ? 1 - u : u, a1, a2, a3 = 0;
  if (detail) {
    a1 = lin_at(dV, 6, v);
    a2 = lin_at(dH, 4, hx);
  } else {
    a1 = lin_at(mV, 9, v);
    a2 = lin_at(mH, 4, hx);
    float ex = (u - (rtl ? .38f : .62f)) / .85f, ey = (v - .22f) / .7f,
          d = sqrtf(ex * ex + ey * ey); /* the oval: 85% x 70% of the screen */
    a3 = d <= .55f ? 0 : d >= 1 ? .36f : .36f * (d - .55f) / .45f;
  }
  return 1 - (1 - a1) * (1 - a2) * (1 - a3);
}
static GLuint vig_tex(int detail) {
  static GLuint tex[4];
  int k = detail * 2 + (app_rtl() ? 1 : 0);
  if (tex[k])
    return tex[k];
  unsigned char *px = malloc((size_t)VIGW * VIGH);
  int rtl = k & 1;
  for (int y = 0; y < VIGH; y++)
    for (int x = 0; x < VIGW; x++)
      px[y * VIGW + x] = (unsigned char)(vig_at(detail, rtl, (x + .5f) / VIGW, (y + .5f) / VIGH) * 255 + .5f);
  for (int ty = 0; ty < VTY; ty++)
    for (int tx = 0; tx < VTX; tx++) {
      int any = 0; /* (a texel beside the tile counts: the filter reads across its edge) */
      for (int y = ty * VIGH / VTY - 1; y <= (ty + 1) * VIGH / VTY && !any; y++)
        for (int x = tx * VIGW / VTX - 1; x <= (tx + 1) * VIGW / VTX && !any; x++)
          if (y >= 0 && y < VIGH && x >= 0 && x < VIGW && px[y * VIGW + x])
            any = 1;
      vigUsed[k][ty][tx] = (unsigned char)any;
    }
  tex[k] = gfx_tex_create(px, VIGW, VIGH, 1, 1);
  free(px);
  return tex[k];
}
/* the shading over the whole screen down to yMax: only its tiles that darken anything (the clear ones cost the GPU a
   full pass over the video for nothing), each run of them one quad; exactly what the one full quad drew */
static void vig_draw(int mode, uint32_t c, float yMax) {
  GLuint t = vig_tex(mode);
  int k = mode * 2 + (app_rtl() ? 1 : 0);
  for (int ty = 0; ty < VTY; ty++) {
    float y0 = (float)ty * SB_H / VTY, y1 = (float)(ty + 1) * SB_H / VTY;
    if (y0 >= yMax)
      break;
    if (y1 > yMax)
      y1 = yMax;
    for (int tx = 0; tx < VTX;) {
      if (!vigUsed[k][ty][tx]) {
        tx++;
        continue;
      }
      int e = tx;
      while (e < VTX && vigUsed[k][ty][e])
        e++;
      float x0 = (float)tx * SB_W / VTX, x1 = (float)e * SB_W / VTX;
      gfx_alpha_tex_uv(t, x0, y0, x1 - x0, y1 - y0, c, x0 / SB_W, y0 / SB_H, x1 / SB_W, y1 / SB_H);
      tx = e;
    }
  }
}
static void bg_picture(const BgLayer *l, float alpha) {
  float u0, v0, u1, v1;
  img_cover_uv(l->aspect, SB_W / SB_H, 1, &u0, &v0, &u1, &v1);
  gfx_image(l->tex, 0, 0, SB_W, SB_H, 0, WHITE_A((int)(255 * 0.86f * alpha + 0.5f)), u0, v0, u1, v1);
}
static void bg_shading(int mode, float alpha) {
  vig_draw(mode, RGBA(18, 18, 20, (int)(255 * alpha + .5f)), SB_H);
}
/* the layer as one opaque picture: shading a mix of two pictures is the mix of the two shaded
   pictures, so each layer carries its own shading and the crossfade stays exact */
static int bg_bake(BgLayer *l) {
  if (bgBakeBroken)
    return 0;
  if (bg_baked_current(l))
    return 1;
  if (!l->tex)
    return l->bakedOk && !strcmp(l->bakedUrl, l->url); /* the picture is reloading: the last bake of it stays */
  int bw, bh;
  gfx_screen_target_size(BGW, BGH, &bw, &bh);
  if (l->baked.fbo && (l->baked.w != bw || l->baked.h != bh))
    gfx_target_free(&l->baked);
  if (!l->baked.fbo && !gfx_target_create(&l->baked, bw, bh)) {
    bgBakeBroken = 1;
    return 0;
  }
  gfx_target_begin(&l->baked, PAGE);
  bg_picture(l, 1);
  bg_shading(l->mode, 1);
  gfx_target_end();
  snprintf(l->bakedUrl, sizeof l->bakedUrl, "%s", l->url);
  l->bakedMode = l->mode;
  l->bakedRtl = app_rtl();
  l->bakedW = SB_W;
  l->bakedH = SB_H;
  l->bakedOk = 1;
  return 1;
}
/* an opaque panel that holds still (the card menu, the streams sheet) says where it is, for the next frame: the
   backdrop is not drawn under it (hidden there; a third of a screen this TV's GPU then has to spare). Set inside
   its edges, so the first frame of its leaving never uncovers a gap */
static float occX, occY, occW, occH, occNX, occNY, occNW, occNH;
static int occOn, occNOn;
void app_occluder(float x, float y, float w, float h) {
  occNX = x;
  occNY = y;
  occNW = w;
  occNH = h;
  occNOn = 1;
}
void app_occluder_drop(void) {
  occOn = 0;
  occNOn = 0;
} /* a panel starting to move: the backdrop under it is drawn again from this frame */
static void bg_baked_draw(GLuint tex, float alpha) {
  uint32_t c = WHITE_A((int)(255 * alpha + 0.5f));
  if (!occOn) {
    gfx_image(tex, 0, 0, SB_W, SB_H, 0, c, 0, 1, 1, 0);
    return;
  }
  float x0 = occX, y0 = occY, x1 = occX + occW,
        y1 = occY + occH; /* the four bands around it, the picture's own place in each */
  float r[4][4] = { { 0, 0, SB_W, y0 }, { 0, y1, SB_W, SB_H }, { 0, y0, x0, y1 }, { x1, y0, SB_W, y1 } };
  for (int i = 0; i < 4; i++) {
    float a = r[i][0], b = r[i][1], e = r[i][2], f = r[i][3];
    if (e - a <= 0 || f - b <= 0)
      continue;
    gfx_image(tex, a, b, e - a, f - b, 0, c, a / SB_W, 1 - b / SB_H, e / SB_W, 1 - f / SB_H);
  }
}
static void bg_layer_draw(BgLayer *l, float alpha) {
  if (!l->url[0])
    return;
  if (bg_bake(l)) {
    bg_baked_draw(l->baked.tex, alpha);
    return;
  }
  if (l->tex) {
    bg_picture(l, alpha);
    bg_shading(l->mode, alpha);
  } /* no offscreen drawing: shaded as drawn */
}
/* the picture under the sheets' glass: the shown layer as it is drawn (baked with its shading), upside down */
unsigned bg_glass_tex(float *alpha) {
  BgLayer *l = &bgL[bgTop];
  if (!l->url[0])
    return 0;
  float k = bgFade.on ? 1 - powf(1 - timer_x(&bgFade), 2) : 1;
  if (alpha)
    *alpha = bgFadingOut ? 1 - k : 1;
  return bg_baked_current(l) || (l->bakedOk && !strcmp(l->bakedUrl, l->url)) ? l->baked.tex : 0;
}
void bg_draw(void) {
  float k = bgFade.on ? ease_apply(EASE_CSS_EASEOUT, timer_x(&bgFade)) : 1;
  /* first launch: the backdrop fades up after the start screen too (0.7 s, from 50 ms in) */
  float rv = ease_apply(EASE_CSS_EASE, app_reveal_x(0.05f, 0.7f));
  float a = rv;
  if (a <= 0.001f)
    return;
  if (bgFadingIn && bgFade.on) {
    bg_layer_draw(&bgL[!bgTop], a);
    bg_layer_draw(&bgL[bgTop], k * a);
  } else if (bgFadingOut && bgFade.on) {
    bg_layer_draw(&bgL[!bgTop], (1 - k) * a);
    bg_layer_draw(&bgL[bgTop], (1 - k) * a);
  } else
    bg_layer_draw(&bgL[bgTop], a);
}

/* ---- screens, keys ---- */
static double lastNav;
static int curKey; /* the trailer's own idleness: OK (a tap, a hold) neither counts nor stops it */
static void idle_touch(void) {
  lastIn = NOW;
  if (curKey == K_OK)
    return;
  lastNav = NOW;
  btr_key(curKey >= K_LEFT && curKey <= K_DOWN);
} /* any other key stops the background trailer at once */
float app_trailer_idle(void) {
  return (float)(NOW - lastNav);
}
static void idle_touch_by(const char *why) {
  if (bgt_state() == BGT_LOAD || bgt_state() == BGT_SHOW)
    fprintf(stderr, "[btr] input: %s\n", why);
  idle_touch();
}
void app_touch(void) {
  idle_touch();
}
/* the page Settings or Search was opened from: its colour key pressed again there goes back to it */
static Scr colorFrom;
static int colorFromSet;
void app_go(Scr s, const void *params, int noStack) {
  idle_touch();
  if (!noStack && SCR != s) {
    float ox, oy;
    int has = 1;
    if (navf.kind == NAV_ZOOM) {
      ox = navf.x;
      oy = navf.y;
    } else
      has = scr_origin(&ox, &oy); /* nothing focused: a plain fade in, Back toward the middle */
    if (nback == 8) {
      memmove(backStack, backStack + 1, 7 * sizeof *backStack);
      memmove(backX, backX + 1, 7 * sizeof *backX);
      memmove(backY, backY + 1, 7 * sizeof *backY);
      nback = 7;
    }
    backX[nback] = ox;
    backY[nback] = oy;
    backStack[nback++] = SCR;
    if (navf.kind == NAV_NONE && has && (s == SCR_DETAIL || s == SCR_SIM || s == SCR_CAT || s == SCR_COL)) {
      navf.kind = NAV_ZOOM;
      navf.x = ox;
      navf.y = oy;
    }
  }
  topNav.active = 0;
  Scr old = SCR;
  if ((s == SCR_SET || s == SCR_SEARCH) && old != SCR_SET && old != SCR_SEARCH && old != SCR_LOGIN) {
    colorFrom = old;
    colorFromSet = 1;
  } /* where Settings / Search were opened from */
  xoutT.on = 0;
  if (old != s && app_splash_done && navf.kind == NAV_BACKZ && screens[old] && screens[old]->out) {
    xoutScr = old;
    timer_start(&xoutT, XOUT_DUR);
  }
  int shared = (old == SCR_HOME || old == SCR_DISC) && (s == SCR_HOME || s == SCR_DISC);
  /* every switch is a hand-over (no screen pops in), signing out to the sign-in page too; Home and Discover share one
     layer and hand over on their own; signing in restarts behind the start screen */
  int xf = app_splash_done && old != s && old != SCR_LOGIN && !shared;
  {
    int tab =
        (old == SCR_HOME || old == SCR_DISC || old == SCR_LIB) && (s == SCR_HOME || s == SCR_DISC || s == SCR_LIB);
    if (old != s && app_splash_done && !tab)
      sx_begin(0);
  } /* page to page, the same logo, poster or words move across (never between the tabs) */
  if (screens[old] && screens[old]->leave && old != s)
    screens[old]->leave();
  SCR = s;
  if (s == SCR_HOME || s == SCR_LIB)
    sb_sync_soon(0);
  if (s != SCR_DETAIL)
    accent_scope(NULL, NULL, NULL); /* the title page sets its own cover colour */
  bg_set_mode(s == SCR_DETAIL);
  if (s == SCR_LOGIN || s == SCR_SEARCH || s == SCR_SET)
    bg_clear(); /* a title's cover never stays behind them */
  if (getenv("SB_NAVLOG"))
    fprintf(stderr, "[nav] %d -> %d xf %d shared %d splash %d at %.3f\n", old, s, xf, shared, app_splash_done, NOW);
  if (xf) {
    xinLib = s == SCR_LIB;
    timer_start(&xin, s == SCR_LIB ? 0.45f : 0.38f);
    xmv = navf;
    if (xmv.kind)
      timer_start(&xmvT, xmv.kind == NAV_SLIDE ? 0.45f : 0.4f);
    else
      xmvT.on = 0;
  }
  navf.kind = NAV_NONE;
  if (screens[s] && screens[s]->enter)
    screens[s]->enter(params);
  text_gc();
  topnav_paint();
}
/* Settings or Search, left for the page it was opened from (its colour key again, or Back on Settings) */
static void color_back(int t) {
  Scr p = colorFrom;
  colorFromSet = 0;
  int ci = tab_of(p);
  topnav_flush(1);
  navf.kind = NAV_BACKZ;
  navf.x = topNav.chipX[t] + topNav.chipW[t] / 2;
  navf.y = BAR_Y;
  topNav.idx = ci >= 0 && ci <= NAV_LIB ? ci : NAV_HOME;
  app_go(p, NULL, 1);
  topNav.active = 0;
  topnav_paint();
}
/* a phone's tab: the stack of pages starts again from it */
void app_go_tab(Scr s) {
  nback = 0;
  navf.kind = NAV_NONE;
  if (s != SCR)
    app_go(s, NULL, 1);
}
void app_back(void) {
  if (form_touch()) { /* a phone: the sheet, the page's own step back, the last page, Home, then Back twice leaves */
    if (app_overlay) {
      app_overlay->key(K_BACK);
      return;
    }
    if (screens[SCR] && screens[SCR]->key && screens[SCR]->key(K_BACK))
      return;
    if (SCR == SCR_LOGIN)
      return;
    if (nback) {
      Scr prev = backStack[--nback];
      navf.kind = NAV_BACKZ;
      navf.x = backX[nback];
      navf.y = backY[nback];
      app_go(prev, NULL, 1);
      return;
    }
    if (SCR != SCR_HOME) {
      app_go(SCR_HOME, NULL, 1);
      return;
    }
    exit_ask();
    return;
  }
  if (topNav.want >= 0) {
    topnav_flush(1);
    topNav.idx = tab_of(tab_scr());
    topnav_paint();
    return;
  } /* Back while the pill travels: it returns */
  if (app_overlay) {
    app_overlay->key(K_BACK);
    return;
  }
  if (SCR == SCR_LOGIN) {
    if (screens[SCR] && screens[SCR]->key)
      screens[SCR]->key(K_BACK);
    return;
  }
  if (screens[SCR] && screens[SCR]->key && screens[SCR]->key(K_BACK))
    return; /* the screen stepped back inside itself */
  int ci = tab_of(SCR);
  if (SCR == SCR_SET && colorFromSet && !topNav.active) {
    color_back(NAV_SET);
    return;
  } /* Settings (it has no tab): Back returns to the page it was opened from */
  if (topNav.active) {
    if (SCR != SCR_HOME) {
      nav_slide(NAV_HOME >= ci ? 1 : -1);
      topNav.idx = NAV_HOME;
      app_go(SCR_HOME, NULL, 1);
      topNav.active = 0;
      topnav_paint();
    } else
      exit_ask();
    return;
  }
  if (nback) {
    Scr prev = backStack[--nback];
    navf.kind = NAV_BACKZ;
    navf.x = backX[nback];
    navf.y = backY[nback];
    app_go(prev, NULL, 1);
    return;
  }
  if (ci >= 0) {
    topnav_focus();
    return;
  } /* inside a tab (Home, Discover, Library, Search, Settings): Back goes up to the tab bar first; from there, Home,
       then the exit question */
  if (SCR != SCR_HOME) {
    navf.kind = NAV_BACKZ;
    navf.x = SB_W / 2;
    navf.y = SB_H / 2;
    topNav.idx = NAV_HOME;
    app_go(SCR_HOME, NULL, 1);
    return;
  }
  exit_ask();
}
/* an addon's link to somewhere else (a stream's externalUrl, a meta link): the app's own pages for stremio:///detail
   links, a code to open it on the phone for everything else (the TV has no browser of ours) */
void app_open_external(const char *url, const char *name) {
  if (!url || !*url)
    return;
  if (!strncmp(url, "stremio:///detail/", 18)) {
    char t[300];
    snprintf(t, sizeof t, "%s", url + 18);
    char *ty = t, *id = strchr(t, '/');
    if (id) {
      *id++ = 0;
      char *vid = strchr(id, '/');
      if (vid)
        *vid++ = 0;
      DetailParams p;
      memset(&p, 0, sizeof p);
      snprintf(p.it.type, sizeof p.it.type, "%s", ty);
      snprintf(p.it.id, sizeof p.it.id, "%s", id);
      if (vid && *vid) {
        snprintf(p.it.videoId, sizeof p.it.videoId, "%s", vid);
        p.openStreams = 1;
      }
      if (*ty && *id) {
        app_go(SCR_DETAIL, &p, 0);
        return;
      }
    }
  }
  if (form_touch()) {
    if (!plat_open_url(url))
      toast(T("Could not open the link"), 2000, NULL);
    return;
  } /* a phone: its own browser */
  char nm[120];
  snprintf(nm, sizeof nm, "%.*s", (int)strcspn(name ? name : "", "\n"), name ? name : "");
  QrCardOpts o;
  memset(&o, 0, sizeof o);
  o.title = T("Open on another device");
  o.sub = nm[0] ? nm : NULL;
  o.url = url;
  qrcard_show(&o);
}
void app_play(const Stream *s, const char *type, const char *id, const char *vid, int fromStart);
void app_open_item(const Item *it) {
  if (it->cw) {
    Stream st;
    if (sb_resume_stream(it->id, it->videoId, &st)) {
      app_play(&st, it->type, it->id, it->videoId, 0);
      return;
    }
  }
  DetailParams p;
  memset(&p, 0, sizeof p);
  p.it = *it;
  app_go(SCR_DETAIL, &p, 0);
}
/* long-press OK on any card opens its menu; a short tap does its normal action */
#define HOLD_MS 550
static struct {
  int down, menu;
  Item item;
  double at;
  int has;
} okHold;
static int menuGuard;
float app_hold_progress(void) {
  if (!okHold.down || okHold.menu)
    return 0;
  float x = (float)((NOW - okHold.at) * 1000 - 220) / (HOLD_MS - 220);
  return x < 0 ? 0 : x > 1 ? 1 : x;
}
static void ok_up(void) {
  menuGuard = 0;
  int wasDown = okHold.down, menu = okHold.menu, has = okHold.has;
  Item it = okHold.item;
  memset(&okHold, 0, sizeof okHold);
  if (wasDown && !menu && has) { /* short press = the card's normal action */
    if (screens[SCR] && screens[SCR]->open)
      screens[SCR]->open(&it);
    else
      app_open_item(&it);
  }
}
static void key_core(int k, int down, int repeat);
/* ---- the Magic Remote pointer. How it behaves (timings as measured on LG TVs):
   pointing moves the same highlight the arrows move and the screen follows as for the arrows; the brake: while
   the thing under the pointer is still moving the pointer must stand still 150 ms first. A click is OK pressed
   and released (held on a card: its menu); a few targets act on release (a tab, a chip, the seek bar, beside a
   sheet = close it); a sheet's body absorbs a click. A click and an OK key within 150 ms are one press. The
   wheel is Up/Down, one step per notch (70 ms apart at most). An arrow key puts the pointer away; it comes
   back only with a real movement: nothing in the first 350 ms, then 32 px from where the hand started again. */
#define PTR_SETTLE 0.15
#define PTR_DEDUPE 0.15
#define PTR_WHEEL 0.07
#define PTR_ARROW_WIN 0.35
#define PTR_ARROW_DIST 32
#define PTR_ARROW_PAUSE 0.3
static struct {
  int on;
  float x, y;
  Hit hv;
  int hasHv;
  float hx, hy;
  double movedAt;
  int down, downAct;
  Hit act;
  double okKeyAt, clickAt, wheelAt;
  int eatUp, eatOk, arrowHid;
  double arrowAt;
  int anchor;
  float ax, ay;
  double tremAt;
  int dirty, hold;
  float wacc, waccX;
  Hit over;
  int hasOver;
} PTR;
/* a desktop window: the focus the pointer gave by hovering holds the page still (the screens keep their scroll where it
   was) until a key, the wheel or a click moves it; on a TV the page follows the pointer as it follows the arrows */
int app_scroll_hold(void) {
#if SB_WINDOWED
  return PTR.hold;
#else
  return 0;
#endif
}
int app_ptr_hold(void) {
  return PTR.hold;
} /* the focus came from hovering, on any screen with a pointer (a TV's Magic Remote too) */
int app_ptr_pressing(void) {
  return PTR.down == 1;
} /* the OK being handled is the pointer's button */
int app_ptr_over(float x, float y, float w, float h) {
  return PTR.on && PTR.x >= x && PTR.x < x + w && PTR.y >= y && PTR.y < y + h;
}
int app_ptr_hovers(void (*focus)(int, int), int a) {
  return PTR.hasOver && PTR.over.focus == focus && PTR.over.a == a;
} /* (last frame's rectangles) */
static void ptr_show(void) {
  int was = PTR.arrowHid;
  PTR.arrowHid = 0;
  if (was)
    plat_cursor_visible(1);
  if (PTR.on)
    return;
  PTR.on = 1;
  PTR.hasHv = 0;
}
static void ptr_hide(void) {
  if (!PTR.on)
    return;
  PTR.on = 0;
  PTR.hasHv = 0;
}
static int ptr_tremor(double now) {
  if (!PTR.arrowHid)
    return 0;
#if SB_WINDOWED
  (void)now;
  return 0; /* a desktop's mouse does not tremble in a hand: any move brings it back */
#endif
  if (now - PTR.arrowAt < PTR_ARROW_WIN)
    return 1;
  if (!PTR.anchor || now - PTR.tremAt > PTR_ARROW_PAUSE) {
    PTR.anchor = 1;
    PTR.ax = PTR.x;
    PTR.ay = PTR.y;
  }
  PTR.tremAt = now;
  float dx = PTR.x - PTR.ax, dy = PTR.y - PTR.ay;
  return dx * dx + dy * dy < PTR_ARROW_DIST * PTR_ARROW_DIST;
}
static void ptr_nav_back(void) {
  if (topNav.want >= 0)
    return;
  int c = tab_of(tab_scr());
  if (c >= NAV_HOME && c <= NAV_LIB && topNav.idx != c) {
    topNav.idx = c;
    topnav_paint();
  }
}
static void ptr_keep(const Hit *h) {
  PTR.hv = *h;
  PTR.hasHv = 1;
  PTR.hx = h->x + h->w / 2;
  PTR.hy = h->y + h->h / 2;
}
/* the brake: the element the pointer last focused moved (a slide, a scroll) or went away -> note when */
static void ptr_settle(double now) {
  if (!PTR.hasHv)
    return;
  const Hit *h = hit_at(PTR.hx, PTR.hy);
  int found = 0;
  float cx = PTR.hx, cy = PTR.hy;
  /* the element is looked up by identity among this frame's rectangles */
  extern const Hit *hit_find(const Hit *like);
  const Hit *e = hit_find(&PTR.hv);
  (void)h;
  if (e) {
    found = 1;
    cx = e->x + e->w / 2;
    cy = e->y + e->h / 2;
  }
  if (!found) {
    PTR.movedAt = now;
    PTR.hasHv = 0;
    return;
  }
  if (fabsf(cx - PTR.hx) > 3 || fabsf(cy - PTR.hy) > 3)
    PTR.movedAt = now;
  PTR.hx = cx;
  PTR.hy = cy;
}
static void ptr_frame(void) {
  if (!PTR.on || !PTR.dirty)
    return;
  PTR.dirty = 0;
  double now = NOW;
  if (player_open()) {
    extern void player_ptr(int type, float x, float y);
    player_ptr(0, PTR.x, PTR.y);
    return;
  }
  if (PTR.down)
    return; /* pressed: a hand's tremble must not move the press onto the next card */
  ptr_settle(now);
  const Hit *d = hit_at(PTR.x, PTR.y);
  if (topNav.active && !(d && d->nav))
    ptr_nav_back();
  if (!d || !d->focus || (PTR.hasHv && hit_same(d, &PTR.hv)))
    return;
  if (PTR.movedAt && now - PTR.movedAt < PTR_SETTLE) {
    PTR.dirty = 1;
    return;
  } /* the page is still moving under the pointer */
  if (d->peek)
    return; /* only peeking: a click brings it in */
  PTR.hold = 1; /* (a TV's rows hold still under the pointer too: app_ptr_hold) */
#if SB_WINDOWED
  curKey = K_RIGHT;
  idle_touch();
  curKey = 0; /* a new element under the pointer: as an arrow's step for the trailers and the card's clock, not every
                 twitch of the mouse */
#endif
  d->focus(d->a, d->b);
  ptr_keep(d);
}
/* a desktop window: the fullscreen button in the top-right corner, the bar's glass circle (hover lights it, a click
   switches, F11 / Alt+Enter / a double click on a film do the same). Its right edge at right, centred on cy; the room
   it takes is returned, for what sits left of it (0 where there is no window) */
#if SB_WINDOWED
static void fs_act(int a, int b) {
  (void)a;
  (void)b;
  plat_fullscreen_toggle();
}
static void fs_none(int a, int b) {
  (void)a;
  (void)b;
}
#endif
float app_fs_button(float right, float cy) {
#if SB_WINDOWED
  const float D = 60; /* as the bar's items: nothing at rest, a grey disc under the pointer */
  float x = right - D, y = cy - D / 2;
  int over = PTR.on && PTR.x >= x && PTR.x < x + D && PTR.y >= y && PTR.y < y + D;
  static Tween lit;
  tween_to(&lit, over ? 1 : 0, 0.18f, EASE_CSS_EASE);
  tween_tick(&lit, DT);
  hit_add(x, y, D, D, fs_none, fs_act, 0, 0);
  if (lit.v > 0.001f)
    gfx_rrect(x, y, D, D, D / 2, WHITE_A((int)(46 * lit.v)));
  icon_draw(plat_fullscreen_on() ? "nav_unfull" : "nav_full", x + 14, y + 14, 32,
            col_mix(WHITE_A(204), WHITE_A(255), lit.v));
  return D + 16;
#else
  (void)right;
  (void)cy;
  return 0;
#endif
}
void app_pointer_move(float x, float y) {
  if (x == PTR.x && y == PTR.y)
    return;
  PTR.x = x;
  PTR.y = y;
  if (ptr_tremor(NOW))
    return;
  ptr_show();
#if SB_WINDOWED
  lastIn = NOW; /* awake (the frame rate, the screensaver), but the card under it keeps opening: only a new element
                   counts as a move (ptr_frame) */
#else
  idle_touch_by("pointer moved");
#endif
  PTR.dirty = 1;
}
void app_pointer_button(int down, float x, float y) {
  double now = NOW;
  PTR.x = x;
  PTR.y = y;
  if (saver_on())
    return; /* the screensaver covers the page: nothing under it is pressed */
  if (down) {
    ptr_show();
    idle_touch();
    if (PTR.okKeyAt && now - PTR.okKeyAt < PTR_DEDUPE) {
      PTR.eatUp = 1;
      return;
    } /* the OK key of this same press came first */
    PTR.clickAt = now;
#if SB_WINDOWED
    if (player_open()) {
      const Hit *f = hit_at(x, y);
      if (f && f->act == fs_act) {
        PTR.down = 3;
        PTR.act = *f;
        return;
      }
    } /* over a film: its corner's fullscreen button is the window's, not the player's */
#endif
    if (player_open()) {
      extern void player_ptr(int type, float x, float y);
      PTR.down = 2;
      player_ptr(1, x, y);
      return;
    }
    const Hit *d = hit_at(x, y);
    if (!d)
      return;
    if (d->act) {
      PTR.down = 3;
      PTR.act = *d;
      return;
    }
    if (d->shield || !d->focus)
      return;
    PTR.hold = 0;
    if (d->peek) {
      d->focus(d->a, d->b);
      ptr_keep(d);
      PTR.down = 4;
      return;
    } /* peeking: the click brings it into view, the next one opens it */
    d->focus(d->a, d->b);
    ptr_keep(d);
    PTR.down = 1;
    key_core(K_OK, 1, 0); /* OK pressed (held: the card's menu) */
    return;
  }
  if (PTR.eatUp) {
    PTR.eatUp = 0;
    return;
  }
  int dn = PTR.down;
  PTR.down = 0;
  if (!dn)
    return;
  if (dn == 2) {
    extern void player_ptr(int type, float x, float y);
    player_ptr(2, x, y);
    return;
  }
  if (dn == 1) {
    key_core(K_OK, 0, 0);
    return;
  }
  if (dn == 4)
    return;
  const Hit *d = hit_at(x, y);
  if (d && hit_same(d, &PTR.act))
    PTR.act.act(PTR.act.a, PTR.act.b); /* released on it: its own action */
}
/* the wheel = the Up/Down arrows (Left/Right for a sideways wheel), one per notch; it leaves the pointer out */
static void wheel_step(int k) {
  idle_touch();
  key_core(app_mirror_key(k), 1, 0);
  key_core(k, 0, 0);
}
void app_pointer_wheel(float dx, float dy) {
  double now = NOW;
  if (saver_on())
    return;
#if SB_WINDOWED
  /* a desktop: a wheel's notch is 1, a touchpad sends small parts of one: they add up, a step for each whole one
     (at most 3 from one event); a pause or a turn the other way starts again. Over a film it is the volume */
  if (!dx && !dy)
    return;
  lastIn = now;
  if (player_open()) {
    extern void player_wheel(float dy);
    player_wheel(dy);
    return;
  }
  if (now - PTR.wheelAt > 0.4)
    PTR.wacc = PTR.waccX = 0;
  PTR.wheelAt = now;
  if ((dy > 0) != (PTR.wacc > 0) && PTR.wacc)
    PTR.wacc = 0;
  if ((dx > 0) != (PTR.waccX > 0) && PTR.waccX)
    PTR.waccX = 0;
  PTR.wacc += dy;
  PTR.waccX += dx;
  for (int i = 0; i < 3 && fabsf(PTR.wacc) >= 0.999f; i++) {
    int up = PTR.wacc > 0;
    PTR.wacc += up ? -1 : 1;
    wheel_step(up ? K_UP : K_DOWN);
  }
  for (int i = 0; i < 3 && fabsf(PTR.waccX) >= 0.999f; i++) {
    int rt = PTR.waccX > 0;
    PTR.waccX += rt ? -1 : 1;
    wheel_step(rt ? K_RIGHT : K_LEFT);
  }
  if (fabsf(PTR.wacc) >= 1)
    PTR.wacc = 0;
  if (fabsf(PTR.waccX) >= 1)
    PTR.waccX = 0;
#else
  if (now - PTR.wheelAt < PTR_WHEEL || (!dx && !dy))
    return;
  PTR.wheelAt = now;
  wheel_step(dy > 0 ? K_UP : dy < 0 ? K_DOWN : dx > 0 ? K_RIGHT : K_LEFT);
#endif
}
/* a desktop's right button: the menu of the card under it (as holding OK on it), or Back where there is none */
int app_pointer_menu(float x, float y) {
  if (saver_on() || player_open())
    return 0;
  PTR.x = x;
  PTR.y = y;
  Screen *sc = app_overlay ? app_overlay : screens[SCR];
  const Hit *d = hit_at(x, y);
  if (!d || d->shield || !d->focus || d->nav || !sc || topNav.active)
    return 0;
  PTR.hold = 0;
  d->focus(d->a, d->b);
  ptr_keep(d);
  idle_touch();
  if (sc->menu && sc->menu()) {
    sound_play(SND_HOLD);
    return 1;
  }
  if (app_overlay)
    return 1; /* a sheet's row with nothing held: nothing (Back would close the sheet) */
  const Item *fi = sc->focused ? sc->focused() : NULL;
  if (!fi || !app_card_menu_hook)
    return 0;
  orbs_pulse(ORB_HOLD, 1);
  sound_play(SND_HOLD);
  app_card_menu_hook(fi);
  return 1;
}
/* the keyboard's menu key (or Shift+F10): the focused card's menu */
void app_menu_key(void) {
  Screen *sc = app_overlay ? app_overlay : screens[SCR];
  if (saver_on() || player_open() || topNav.active || !sc)
    return;
  idle_touch();
  if (sc->menu && sc->menu()) {
    sound_play(SND_HOLD);
    return;
  }
  if (app_overlay || !sc->focused)
    return;
  const Item *fi = sc->focused();
  if (fi && app_card_menu_hook) {
    orbs_pulse(ORB_HOLD, 1);
    app_card_menu_hook(fi);
  }
}
void app_pointer_system(int show) {
  if (!show) {
    ptr_hide();
    return;
  }
  if (PTR.arrowHid) {
    if (NOW - PTR.arrowAt < PTR_ARROW_WIN)
      plat_cursor_visible(0);
    return;
  }
  ptr_show();
} /* (the TV showing it again right after an arrow: only the remote stirring) */
static int text_taker(void) {
  return (app_overlay && app_overlay->text) || (!app_overlay && screens[SCR] && screens[SCR]->text);
}
void app_key(int k, int down, int repeat) {
  if (k == K_ERASE) {
    if (text_taker() && !player_open()) {
      if (down)
        app_text("\b");
      return;
    }
    k = K_BACK;
  } /* Backspace: deletes where text is typed */
  if (k == K_CURSOR_SHOW || k == K_CURSOR_HIDE) {
    if (down)
      app_pointer_system(k == K_CURSOR_SHOW);
    return;
  } /* a notice from the TV, not a key */
  if (down && k == K_OK && !repeat) {
    if (PTR.eatOk || PTR.down || (PTR.clickAt && NOW - PTR.clickAt < PTR_DEDUPE)) {
      PTR.eatOk = 1;
      return;
    }
    PTR.okKeyAt = NOW;
  } /* the click of this same press came first */
  if (!down && k == K_OK && PTR.eatOk) {
    PTR.eatOk = 0;
    return;
  }
  if (down && saver_on() && k != K_CURSOR_SHOW && k != K_CURSOR_HIDE) {
    saver_stop();
    idle_touch();
    if (k == K_OK)
      PTR.eatOk = 1;
    return;
  } /* the key that wakes the screensaver does nothing else */
  if (down && k >= K_LEFT && k <= K_DOWN) {
    ptr_hide();
    PTR.arrowHid = 1;
    PTR.arrowAt = NOW;
    PTR.anchor = 0;
    plat_cursor_visible(0);
  } /* every arrow re-arms it, hidden or not; ptr_hide alone is the app's hover: the arrow on screen is the TV's */
  key_core(k, down, repeat);
}
/* where the focus is, as far as can be told from outside: the screen, the open sheet, the top bar, the centre of
   the focused element (the same point the page transitions grow from) */
typedef struct {
  int scr, nav, navIdx, known;
  const void *ov;
  float x, y;
} FocusSig;
static void focus_sig(FocusSig *f) {
  memset(f, 0, sizeof *f);
  f->scr = SCR;
  f->ov = app_overlay;
  f->nav = topNav.active;
  f->navIdx = topNav.idx;
  Screen *s = app_overlay ? app_overlay : screens[SCR];
  if (f->nav)
    f->known = 1;
  else if (s && s->origin)
    f->known = s->origin(&f->x, &f->y) || !app_overlay;
}
static void key_dispatch(int k, int repeat);
static int keyHeld; /* the key being handled is a repeat of a held one */
int app_key_held(void) {
  return keyHeld;
}
static int okDown; /* OK is down (a key or the pointer's button) */
int app_ok_down(void) {
  return okDown;
}
float app_hold_k(double at) {
  float x = (float)((NOW - at) * 1000 - 220) / (HOLD_MS - 220);
  return x < 0 ? 0 : x > 1 ? 1 : x;
}
static void key_core(int k, int down, int repeat) {
  if (down) {
    keyHeld = repeat != 0;
    if (k != K_CURSOR_SHOW && k != K_CURSOR_HIDE)
      PTR.hold = 0;
  } /* a key moves the focus: the page follows it again */
  if (k == K_OK)
    okDown = down;
  if (player_open()) {
    if (!(k == K_OK && repeat && down))
      player_key(k, down);
    return;
  }
  if (!down) {
    if (k == K_OK)
      ok_up();
    return;
  }
  /* the pointer appearing or hiding is the TV's own doing (it hides by itself, and shows when the remote is
     picked up): not input that stops the background trailer */
  if (k != K_CURSOR_SHOW && k != K_CURSOR_HIDE) {
    char why[32];
    snprintf(why, sizeof why, "key %d", k);
    curKey = k;
    idle_touch_by(why);
  }
  k = app_mirror_key(k); /* mirrored layout: Left moves forward (every screen sees the logical direction) */
  if (k >= K_LEFT &&
      k <= K_DOWN) { /* an arrow: a tap when the focus moved (a screen's own sound, e.g. a filter, wins) */
    FocusSig f0, f1;
    focus_sig(&f0);
    double t0 = NOW;
    key_dispatch(k, repeat);
    focus_sig(&f1);
    /* the glows lean the way it went: a tab sweeps them across, a step only leans them (held keys, less) */
    if (f1.nav && f1.navIdx != f0.navIdx)
      orbs_pulse(f1.navIdx > f0.navIdx ? ORB_TAB_R : ORB_TAB_L, 0.8f);
    else
      orbs_pulse(k == K_LEFT    ? ORB_LEFT
                 : k == K_RIGHT ? ORB_RIGHT
                 : k == K_UP    ? ORB_UP
                                : ORB_DOWN,
                 repeat                        ? 0.15f
                 : memcmp(&f0, &f1, sizeof f0) ? 0.35f
                                               : 0.2f);
    if (sound_last() < t0 - 1e-6 && memcmp(&f0, &f1, sizeof f0))
      sound_play(f1.nav && f1.navIdx != f0.navIdx ? SND_TAB : SND_FOCUS);
    else if (sound_last() < t0 - 1e-6 && !f1.known)
      sound_play(SND_FOCUS); /* a sheet that cannot say where its focus is: every move */
    return;
  }
  key_dispatch(k, repeat);
}
/* the remote's colour keys: red Settings (or the notice out), green Search; the profile panel hands them on too */
void app_color_key(int k) {
  if (k == K_RED && update_notice_key()) {
    sound_play(SND_TAB);
    return;
  } /* a notice out: red takes you to what it is about */
  int t = k == K_RED ? NAV_SET : NAV_SEARCH;
  sound_play(SND_TAB);
  if (SCR == chipScr(t) && colorFromSet) {
    color_back(t); /* pressed on its own page: back where it was opened from */
    return;
  }
  topnav_flush(1);
  if (chipScr(t) != SCR) {
    topNav.idx = -1;
    topnav_switch(t);
  }
  topNav.idx = t;
  topnav_enter_content();
  return;
}
/* Settings, from the profile panel (it has no item in the bar): out of the picture, Back returns to this page */
void app_open_settings(void) {
  if (SCR == SCR_SET)
    return;
  topnav_flush(1);
  topNav.idx = -1;
  topnav_switch(NAV_SET);
  topNav.idx = NAV_SET;
  topnav_enter_content();
}
static void key_dispatch(int k, int repeat) {
  if (k == K_OK && repeat) {
    return;
  } /* repeats of a held OK: the hold timer decides */
  if (k == K_OK || k == K_BACK)
    sound_play(SND_FOCUS); /* a press: the same tap as a move */
  if (k == K_OK)
    orbs_pulse(ORB_OK, 0.6f);
  else if (k == K_BACK)
    orbs_pulse(ORB_BACK, 0.6f);
  else if (k == K_RED || k == K_GREEN) {
    int back = SCR == (k == K_RED ? SCR_SET : SCR_SEARCH) && colorFromSet;
    orbs_pulse(back ? ORB_BACK : k == K_RED ? ORB_RED : ORB_GREEN, back ? 0.8f : 1);
  } /* (its own page: they breathe out as it leaves) */
  if (app_overlay) {
    if (k == K_OK && menuGuard)
      return;
    app_overlay->key(k);
    return;
  }
  if (k == K_BACK) {
    app_back();
    return;
  }
  if (k == K_EXIT) {
    app_quit();
    return;
  }
  if ((k == K_RED || k == K_GREEN) && SCR != SCR_LOGIN) {
    app_color_key(k);
    return;
  } /* the remote's colour keys: red Settings, green Search */
  if (topNav.active) {
    topnav_key(k);
    return;
  }
  if (k == K_OK && screens[SCR] && screens[SCR]->focused) {
    const Item *fi = screens[SCR]->focused();
    if (fi) {
      if (!okHold.down) {
        okHold.down = 1;
        okHold.menu = 0;
        okHold.item = *fi;
        okHold.has = 1;
        okHold.at = NOW;
      }
      return;
    }
  }
  if (screens[SCR] && screens[SCR]->key)
    screens[SCR]->key(k);
}
void app_text(const char *utf8) {
  idle_touch();
  if (app_overlay && app_overlay->text)
    app_overlay->text(utf8);
  else if (screens[SCR] && screens[SCR]->text)
    screens[SCR]->text(utf8);
  /* a keyboard typing on one of the tabs: Search opens with it (a letter or digit; Space stays play/pause) */
  else if (!app_overlay && app_is_board(SCR) && !player_open() && app_splash_done && (unsigned char)utf8[0] > ' ') {
    extern void search_start_typing(const char *utf8);
    app_key(K_GREEN, 1, 0);
    app_key(K_GREEN, 0, 0);
    search_start_typing(utf8);
  }
}
void app_refresh_boards(void) {
  extern void board_invalidate(void);
  extern void board_refresh_quiet(void);
  board_invalidate();
  if (app_is_board(SCR))
    board_refresh_quiet();
} /* in place: no row replays its entrance */
void app_init(void) {
  vig_tex(0);
  vig_tex(1); /* both shadings made at start: the first title page would make its own mid-zoom */
  topNav.want = -1;
  NOW = SDL_GetTicks() / 1000.0;
  lastIn = NOW;
  lastNav = NOW;
  bgMode = 0;
  tween_set(&toastA, 0);
  accent_apply();
  accent_tick(0);
  sync_init();
  btr_init();
  castlink_start();
}
static void amb_bake(void);
void app_tick(float dt) {
  DT = dt;
  NOW = SDL_GetTicks() / 1000.0;
  FRAME++;
  ptr_frame();
  touch_tick();
  {
    static int swelled;
    if (app_hold_progress() > 0 && !swelled) {
      swelled = 1;
      sound_play(SND_HOLD);
    } else if (!okHold.down)
      swelled = 0;
  } /* holding OK: a low swell as the line starts round */
  if (okHold.down && !okHold.menu && (NOW - okHold.at) * 1000 >= HOLD_MS) {
    okHold.menu = 1;
    menuGuard = 1;
    orbs_pulse(ORB_HOLD, 1);
    if (app_card_menu_hook)
      app_card_menu_hook(&okHold.item);
  }
  timer_tick(&xin, dt);
  timer_tick(&xmvT, dt);
  timer_tick(&xoutT, dt);
  saver_tick(dt);
  accent_tick(dt);
  tint_tick();
  toast_tick(dt);
  clock_tick();
  if (SCR == SCR_LOGIN || splash_signed_in())
    login_glow_bake(); /* (the sign-in's glass: the glows under it) */
  amb_bake();
  topnav_tick(dt);
  bg_tick(dt);
  sync_tick();
  recs_tick();
  update_tick(dt);
  best_tick();
  curKey = 0; /* (a page switch later in the frame is not a key's) */
  {
    extern void board_col_tick(void);
    board_col_tick();
  }
  btr_tick(dt);
  welcome_tick();
  lift_tick(dt);
  tabs_warm_tick();
  {
    static int greeted;
    if (!greeted && app_splash_done) {
      greeted = 1;
      sound_play(SND_START);
    }
  } /* start-up: one very faint sound */
  {
    static int wasP;
    int po = player_open();
    if (po != wasP) {
      wasP = po;
      if (po)
        sound_play(SND_PLAY);
      sound_quiet(po);
      if (form_touch()) {
        plat_orientation(po);
        plat_immersive(po);
        gfx_film_layout(po && !form_tablet() ? 1600 : 0);
        gfx_touch_layout(po ? 0 : form_density());
      }
    }
  } /* into the player: one faint sound, then the film has the audio alone; a phone turns sideways for it, its bars go,
       and the canvas is the TV's (back upright after) */
  castlink_tick(dt); /* (a phone drives the film too: before the app rests under it) */
  player_tick(dt);
  if (player_open() && player_cover() >= 0.999f)
    return; /* the app rests while the film plays (not while the player fades over it or away from it) */
  if (screens[SCR] && screens[SCR]->tick)
    screens[SCR]->tick(dt);
  /* every overlay that has shown keeps ticking after it lets go (its closing motion plays out, and its state settles:
     frozen half-closed, the next opening was taken for a reopening mid-close and skipped its entrance) */
  if (app_overlay) {
    int k = 0;
    for (; k < nseenOv; k++)
      if (seenOv[k] == app_overlay)
        break;
    if (k == nseenOv && nseenOv < 8)
      seenOv[nseenOv++] = app_overlay;
  }
  for (int k = 0; k < nseenOv; k++)
    if (seenOv[k] != app_overlay && seenOv[k]->tick)
      seenOv[k]->tick(dt);
  if (app_overlay && app_overlay->tick)
    app_overlay->tick(dt);
}
/* (3.0.0: the sign-in has no picture behind it any more; it sits on Home's flat page colour like the rest) */
static void glass_draw(void) {
}
/* Settings' and Search's background, and the guide's: the two glows (ui/orbs.c), on their springs */
/* the guide over Home (welcome.c) takes the glows for its page (as Settings has them) and draws them over its own
   base: 1 while it is up, 2 while it fades out (they go with it) */
static int ambGuide;
static Tween ambA;
static int ambIn;
void app_ambient_guide(int mode) {
  ambGuide = mode;
}
/* the phone's words landed: the glows surge in towards the middle and spring back */
void app_glow_kick(void) {
  orbs_pulse(ORB_PHONE, 1);
}
static void ambient_draw(void) {
  int srch = SCR == SCR_SEARCH && !ambGuide, want = 0; /* 3.0.0: Search, Settings and the guide all sit on the plain
                                                           page (no drifting glows) */
  Orbs *o = orbs_app();
  if (want && !ambIn) {
    ambIn = 1;
    if (ambA.v < 0.02f)
      orbs_place(o, srch, 4);
  } /* arriving: out of the middle, wide, to their corners */
  if (!want)
    ambIn = 0;
  tween_to(&ambA, want ? 1 : 0, want ? 0.45f : 0.42f, EASE_CSS_EASE);
  tween_tick(&ambA, DT);
  if (ambA.v <= 0.001f)
    return;
  /* coming in or going: quick and plain to see; between the two pages: a slow glide; going, they rush in towards the
     middle and spread as they fade, melting into the next page's colours */
  orbs_step(o, (float)DT, srch, want ? 1 : 5, want ? (ambA.v < 0.98f ? 7.5f : 2.6f) : 8.0f);
  orbs_draw(o, ambA.v);
}
/* the glows, small, for the glass over the pages that have them (Settings, Search, the guide): made in the tick, they
 * move */
static GfxTarget ambT;
static int ambTOk = -1, ambTFresh;
static int amb_glass_on(void) {
  return welcome_is_open() || ((SCR == SCR_SET || SCR == SCR_SEARCH) && ambA.v > 0.001f);
}
static void amb_bake(void) {
  {
    static int no = -1;
    if (no < 0)
      no = getenv("SB_NOAMB") != NULL;
    if (no) {
      ambTFresh = 0;
      return;
    }
  } /* (development: measuring) */
  if (!amb_glass_on()) {
    ambTFresh = 0;
    return;
  }
  int w, h;
  gfx_screen_target_size(384, 216, &w, &h);
  if (ambT.fbo && (ambT.w != w || ambT.h != h)) {
    gfx_target_free(&ambT);
    ambTOk = -1;
    ambTFresh = 0;
  }
  if (ambTOk < 0)
    ambTOk = gfx_target_create(&ambT, w, h);
  if (ambTFresh && FRAME % 4)
    return; /* the glows drift slowly: made again every fourth frame (each bake is a switch of the GPU's target, costly
               on these chips) */
  if (!ambTOk)
    return;
  gfx_target_begin(&ambT, PAGE | 0xff000000u);
  if (welcome_is_open())
    login_backdrop_draw();
  else
    orbs_draw(orbs_app(), ambA.v);
  gfx_target_end(); /* (the guide has the sign-in's glows) */
  ambTFresh = 1;
}
unsigned int app_ambient_glass_tex(void) {
  return ambTOk > 0 && ambTFresh ? ambT.tex : 0;
}
void app_ambient_draw(void) {
  if (ambGuide)
    ambient_draw();
} /* (the guide's, once a frame) */
static void page_draw(void) {
  if (welcome_covers() && app_overlay && app_overlay->draw) {
    app_overlay->draw();
    return;
  } /* the guide's page hides all of it */
  static const char *skip = (const char *)1;
  if (skip == (const char *)1)
    skip = getenv("SB_SKIP"); /* development: SB_SKIP=bg,bar,screen,amb leaves parts out, to time what each costs */
  int ph = form_touch() && phone_owns(SCR); /* a phone's own page: no TV backdrop, glows or top bar */
  if (!ph) {
    if (!skip || !strstr(skip, "bg"))
      bg_draw();
    glass_draw();
    if (!ambGuide && (!skip || !strstr(skip, "amb")))
      ambient_draw();
  }
  if (screens[SCR] && screens[SCR]->draw && (!skip || !strstr(skip, "screen"))) {
    float a = app_screen_alpha();
    if (xoutT.on)
      a *= ease_apply(
          EASE_CSS_EASEIN,
          timer_x(&xoutT)); /* under a page leaving by Back: it comes up as that one goes, never both at once */
    if (a < 1)
      gfx_opacity_push(a);
    int mv = xmvT.on && xmv.kind &&
             !sx_flying(); /* the hand-over's move: the incoming screen as a whole, one transform (not while a logo
                              flies across: its landing spot must hold still, or it dips and comes back) */
    if (mv) {
      float k = ease_apply(EASE_CSS_EASEOUT, timer_x(&xmvT));
      if (xmv.kind == NAV_SLIDE)
        gfx_offset_push(90 * xmv.dx * (1 - k), 0);
      else {
        float s0 = xmv.kind == NAV_ZOOM ? 0.9f : 1.07f, sc = s0 + (1 - s0) * k;
        gfx_scale_push(sc, sc, xmv.x, xmv.y);
      }
    }
    screens[SCR]->draw();
    if (mv) {
      if (xmv.kind == NAV_SLIDE)
        gfx_offset_pop();
      else
        gfx_scale_pop();
    }
    if (a < 1)
      gfx_opacity_pop();
  }
  if (xoutT.on && screens[xoutScr] && screens[xoutScr]->out) {
    int hn = hit_count();
    sx_ghost(1);
    screens[xoutScr]->out(timer_x(&xoutT));
    sx_ghost(0);
    hit_trim(hn);
  } /* seen leaving, never pointed at */
  if (form_touch())
    phone_bars_draw();
  else if (!skip || !strstr(skip, "bar"))
    topnav_draw();
  for (int k = 0; k < nseenOv; k++)
    if (seenOv[k] != app_overlay && seenOv[k]->draw)
      seenOv[k]->draw(); /* closing ones (they draw nothing once gone) */
  if (app_overlay && app_overlay->draw)
    app_overlay->draw();
}
/* the page as it shows under the sheets and cards (its backdrop, the screen, the top bar), for their glass: drawn once
   into the glass's own small picture; what it adds for the pointer is dropped */
void app_draw_under(void) {
  int hn = hit_count();
  sx_ghost(1);
  int occWas = occOn;
  occOn = 0; /* (the glass sees the whole page) */
  bg_draw();
  glass_draw();
  if (!ambGuide)
    ambient_draw();
  if (screens[SCR] && screens[SCR]->draw)
    screens[SCR]->draw();
  topnav_draw();
  sx_ghost(0);
  hit_trim(hn);
  occOn = occWas;
}
void app_draw(void) {
  occOn = occNOn;
  occX = occNX;
  occY = occNY;
  occW = occNW;
  occH = occNH;
  occNOn = 0; /* (last frame's still panel) */
  {
    const Hit *o = PTR.on && !player_open() ? hit_at(PTR.x, PTR.y) : NULL;
    PTR.hasOver = o != NULL;
    if (o)
      PTR.over = *o;
  } /* what is under the pointer, before this frame's rectangles replace last frame's */
  hit_begin();
  touch_begin_frame();
  sx_frame_begin();
  sx_layer(0);
  /* the player fades in over the page and, leaving, fades off it onto the page: the page is drawn under it only
     while it does not cover it all (the video plane itself never fades: the art and the controls above it do) */
  if (player_open() && saver_covers()) {
    saver_draw();
    return;
  } /* (over a paused film: the screensaver alone) */
  if (player_open()) {
    if (player_page_under())
      page_draw();
    sx_layer(1);
    player_draw();
    sx_layer(0);
    sx_flush();
    toast_draw();
    saver_draw();
    return;
  }
  if (saver_covers()) {
    saver_draw();
    return;
  } /* nothing under it is seen: not drawn */
  page_draw();
  sx_flush(); /* a logo or poster on its way from the last page */
  toast_draw();
  saver_draw();
}
