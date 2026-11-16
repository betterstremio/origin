/* SB_SCROLL=<profile> or SB_INPUT="ms:key:down|up|rep,...": the app presses keys on a wall-clock timeline (SDL_GetTicks, as
   stress.c does), through app_key as real key events are, held keys repeating with repeat=1. The first key comes 3 s
   after Home is ready. Per-phase fill numbers are in fillstat.c. Development only (see scrollin.h). */
#include "core/scrollin.h"
#include "core/app.h"
#include "core/fillstat.h"
#include "core/keys.h"
#include "core/recs.h"
#include "screens/board.h"
#include "screens/profiles.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { S_PH, S_TAPS, S_HOLD, S_WAIT, S_END, S_GOROW, S_EVT, S_QUIT };
enum { HC_LASTROW, HC_ROW_START, HC_COL_START, HC_MS };
enum { X_NONE, X_DOWN, X_UP, X_RIGHT, X_LEFT, X_LAST, X_START, X_BACKH }; /* what the phase should end on */
typedef struct {
  int op, key, n, every, arg, kind;
  char name[28];
} Step;
#define MAXSTEP 128
static Step st[MAXSTEP];
static int nst, cur, on, started, phOn;
static double readyAt, base, holdRel;
static int sub; /* progress inside the step */
static int startRow, startCol, phRow, phCol, holdDown;
static char profName[32];
static int N = 8, M = 10, nPh = 8; /* (nPh: the taps the next phase makes, for its expected end) */
static double lastTap;
static int curKind, nRows0;
static unsigned recsV0;
static int beginR, beginC;

int scrollin_on(void) {
  return on;
}
static void add(int op, int key, int n, int every, int arg) {
  if (nst < MAXSTEP) {
    st[nst].op = op;
    st[nst].key = key;
    st[nst].n = n;
    st[nst].every = every;
    st[nst].arg = arg;
    st[nst].kind = 0;
    st[nst].name[0] = 0;
    nst++;
  }
}
static void ph(const char *name, int kind) {
  add(S_PH, 0, nPh, 0, 0);
  st[nst - 1].kind = kind;
  snprintf(st[nst - 1].name, sizeof st[nst - 1].name, "%s", name);
}
static int key_of(const char *n) {
  static const struct {
    const char *n;
    int k;
  } M_[] = { { "left", K_LEFT }, { "right", K_RIGHT }, { "up", K_UP },   { "down", K_DOWN },
             { "ok", K_OK },     { "back", K_BACK },   { "red", K_RED }, { "green", K_GREEN } };
  for (size_t i = 0; i < sizeof M_ / sizeof M_[0]; i++)
    if (!strcmp(M_[i].n, n))
      return M_[i].k;
  return 0;
}
/* a phase of N taps down, then back up, at one pace (no rest between: a hand does not rest) */
static void taps_vert(const char *tag, int every) {
  char nm[40];
  snprintf(nm, sizeof nm, "%s-down", tag);
  nPh = N;
  ph(nm, X_DOWN);
  add(S_TAPS, K_DOWN, N, every, 0);
  add(S_END, 0, 0, 0, 0); /* (arg 0: straight on) */
  snprintf(nm, sizeof nm, "%s-up", tag);
  ph(nm, X_START);
  add(S_TAPS, K_UP, N, every, 0);
  add(S_END, 0, 0, 0, 1200);
}
static void taps_horiz(const char *tag, int every) {
  char nm[40];
  add(S_GOROW, 0, 0, 0, 1);
  add(S_WAIT, 0, 0, 0, 2000);
  snprintf(nm, sizeof nm, "%s-right", tag);
  nPh = M;
  ph(nm, X_RIGHT);
  add(S_TAPS, K_RIGHT, M, every, 0);
  add(S_END, 0, 0, 0, 0);
  snprintf(nm, sizeof nm, "%s-left", tag);
  ph(nm, X_BACKH);
  add(S_TAPS, K_LEFT, M, every, 0);
  add(S_END, 0, 0, 0, 1200);
}
static int build(const char *p) {
  if (!strcmp(p, "CTRL")) {
    nPh = N < 6 ? N : 6;
    ph("CTRL-down", X_DOWN);
    add(S_TAPS, K_DOWN, N < 6 ? N : 6, 1000, 0);
    add(S_END, 0, 0, 0, 1500);
    ph("CTRL-up", X_START);
    add(S_TAPS, K_UP, N < 6 ? N : 6, 1000, 0);
    add(S_END, 0, 0, 0, 1500);
  } else if (!strcmp(p, "T150") || !strcmp(p, "T250")) {
    int e = atoi(p + 1);
    taps_vert(p, e);
    taps_horiz(p, e);
  } else if (!strcmp(p, "H-V")) {
    ph("H-V-down", X_LAST);
    add(S_HOLD, K_DOWN, 0, 0, HC_LASTROW);
    add(S_END, 0, 0, 0, 1500);
    ph("H-V-up", X_START);
    add(S_HOLD, K_UP, 0, 0, HC_ROW_START);
    add(S_END, 0, 0, 0, 1500);
  } else if (!strcmp(p, "H-R")) {
    add(S_GOROW, 0, 0, 0, 2);
    add(S_WAIT, 0, 0, 0, 2500);
    ph("H-R-right", X_NONE); /* (how far a 3 s hold goes depends on the row: no column is checked) */
    add(S_HOLD, K_RIGHT, 0, 3000, HC_MS);
    add(S_END, 0, 0, 0, 1500);
    ph("H-R-left", X_BACKH);
    add(S_HOLD, K_LEFT, 0, 0, HC_COL_START);
    add(S_END, 0, 0, 0, 1500);
  } else if (!strcmp(p, "B5")) {
    add(S_GOROW, 0, 0, 0, 1);
    add(S_WAIT, 0, 0, 0, 2000);
    ph("B5-right", X_NONE);
    for (int i = 0; i < 3; i++) {
      add(S_TAPS, K_RIGHT, 5, 150, 0);
      add(S_WAIT, 0, 0, 0, 700);
    }
    add(S_END, 0, 0, 0, 0);
    ph("B5-down", X_NONE);
    for (int i = 0; i < 3; i++) {
      add(S_TAPS, K_DOWN, 5, 150, 0);
      add(S_WAIT, 0, 0, 0, 700);
    }
    add(S_END, 0, 0, 0, 1200);
  } else if (!strcmp(p, "RT")) {
    nPh = N;
    ph("RT-down", X_DOWN);
    add(S_TAPS, K_DOWN, N, 150, 0);
    add(S_END, 0, 0, 0, 1000);
    ph("RT-up", X_START);
    add(S_TAPS, K_UP, N, 150, 0);
    add(S_END, 0, 0, 0, 1000);
    nPh = N;
    ph("RT-down2", X_DOWN);
    add(S_TAPS, K_DOWN, N, 150, 0);
    add(S_END, 0, 0, 0, 1500);
  } else
    return 0;
  add(S_QUIT, 0, 0, 0, 0);
  return 1;
}
/* "ms:key:down|up|rep,...": from the first key's time zero */
static int build_input(const char *s) {
  char buf[2048];
  snprintf(buf, sizeof buf, "%s", s);
  ph("INPUT", X_NONE);
  for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
    char k[24] = "", a[16] = "";
    int ms = 0;
    if (sscanf(t, "%d:%23[^:]:%15s", &ms, k, a) != 3 || !key_of(k))
      continue;
    add(S_EVT, key_of(k), a[0] == 'u' ? 0 : 1, a[0] == 'r', ms);
  }
  add(S_END, 0, 0, 0, 1500);
  add(S_QUIT, 0, 0, 0, 0);
  return 1;
}
void scrollin_init(void) {
  const char *p = getenv("SB_SCROLL"), *in = getenv("SB_INPUT");
  if (!p && !in)
    return;
  if (getenv("SB_SCROLL_N") && atoi(getenv("SB_SCROLL_N")) > 0)
    N = atoi(getenv("SB_SCROLL_N"));
  if (getenv("SB_SCROLL_M") && atoi(getenv("SB_SCROLL_M")) > 0)
    M = atoi(getenv("SB_SCROLL_M"));
  snprintf(profName, sizeof profName, "%s", p ? p : "INPUT");
  if (in ? !build_input(in) : !build(p)) {
    fprintf(stderr, "[scroll] unknown profile %s\n", p);
    return;
  }
  on = 1;
  fprintf(stderr, "[scroll] %s (%d steps), starts 3 s after Home is ready\n", profName, nst);
}
static int pos(int *r, int *c, int *n) {
  return board_focus_pos(r, c, n);
}
/* keys sent and waiting for the focus to move (key-to-motion) */
static double pend[8];
static int npend, pr0 = -9, pc0 = -9;
static void send(int key, int down, int rep, double sched) {
  if (getenv("SB_SCROLL_TRACE"))
    fprintf(stderr, "[scroll] t=%.0f key %d down %d rep %d\n", (double)SDL_GetTicks(), key, down, rep);
  app_key(key, down, rep);
  if (down && (key == K_UP || key == K_DOWN || key == K_LEFT || key == K_RIGHT)) {
    if (npend == 8) {
      memmove(pend, pend + 1, 7 * sizeof *pend);
      npend--;
    }
    pend[npend++] = sched;
  }
}
static void track_motion(double now) {
  int r, c, n;
  pos(&r, &c, &n);
  while (npend && now - pend[0] > 1500) {
    memmove(pend, pend + 1, (size_t)(npend - 1) * sizeof *pend);
    npend--;
  }
  if ((r != pr0 || c != pc0) && getenv("SB_SCROLL_TRACE"))
    fprintf(stderr, "[scroll] t=%.0f focus row %d col %d of %d\n", now, r, c, n);
  if ((r != pr0 || c != pc0) && pr0 != -9 && npend) {
    fill_key(now - pend[0]);
    memmove(pend, pend + 1, (size_t)(npend - 1) * sizeof *pend);
    npend--;
  }
  pr0 = r;
  pc0 = c;
}
static void next(double at) {
  cur++;
  sub = 0;
  base = at;
}
static int expect_row(int kind, int cnt, int *ec) {
  int r, c, n;
  pos(&r, &c, &n);
  *ec = -999;
  if (kind != X_BACKH) {
    beginR = r;
    beginC = c;
  }
  switch (kind) {
  case X_DOWN: {
    int e = r + cnt;
    return e > n - 1 ? n - 1 : e;
  }
  case X_UP:
    return r - cnt < -1 ? -1 : r - cnt;
  case X_RIGHT:
    *ec = c + cnt;
    return r;
  case X_START:
    return startRow;
  case X_LAST:
    return n - 1;
  case X_BACKH: /* back to where the last phase began (sideways: the same row and column) */
    *ec = beginC;
    return beginR;
  default:
    return -999;
  }
}
void scrollin_tick(void) {
  if (!on || cur >= nst)
    return;
  double now = SDL_GetTicks();
  track_motion(now);
  if (!started) {
    if (profiles_is_open()) { /* "Who's watching?": the first profile */
      static double okAt;
      if (now - okAt > 1000) {
        okAt = now;
        app_key(K_OK, 1, 0);
        app_key(K_OK, 0, 0);
        readyAt = 0;
      }
      return;
    }
    if (SCR == SCR_HOME && board_home_ready()) {
      int r0, c0, n0;
      pos(&r0, &c0, &n0);
      unsigned rv = recs_version();
      if (!readyAt || n0 != nRows0 || rv != recsV0) { /* (Home's rows arrive in stages and the board is rebuilt: 3 s of calm) */
        readyAt = now;
        nRows0 = n0;
        recsV0 = rv;
      }
      if (now - readyAt >= 3000 && (rv || !recs_on() || now > 45000)) {
        started = 1;
        base = now;
        int c, n;
        pos(&startRow, &startCol, &n);
        (void)c;
        fprintf(stderr, "[scroll] go: focus row %d col %d of %d rows\n", startRow, startCol, n);
      }
    } else if (now > 120000) {
      fprintf(stderr, "[scroll] Home never ready, giving up\n");
      on = 0;
      app_quit();
    }
    return;
  }
  for (int guard = 0; guard < 64 && cur < nst; guard++) {
    Step *s = &st[cur];
    int r, c, n;
    pos(&r, &c, &n);
    switch (s->op) {
    case S_PH: {
      int ec;
      int er = expect_row(s->kind, s->n, &ec);
      fill_phase_begin(s->name, er, ec);
      curKind = s->kind;
      phOn = 1;
      next(base);
      continue;
    }
    case S_WAIT:
      if (now < base + s->arg)
        return;
      next(base + s->arg);
      continue;
    case S_TAPS: { /* tap sub: key down at base+sub*every, up 40 ms (or half the pace) later */
      int up = s->every / 2 < 40 ? s->every / 2 : 40, k = sub / 2;
      if (k >= s->n) {
        next(base + (double)(s->n - 1) * s->every + s->every);
        continue;
      }
      double t = base + (double)k * s->every + ((sub & 1) ? up : 0);
      if (now < t)
        return;
      send(s->key, !(sub & 1), 0, t);
      sub++;
      continue;
    }
    case S_HOLD: {
      if (!holdDown) { /* press, then repeats at 500 ms / 50 ms (Android's), until the stop */
        send(s->key, 1, 0, now);
        holdDown = 1;
        sub = 1;
        holdRel = now;
        base = now;
        return;
      }
      int stop = s->arg == HC_LASTROW   ? r >= n - 1
                 : s->arg == HC_ROW_START ? r <= startRow
                 : s->arg == HC_COL_START ? c <= beginC
                                          : now - base >= s->every;
      if (stop || now - base > 60000) {
        send(s->key, 0, 0, now);
        holdDown = 0;
        next(now);
        continue;
      }
      double t = base + 500 + (double)(sub - 1) * 50;
      if (now < t)
        return;
      send(s->key, 1, 1, t);
      sub++;
      continue;
    }
    case S_GOROW: {
      int k = sub / 2;
      if (r >= s->arg || k >= 12) {
        if (now - lastTap < 300)
          return;
        next(now);
        continue;
      }
      double t = base + (double)k * 700 + ((sub & 1) ? 40 : 0);
      if (now < t)
        return;
      send(K_DOWN, !(sub & 1), 0, t);
      if (sub & 1)
        lastTap = now;
      sub++;
      continue;
    }
    case S_EVT: {
      double t = base + s->arg;
      if (now < t)
        return;
      send(s->key, s->n, (int)s->every, t);
      cur++;
      continue;
    }
    case S_END:
      if (sub == 0) {
        fill_phase_active(0);
        holdRel = now;
        sub = 1;
      }
      if (now - holdRel < s->arg)
        return;
      if (curKind == X_LAST)
        fill_phase_expect(n - 1, -999); /* (rows may have come while holding) */
      fill_phase_end(r, c);
      phOn = 0;
      next(now);
      continue;
    case S_QUIT:
      fprintf(stderr, "[scroll] done %s\n", profName);
      on = 0;
      if (!getenv("SB_SCROLL_KEEP"))
        app_quit();
      return;
    }
  }
}
