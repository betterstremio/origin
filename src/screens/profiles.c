/* "Who's watching?" (see profiles.h). At launch it sits over a Home already built for the last profile, so choosing
   that one again costs one press and nothing else; another profile comes back from this device's kept copy. */
#include "screens/profiles.h"
#include "screens/login.h"
#include "screens/board.h"
#include "core/app.h"
#include "core/keys.h"
#include "core/hit.h"
#include "core/i18n.h"
#include "core/nuvio.h"
#include "core/sync.h"
#include "core/stremio.h"
#include "core/store.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/widgets.h"
#include "ui/osk.h"
#include <strings.h>
#include <stdio.h>
#include <string.h>
#include "platform/plat.h"

#define MAXP 8
static struct { int open, was, mode, idx, pinMode, addMode, pad, busy; char addName[24]; Osk osk; double at, outAt; char pin[12], err[96]; NvProfile pinFor; float foc[MAXP]; } P;
static Screen overlay;

int profiles_is_open(void) { return P.open; }
static void close_now(void) { if (!P.open) return; P.open = 0; P.outAt = app_now(); if (app_overlay == &overlay) app_overlay = NULL; }
static void leave_for(int index) {
  int mode = P.mode; close_now();
  if (mode == PF_SIGNIN) { login_profile_go(index); return; }
  if (index == nv_profile_current()) return; /* the one already behind */
  if (!nv_profile_kept(index)) { static char arg[32]; snprintf(arg, sizeof arg, "load:%d", index); board_invalidate(); app_go(SCR_LOGIN, arg, 1); return; } /* first time on this device: the loading screen while it comes down */
  nv_profile_select(index); board_invalidate(); topNav.idx = NAV_HOME; if (SCR == SCR_HOME) board_rebuild(); else app_go(SCR_HOME, NULL, 1);
  sync_after_switch();
}
static void on_pin(int ok, const char *err, void *ud) { (void)ud; P.busy = 0; if (!P.open || !P.pinMode) return;
  if (ok) { leave_for(P.pinFor.index); return; } P.pin[0] = 0; snprintf(P.err, sizeof P.err, "%s", T(err ? err : "Wrong PIN")); }
static int can_add(void) { return nv_profile_count() < 6; } /* (Nuvio's limit) */
static int tile_count(void) { int n = nv_profile_count(); if (n > MAXP) n = MAXP; return n + can_add(); }
static void on_added(int ok, const char *err, void *ud) { (void)ud; P.busy = 0; if (!P.open || !P.addMode) return;
  if (!ok) { snprintf(P.err, sizeof P.err, "%s", T(err ? err : "Couldn\xe2\x80\x99t add the profile")); return; }
  P.addMode = 0; P.err[0] = 0; osk_close(&P.osk); NvProfile p; for (int i = 0; i < nv_profile_count() && i < MAXP; i++) if (nv_profile_get(i, &p) && !strcasecmp(p.name, P.addName)) P.idx = i; }
static void add_open(void) { P.addMode = 1; P.addName[0] = 0; P.err[0] = 0; osk_open(&P.osk); P.osk.shift = 1; }
static void add_submit(void) { size_t n = strlen(P.addName); while (n && P.addName[n - 1] == ' ') P.addName[--n] = 0;
  if (!n) { snprintf(P.err, sizeof P.err, "%s", T("Enter a name")); return; } P.busy = 1; nv_profile_add(P.addName, on_added, NULL); }
static void choose(int i) {
  if (i >= nv_profile_count()) { add_open(); return; }
  NvProfile p; if (!nv_profile_get(i, &p)) return;
  if (!p.pin) { leave_for(p.index); return; }
  P.pinFor = p; P.pinMode = 1; P.pad = 0; P.pin[0] = 0; P.err[0] = 0;
}
static void pad_press(int k) {
  if (P.busy) return; size_t n = strlen(P.pin); P.err[0] = 0;
  if (k == 9) { if (n) P.pin[n - 1] = 0; return; }
  if (k == 11) { if (n) { P.busy = 1; nv_profile_verify_pin(P.pinFor.index, P.pin, on_pin, NULL); } return; }
  if (n < 8) { P.pin[n] = k == 10 ? '0' : (char)('1' + k); P.pin[n + 1] = 0; }
}
static int current_locked(void) { NvProfile p; for (int i = 0; i < nv_profile_count(); i++) if (nv_profile_get(i, &p) && p.index == nv_profile_current()) return p.pin; return 0; }
static int key(int k) {
  if (!P.open || (P.mode == PF_START && !app_splash_done)) return 1;
  if (P.addMode) {
    if (P.busy) return 1;
    if (k == K_BACK && !P.addName[0]) { P.addMode = 0; P.err[0] = 0; osk_close(&P.osk); return 1; }
#if SB_WINDOWED
    if (k == K_OK && plat_text_input() && !app_ptr_pressing()) { P.err[0] = 0; add_submit(); return 1; } /* a keyboard's Enter: done (a click types the key under it) */
#endif
    P.err[0] = 0; if (osk_key(&P.osk, k, P.addName, 21) == 2) add_submit();
    return 1; }
  if (P.pinMode) {
    if (k == K_BACK) { P.pinMode = 0; P.err[0] = 0; return 1; }
    int c = P.pad % 3, r = P.pad / 3;
    if (k == K_LEFT && c > 0) P.pad--; else if (k == K_RIGHT && c < 2) P.pad++; else if (k == K_UP && r > 0) P.pad -= 3; else if (k == K_DOWN && r < 3) P.pad += 3; else if (k == K_OK) pad_press(P.pad);
    return 1; }
  int n = tile_count();
  if (k == K_LEFT && P.idx > 0) P.idx--; else if (k == K_RIGHT && P.idx < n - 1) P.idx++;
  else if (k == K_OK) choose(P.idx);
  else if (k == K_BACK) {
    if (P.mode == PF_SIGNIN) { close_now(); login_back_to_form(); }
    else if (P.mode == PF_START && current_locked()) app_quit();
    else close_now(); }
  return 1;
}
static void tick(float dt) { osk_tick(&P.osk, dt); float a = dt * 14 > 1 ? 1 : dt * 14; for (int i = 0; i < MAXP; i++) P.foc[i] += ((i == P.idx && !P.pinMode ? 1.0f : 0.0f) - P.foc[i]) * a; }
static void h_foc(int i, int b) { (void)b; if (!P.pinMode) P.idx = i; else P.pad = i; }
static void h_act(int i, int b) { (void)b; if (!P.open) return; if (P.pinMode) pad_press(i); else { P.idx = i; choose(i); } }
static void disc(float x, float y, float D, unsigned color, const char *name, float foc, int lock) {
  float s = 1 + 0.08f * foc, cx = x + D / 2, cy = y + D / 2; gfx_scale_push(s, s, cx, cy);
  if (foc > 0.01f) gfx_ring(x - 10, y - 10, D + 20, D + 20, (D + 20) / 2, 5, WHITE_A((int)(240 * foc)));
  gfx_rrect(x, y, D, D, D / 2, RGB((color >> 16) & 255, (color >> 8) & 255, color & 255)); gfx_rrect_v(x, y, D, D, D / 2, WHITE_A(50), WHITE_A(0));
  char ini[8] = { 0 }; { const unsigned char *u = (const unsigned char *)name; int len = !*u ? 0 : (*u & 0x80) ? ((*u & 0xe0) == 0xc0 ? 2 : (*u & 0xf0) == 0xe0 ? 3 : 4) : 1; memcpy(ini, name, (size_t)len); } if (ini[0] >= 'a' && ini[0] <= 'z') ini[0] -= 32;
  float px = D * 0.46f; text_draw(ini, cx, cy - text_line_height(px) / 2, px, 800, WHITE_A(255), 0, 0, ALIGN_CENTER);
  gfx_scale_pop();
  text_draw(name, cx, y + D + 34, 32, foc > 0.5f ? 700 : 500, WHITE_A(foc > 0.5f ? 255 : 190), 0, D + 40, ALIGN_CENTER);
  if (lock) text_draw(T("PIN"), cx, y + D + 34 + text_line_height(32) + 4, 22, 600, WHITE_A(150), 1, 0, ALIGN_CENTER);
}
static void add_tile(float x, float y, float D, float foc) {
  float s = 1 + 0.08f * foc, cx = x + D / 2, cy = y + D / 2; gfx_scale_push(s, s, cx, cy);
  if (foc > 0.01f) gfx_ring(x - 10, y - 10, D + 20, D + 20, (D + 20) / 2, 5, WHITE_A((int)(240 * foc)));
  gfx_rrect(x, y, D, D, D / 2, WHITE_A(26)); float px = D * 0.5f; text_draw("+", cx, cy - text_line_height(px) / 2, px, 500, WHITE_A(230), 0, 0, ALIGN_CENTER);
  gfx_scale_pop();
  text_draw(T("Add profile"), cx, y + D + 34, 32, foc > 0.5f ? 700 : 500, WHITE_A(foc > 0.5f ? 255 : 190), 0, D + 40, ALIGN_CENTER);
}
static void draw(void) {
  if (!P.was) return; double t = app_now(); float op;
  if (P.open) { op = (float)(t - P.at) / 0.2f; if (op > 1) op = 1; } else { op = 1 - (float)(t - P.outAt) / 0.18f; if (op <= 0) return; }
  gfx_opacity_push(op);
  if (P.mode == PF_SIGNIN) login_backdrop_draw();
  else gfx_rect(0, 0, SB_W, SB_H, (P.mode == PF_START && current_locked()) || P.pinMode || P.addMode ? PAGE | 0xff000000u : RGBA(8, 8, 14, 225)); /* a locked profile's Home shows nothing until its PIN */
  if (P.open) hit_shield(0, 0, SB_W, SB_H);
  if (P.addMode) {
    float kw = 10 * 76 + 9 * 8, fx = SB_W / 2 - kw / 2, fy = 270, fh = 92; char shown[32]; snprintf(shown, sizeof shown, "%s|", P.addName);
    text_draw(T("Add profile"), SB_W / 2, 130, 60, 700, WHITE_A(255), -0.5f, 0, ALIGN_CENTER);
    gfx_rrect(fx, fy, kw, fh, 20, WHITE_A(26)); text_draw(shown, SB_W / 2, fy + fh / 2 - text_line_height(38) / 2, 38, 600, WHITE_A(240), 0, kw - 40, ALIGN_CENTER);
    if (P.busy || P.err[0]) text_draw(P.busy ? T("Adding\xe2\x80\xa6") : P.err, SB_W / 2, fy + fh + 14, 26, 500, P.busy ? WHITE_A(190) : RGB(0xff, 0x9a, 0x9a), 0, 800, ALIGN_CENTER);
    osk_draw(&P.osk, fx, fy + fh + 66, kw);
  } else if (!P.pinMode) {
    int n = tile_count(); float D = n <= 5 ? 190 : 140, G = n <= 5 ? 64 : 40, W = n * D + (n - 1) * G, x = SB_W / 2 - W / 2, y = SB_H / 2 - D / 2 - 30;
    text_draw(T("Who\xe2\x80\x99s watching?"), SB_W / 2, y - 150, 60, 700, WHITE_A(255), -0.5f, 0, ALIGN_CENTER);
    for (int i = 0; i < n; i++) { NvProfile p; int real = nv_profile_get(i, &p); float px = app_rtl() ? x + W - D - i * (D + G) : x + i * (D + G);
      if (real) disc(px, y, D, p.color, p.name, P.foc[i], p.pin); else add_tile(px, y, D, P.foc[i]); if (P.open) hit_add(px, y, D, D, h_foc, h_act, i, 0); }
  } else {
    static const char *const L[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "<", "0", "OK" };
    float KW = 130, KH = 92, G = 14, pw = 3 * KW + 2 * G, x0 = SB_W / 2 - pw / 2, y0 = SB_H / 2 - 110;
    text_draw(T("Enter PIN"), SB_W / 2, y0 - 240, 52, 700, WHITE_A(255), -0.5f, 0, ALIGN_CENTER);
    text_draw(P.pinFor.name, SB_W / 2, y0 - 240 + text_line_height(52) + 4, 30, 500, WHITE_A(190), 0, 800, ALIGN_CENTER);
    size_t n = strlen(P.pin); float DS = 26, DG = 22, dw = n ? n * DS + (n - 1) * DG : 0; for (size_t i = 0; i < n; i++) gfx_rrect(SB_W / 2 - dw / 2 + i * (DS + DG), y0 - 66, DS, DS, DS / 2, WHITE_A(240));
    if (P.err[0]) text_draw(P.err, SB_W / 2, y0 - 30 - 36, 26, 500, RGB(0xff, 0x9a, 0x9a), 0, 800, ALIGN_CENTER);
    for (int i = 0; i < 12; i++) { float x = x0 + (i % 3) * (KW + G), y = y0 + (i / 3) * (KH + G); int foc = i == P.pad;
      gfx_rrect(x, y, KW, KH, 20, foc ? WHITE_A(240) : WHITE_A(26)); text_draw(L[i], x + KW / 2, y + KH / 2 - text_line_height(36) / 2, 36, 700, foc ? RGB(10, 10, 16) : WHITE_A(235), 0, 0, ALIGN_CENTER);
      if (P.open) hit_add(x, y, KW, KH, h_foc, h_act, i, 0); }
  }
  gfx_opacity_pop();
}
/* a keyboard types the new profile's name (Backspace comes as "\b") */
static void text_in(const char *u) { if (!P.open || !u) return; if (!strcmp(u, "\b") && (!P.addMode || !P.addName[0])) { key(K_BACK); return; } /* Backspace with nothing to delete: Back */
  if (!P.addMode || P.busy) return; size_t n = strlen(P.addName);
  if (!strcmp(u, "\b")) { if (n) { n--; while (n && (P.addName[n] & 0xc0) == 0x80) n--; P.addName[n] = 0; } P.err[0] = 0; return; }
  if ((unsigned char)u[0] < ' ') return; size_t m = strlen(u); if (n + m >= 21) return; memcpy(P.addName + n, u, m + 1); P.osk.shift = 0; P.err[0] = 0; }
static Screen overlay = { NULL, NULL, key, tick, draw, NULL, NULL, text_in };
void profiles_at_start(void) { if (!acct_is_nuvio() || !sb_signed_in()) return; if ((nv_profile_count() > 1 && strcmp(store_get("askProfile", "true"), "false")) || current_locked()) profiles_open(PF_START); }
void profiles_open(int mode) {
  fprintf(stderr, "[profiles] open, mode %d\n", mode); P.open = 1; P.was = 1; P.mode = mode; P.pinMode = 0; P.addMode = 0; P.osk.open = 0; P.osk.a.v = 0; P.busy = 0; P.err[0] = 0; P.idx = 0; P.at = app_now();
  NvProfile p; for (int i = 0; i < nv_profile_count() && i < MAXP; i++) if (nv_profile_get(i, &p) && p.index == nv_profile_current()) P.idx = i;
  app_overlay = &overlay;
}
