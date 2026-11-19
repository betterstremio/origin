/* The release lookup matches only this package's own name for the running app id (<appid>_<x.y.z>_arm.ipk),
   so the native build is never offered another build's package. The install goes through the Homebrew
   Channel (it downloads, checks the SHA-256 and installs as root); before it starts, a small script handed to
   the Channel waits for the new version's appinfo.json and opens the app again, since nothing else in this
   process survives the install. */
#include "core/update.h"
#include <math.h>
#include "ui/glass.h"
#include "core/app.h"
#include "core/net.h"
#include "core/store.h"
#include "core/i18n.h"
#include "core/keys.h"
#include "core/hit.h"
#include "platform/plat.h"
#include "screens/settings.h"
#include "screens/player.h"
#include "ui/gfx.h"
#include "ui/text.h"
float key_hint(float x, float y, int key, const char *label, uint32_t col,
               int alignRight); /* (ui/widgets.h: its pill_hide would clash with ours) */
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef SB_APP_ID
#define SB_APP_ID "io.github.betterstremio.origin"
#endif
/* the id 3.0.0 and later ship under. A build under the old id (the 2.1.12 bridge) also accepts packages named with it,
   so existing installs move over; a build already under the new id behaves as it always did. */
#ifndef SB_NEW_APP_ID
#define SB_NEW_APP_ID "io.github.betterstremio.origin"
#endif
#define SB_BRIDGE (strcmp(SB_APP_ID, SB_NEW_APP_ID) != 0)
/* this build's own package kind among the release's files: <appid>_<x.y.z><PKG_KIND> */
#define PKG_KIND SB_PKG_KIND
#define PKG_KIND_LEN (sizeof PKG_KIND - 1)

#define UPD_GH                                                                                                 \
  "https://api.github.com/repos/betterstremio/origin/releases?per_page=5" /* one tag per version, one file per \
                                                                             platform: PKG_KIND picks ours */
/* the website's manifest (tools/webos-feed.sh, written by every release) is asked first: GitHub's API allows 60 calls
   an hour per address without an account, and a home's TV shares that with every other device there. It names only the
   webOS IPK, so the other platforms go to the release list when it has nothing for them. */
#define UPD_MAN "https://betterstremio.github.io/webos/api/apps/" SB_APP_ID "/manifest.json"
typedef struct {
  char ver[24], url[512], sha[72];
  char notes[5][256];
  int nnotes;
  int newId; /* the package is under SB_NEW_APP_ID (only ever set by the bridge) */
} Rel;
static struct {
  char cur[24], appinfo[1200];
  int read;
  Rel avail;
  int has, state;
  char msg[240];
  unsigned serial;
  int pill, pf;
  double hideAt, openAt;
  Tween pa, slide, dot;
  unsigned long job;
  int started;
  unsigned shown;
  double shownAt, goAt;
  int kind;
  int room; /* the install waits for the cache to be emptied (cache_room_done) */
  struct {
    char lb[128], badge[48];
    void (*ok)(void);
    int pend;
  } n; /* kind 1: the other notice's capsule */
} U;
static void changed(void) {
  U.serial++;
}
static int ver_cmp(const char *a, const char *b);
static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* "1.0.0" from appinfo.json beside the binary (the package), or VERSION for a development build run from a checkout */
static void read_current(void) {
  if (U.read)
    return;
  U.read = 1;
  {
    char p[1200];
    snprintf(p, sizeof p, "%s/../appinfo.json", plat_asset_dir());
    FILE *f = fopen(p, "rb");
    char b[4096];
    size_t n = f ? fread(b, 1, sizeof b - 1, f) : 0;
    b[n] = 0;
    if (f)
      fclose(f);
    cJSON *j = cJSON_Parse(b);
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "version");
    if (cJSON_IsString(v) && v->valuestring) {
      snprintf(U.cur, sizeof U.cur, "%s", v->valuestring);
      snprintf(U.appinfo, sizeof U.appinfo, "%s", p);
    }
    cJSON_Delete(j);
  }
  if (!U.cur[0]) {
    char p[1200];
    snprintf(p, sizeof p, "%s/../VERSION", plat_asset_dir());
    FILE *f = fopen(p, "rb");
    if (f) {
      if (fscanf(f, "%23s", U.cur) != 1)
        U.cur[0] = 0;
      fclose(f);
    }
  }
  {
    char w[1200], sh[1200];
    snprintf(w, sizeof w, "%s/relaunch.wait", plat_data_dir());
    snprintf(sh, sizeof sh, "%s/relaunch.sh", plat_data_dir());
    if (access(w, F_OK))
      unlink(sh);
  } /* the script that opened the app again after its update: done once it stops waiting */
  /* a remembered update that is now installed (or older) is forgotten */
  const char *uv = store_get("updVer", "");
  if (*uv && U.cur[0] && ver_cmp(uv, U.cur) <= 0)
    store_remove("updVer");
}
const char *update_current(void) {
  read_current();
  return U.cur;
}
const char *update_avail(void) {
  return U.has ? U.avail.ver : NULL;
}
int update_note_count(void) {
  return U.has ? U.avail.nnotes : 0;
}
const char *update_note(int i) {
  return U.has && i >= 0 && i < U.avail.nnotes ? U.avail.notes[i] : "";
}
int update_state(void) {
  return U.state;
}
const char *update_msg(void) {
  return U.msg;
}
unsigned update_serial(void) {
  return U.serial;
}

static int ver_cmp(const char *a, const char *b) {
  for (int i = 0; i < 3; i++) {
    long x = a ? strtol(a, NULL, 10) : 0, y = b ? strtol(b, NULL, 10) : 0;
    if (x != y)
      return x > y ? 1 : -1;
    a = a ? strchr(a, '.') : NULL;
    if (a)
      a++;
    b = b ? strchr(b, '.') : NULL;
    if (b)
      b++;
  }
  return 0;
}
static int ws(const char *s) {
  return *s == ' ' || *s == '\t' || *s == '\v' || *s == '\f' || *s == '\r' ? 1 : !strncmp(s, "\xc2\xa0", 2) ? 2 : 0;
}
/* the release's bullet points as plain text, the first five: /^\s*[-*•]\s+(.+)$/, then no **, __, ` and [text](link) ->
 * text */
static int notes_from(const char *md, char out[5][256]) {
  int n = 0;
  if (!md)
    return 0;
  for (const char *l = md; *l && n < 5;) {
    const char *e = strchr(l, '\n');
    size_t len = e ? (size_t)(e - l) : strlen(l);
    if (len && l[len - 1] == '\r')
      len--;
    char line[1024];
    if (len >= sizeof line)
      len = sizeof line - 1;
    memcpy(line, l, len);
    line[len] = 0;
    l = e ? e + 1 : l + strlen(l);
    char *p = line;
    int k;
    while ((k = ws(p)))
      p += k;
    if (*p == '-' || *p == '*')
      p++;
    else if (!strncmp(p, "\xe2\x80\xa2", 3))
      p += 3;
    else
      continue;
    int nsp = 0;
    while ((k = ws(p))) {
      p += k;
      nsp++;
    }
    if (!nsp)
      continue;
    if (!*p && nsp < 2)
      continue; /* (.+) needs a character: one of the spaces when nothing else follows */
    char t[1024];
    size_t o = 0;
    for (const char *q = p; *q && o + 1 < sizeof t;) {
      if (!strncmp(q, "**", 2) || !strncmp(q, "__", 2)) {
        q += 2;
        continue;
      }
      if (*q == '`') {
        q++;
        continue;
      }
      t[o++] = *q++;
    }
    t[o] = 0;
    char u[1024];
    o = 0;
    for (const char *q = t; *q && o + 1 < sizeof u;) {
      if (*q == '[') {
        const char *c = q + 1;
        while (*c && *c != ']')
          c++;
        if (*c == ']' && c > q + 1 && c[1] == '(') {
          const char *d = c + 2;
          while (*d && *d != ')')
            d++;
          if (*d == ')') {
            size_t m = (size_t)(c - q - 1);
            if (o + m >= sizeof u)
              m = sizeof u - 1 - o;
            memcpy(u + o, q + 1, m);
            o += m;
            q = d + 1;
            continue;
          }
        }
      }
      u[o++] = *q++;
    }
    u[o] = 0;
    char *s = u;
    while ((k = ws(s)))
      s += k;
    size_t sl = strlen(s);
    while (sl && (s[sl - 1] == ' ' || s[sl - 1] == '\t' || s[sl - 1] == '\r'))
      s[--sl] = 0;
    snprintf(out[n++], 256, "%s", s);
  }
  return n;
}
static void pill_show(void);
static double nextAt;
static int forceCheck, pillPend; /* the next check while the app stays open; a check that skips the 6-hour rest; a
                                    notice that waits for the film to end */
static int reached; /* the release list answered: finding nothing for this package then means nothing newer */
/* the new-id app's appinfo.json: this app's own with the id swapped in its path (both live under
   .../usr/palm/applications/<id>/); 0 when this app's path does not hold its id */
static int new_appinfo(char *out, size_t cap) {
  char old[96];
  snprintf(old, sizeof old, "/%s/", SB_APP_ID);
  const char *at = U.appinfo[0] ? strstr(U.appinfo, old) : NULL;
  if (!at)
    return 0;
  snprintf(out, cap, "%.*s/%s/%s", (int)(at - U.appinfo), U.appinfo, SB_NEW_APP_ID, at + strlen(old));
  return 1;
}
/* the new-id app already installed at this version or later (webOS): nothing to offer, it is there to open */
static int new_app_has(const char *ver) {
  char p[1200], b[4096];
  if (!new_appinfo(p, sizeof p))
    return 0;
  FILE *f = fopen(p, "rb");
  if (!f)
    return 0;
  size_t n = fread(b, 1, sizeof b - 1, f);
  fclose(f);
  b[n] = 0;
  cJSON *j = cJSON_Parse(b), *v = cJSON_GetObjectItemCaseSensitive(j, "version");
  int has = cJSON_IsString(v) && v->valuestring && ver_cmp(v->valuestring, ver) >= 0;
  cJSON_Delete(j);
  return has;
}
static void done(const Rel *best, int manual) {
  char at[32];
  snprintf(at, sizeof at, "%.0f", now_ms());
  store_set("updAt", at);
  if (best && best->newId && new_app_has(best->ver)) {
    fprintf(stderr, "[update] %s is already installed as %s: nothing to offer\n", best->ver, SB_NEW_APP_ID);
    best = NULL;
    reached = 1; /* (an answer, and nothing newer for this app: "up to date", not "could not check") */
  }
  if (best && U.cur[0] && ver_cmp(best->ver, U.cur) > 0) {
    U.avail = *best;
    U.has = 1;
    U.state = UPD_AVAIL;
    store_set("updVer", best->ver);
    fprintf(stderr, "[update] %s available (running %s)\n", best->ver, U.cur);
    if (best->newId)
      fprintf(stderr, "[update] %s is under the new app id %s: installing it beside this one\n", best->ver,
              SB_NEW_APP_ID);
    if (!manual) {
      if (player_open())
        pillPend = 1;
      else
        pill_show();
    } /* (never over a film: after it) */
  } else {
    int none = best || reached;
    U.has = 0;
    U.state = none ? UPD_NONE : UPD_ERROR;
    store_remove("updVer");
    fprintf(stderr, "[update] %s\n", none ? "up to date" : "could not check");
    if (manual)
      toast(T(none ? "You have the latest version" : "Could not check for updates"), 2000, NULL);
    if (!none && U.started)
      nextAt = app_now() + 600; /* no answer (the network not up yet, the site away): again in 10 minutes */
  }
  changed();
}
static void on_releases(int st, const unsigned char *body, size_t len, void *ud);
static void on_manifest(int st, const unsigned char *body, size_t len, void *ud) {
  (void)len;
  int manual = (int)(size_t)ud;
  Rel r;
  memset(&r, 0, sizeof r);
  int ok = 0;
  cJSON *m = st == 200 ? cJSON_Parse((const char *)body) : NULL;
  cJSON *v = cJSON_GetObjectItemCaseSensitive(m, "version"), *u = cJSON_GetObjectItemCaseSensitive(m, "ipkUrl"),
        *h = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "ipkHash"), "sha256");
  if (cJSON_IsString(v) && cJSON_IsString(u) && cJSON_IsString(h) && *v->valuestring && *u->valuestring &&
      *h->valuestring) {
    size_t ul = strlen(u->valuestring);
    if (ul > PKG_KIND_LEN &&
        !strcmp(u->valuestring + ul - PKG_KIND_LEN,
                PKG_KIND)) { /* the listing may name another build under the same id: only this package's kind */
      ok = 1;
      const char *fn = strrchr(u->valuestring, '/');
      fn = fn ? fn + 1 : u->valuestring;
      r.newId = SB_BRIDGE && !strncmp(fn, SB_NEW_APP_ID "_", sizeof SB_NEW_APP_ID);
      snprintf(r.ver, sizeof r.ver, "%s", v->valuestring);
      snprintf(r.url, sizeof r.url, "%s", u->valuestring);
      snprintf(r.sha, sizeof r.sha, "%s", h->valuestring);
      cJSON *ns = cJSON_GetObjectItemCaseSensitive(m, "notes"), *x;
      if (cJSON_IsArray(ns)) {
        cJSON_ArrayForEach(x, ns)
          if (r.nnotes < 5 && cJSON_IsString(x))
            snprintf(r.notes[r.nnotes++], 256, "%s", x->valuestring);
      } else {
        cJSON *rn = cJSON_GetObjectItemCaseSensitive(m, "releaseNotes");
        r.nnotes = notes_from(cJSON_IsString(rn) ? rn->valuestring : NULL, r.notes);
      }
    }
  }
  cJSON_Delete(m);
  if (ok)
    done(&r, manual);
  else {
    const char *gh = getenv("SB_UPD_GH");
    net_get_timeout(gh ? gh : UPD_GH, 15000, on_releases, ud);
  } /* no manifest, or none for this package: the release list */
}
static int is_hex64(const char *s) {
  for (int i = 0; i < 64; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
      return 0;
  return !s[64];
}
static void on_releases(int st, const unsigned char *body, size_t len, void *ud) {
  (void)len;
  int manual = (int)(size_t)ud;
  Rel best;
  int has = 0;
  memset(&best, 0, sizeof best);
  cJSON *list = st == 200 ? cJSON_Parse((const char *)body) : NULL, *r, *a;
  cJSON *arr = cJSON_IsArray(list) ? list : NULL;
  reached = arr != NULL;
  cJSON_ArrayForEach(r, arr) {
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "draft")) ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "prerelease")))
      continue;
    cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(r, "assets")) {
      cJSON *nm = cJSON_GetObjectItemCaseSensitive(a, "name"), *dg = cJSON_GetObjectItemCaseSensitive(a, "digest"),
            *du = cJSON_GetObjectItemCaseSensitive(a, "browser_download_url");
      if (!cJSON_IsString(nm) || !cJSON_IsString(dg) || !cJSON_IsString(du))
        continue;
      const char *n = nm->valuestring;
      int x, y, z, used = 0, nw = 0;
      const char *pre = SB_APP_ID "_";
      if (SB_BRIDGE && !strncmp(n, SB_NEW_APP_ID "_", sizeof SB_NEW_APP_ID))
        pre = SB_NEW_APP_ID "_", nw = 1;
      size_t pl = strlen(pre);
      if (strncmp(n, pre, pl) || sscanf(n + pl, "%d.%d.%d%n", &x, &y, &z, &used) != 3 ||
          strcmp(n + pl + used, PKG_KIND))
        continue; /* ^<appid>_(\d+\.\d+\.\d+)_arm\.ipk$ (_android.apk on Android) */
      {
        const char *q = n + pl;
        for (int i = 0; i < used; i++)
          if (!((q[i] >= '0' && q[i] <= '9') || q[i] == '.'))
            used = -1;
        if (used < 0)
          continue;
      }
      if (strncmp(dg->valuestring, "sha256:", 7) || !is_hex64(dg->valuestring + 7))
        continue;
      char ver[24];
      snprintf(ver, sizeof ver, "%.*s", used, n + pl);
      int c = has ? ver_cmp(ver, best.ver) : 1;
      if (c < 0 || (c == 0 && !(nw && !best.newId)))
        continue; /* newest wins; on the same version the new id does */
      memset(&best, 0, sizeof best);
      has = 1;
      best.newId = nw;
      snprintf(best.ver, sizeof best.ver, "%s", ver);
      snprintf(best.url, sizeof best.url, "%s", du->valuestring);
      snprintf(best.sha, sizeof best.sha, "%s", dg->valuestring + 7);
      cJSON *b = cJSON_GetObjectItemCaseSensitive(r, "body");
      best.nnotes = notes_from(cJSON_IsString(b) ? b->valuestring : NULL, best.notes);
    }
  }
  cJSON_Delete(list);
  done(has ? &best : NULL, manual);
}
void update_check(int manual) {
  read_current();
  if (U.state == UPD_CHECKING || U.state == UPD_INSTALLING)
    return;
  const char *gh = getenv("SB_UPD_GH"); /* development: another release list (a local server) */
  if (!manual && !forceCheck && !gh && now_ms() - atof(store_get("updAt", "0")) < 6 * 3600000.0 &&
      !*store_get("updVer", "")) {
    U.state = UPD_IDLE;
    return;
  }
  U.state = UPD_CHECKING;
  U.msg[0] = 0;
  reached = 0;
  changed();
  const char *mf =
      getenv("SB_UPD_MAN"); /* development: another manifest; SB_UPD_GH alone tests the release list by itself */
  if (mf || (!gh && !strcmp(PKG_KIND, "_arm.ipk"))) /* (the manifest names only the webOS IPK) */
    net_get_timeout(mf ? mf : UPD_MAN, 15000, on_manifest, (void *)(size_t)manual);
  else
    net_get_timeout(gh ? gh : UPD_GH, 15000, on_releases, (void *)(size_t)manual);
}

/* ---- install ---- */
static char waitPath[1200], shPath[1200];
static void relaunch_cancel(void) {
  if (waitPath[0])
    unlink(waitPath);
  if (shPath[0])
    unlink(shPath);
}
/* the Channel runs this as root, apart from the app: once appinfo.json shows the new version (at most 5 min), the app
 * opens again */
static void relaunch_arm(void) {
  if (!U.appinfo[0])
    return;
  /* the app to wait for and open: this one, or the new-id app that lands beside it (its appinfo.json sits in the same
     place with the id swapped in the path) */
  const char *id = SB_APP_ID;
  char info[1200];
  snprintf(info, sizeof info, "%s", U.appinfo);
  if (U.avail.newId) {
    if (!new_appinfo(info, sizeof info)) {
      fprintf(stderr, "[update] no relaunch waiter: %s has no /%s/ in it\n", U.appinfo, SB_APP_ID);
      return;
    }
    id = SB_NEW_APP_ID;
  }
  char *sh = shPath;
  snprintf(sh, sizeof shPath, "%s/relaunch.sh", plat_data_dir());
  snprintf(waitPath, sizeof waitPath, "%s/relaunch.wait", plat_data_dir());
  FILE *w = fopen(waitPath, "w");
  if (w)
    fclose(w);
  FILE *f = fopen(sh, "w");
  if (!f)
    return;
  fprintf(
      f,
      "#!/bin/sh\n# the app opens again once its update is in (written by the app before it asked for the install)\n"
      "M='%s'\ni=0\nwhile [ $i -lt 150 ] && [ -f \"$M\" ]; do\n  sleep 2; i=$((i+1))\n"
      "  if grep -q '\"version\": *\"%s\"' '%s' 2>/dev/null; then rm -f \"$M\"; sleep 2; luna-send -n 1 -f luna://com.webos.applicationManager/launch '{\"id\":\"%s\"}' >/dev/null 2>&1; exit 0; fi\n"
      "done\nrm -f \"$M\"\n",
      waitPath, U.avail.ver, info, id);
  fclose(f);
  char cmd[1400];
  snprintf(cmd, sizeof cmd, "setsid sh '%s' >/dev/null 2>&1 </dev/null &", sh);
  cJSON *p = cJSON_CreateObject();
  cJSON_AddStringToObject(p, "command", cmd);
  char *ps = cJSON_PrintUnformatted(p);
  cJSON_Delete(p);
  char *r = plat_luna_call("luna://org.webosbrew.hbchannel.service/exec", ps, 3000);
  free(ps);
  fprintf(stderr, "[update] relaunch waiter: %s\n", r ? r : "(no answer)");
  free(r);
}
static void on_install(const char *payload, void *ud) {
  (void)ud;
  cJSON *j = cJSON_Parse(payload);
  if (!j)
    return;
  cJSON *rv = cJSON_GetObjectItemCaseSensitive(j, "returnValue"),
        *et = cJSON_GetObjectItemCaseSensitive(j, "errorText"), *st = cJSON_GetObjectItemCaseSensitive(j, "statusText"),
        *pg = cJSON_GetObjectItemCaseSensitive(j, "progress");
  if (cJSON_IsFalse(rv) || (cJSON_IsString(et) && et->valuestring)) {
    const char *e = cJSON_IsString(et) ? et->valuestring : "";
    /* no Channel on this TV, or the app may not call it (an app installed in developer mode without the
       Channel): the hub answers at once, and the way that is left is the Channel's own install */
    int noChannel = strstr(e, "does not exist") || strstr(e, "Unknown service") || strstr(e, "Denied") ||
                    strstr(e, "not allowed") || strstr(e, "Not permitted");
    U.state = UPD_AVAIL;
    snprintf(U.msg, sizeof U.msg, "%s",
             noChannel ? T("Install it from the Homebrew Channel") : T1("The update failed: {e}", "e", e));
    if (U.job) {
      plat_luna_cancel(U.job);
      U.job = 0;
    }
    relaunch_cancel();
    fprintf(stderr, "[update] install failed: %s\n", e);
    toast(noChannel ? T("Install it from the Homebrew Channel") : T("The update failed"), 2500, NULL);
    changed();
    cJSON_Delete(j);
    return;
  }
  if (cJSON_IsString(st) && st->valuestring) {
    char pc[16] = "";
    if (cJSON_IsNumber(pg))
      snprintf(pc, sizeof pc, " %d%%", (int)(pg->valuedouble + 0.5));
    snprintf(U.msg, sizeof U.msg, "%s%s", T(st->valuestring), pc);
  }
  if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "finished"))) {
    snprintf(U.msg, sizeof U.msg, "%s", T("Installed: the app opens again in a moment"));
    fprintf(stderr, "[update] installed %s\n", U.avail.ver);
  }
  changed();
  cJSON_Delete(j);
}
static void install_go(void);
void update_install(void) {
  if (app_now() - U.openAt < 0.9)
    return; /* the OK that opened this page */
  if (!U.has) {
    update_check(1);
    return;
  }
  if (U.state == UPD_INSTALLING || U.state == UPD_CHECKING)
    return; /* (a check under way may still change what is offered) */
#if SB_MANUAL_UPDATE
  snprintf(U.msg, sizeof U.msg, "%s", T("Install the update with Apps2Samsung"));
  changed();
  return;
#endif
  U.state = UPD_INSTALLING;
  snprintf(U.msg, sizeof U.msg, "%s", T("Downloading…"));
  changed();
  fprintf(stderr, "[update] installing %s\n", U.avail.ver);
  if (!cache_room_start(120L * 1024 * 1024)) { /* the cache is being emptied: update_tick goes on when that is done */
    U.room = 1;
    return;
  }
  install_go();
}
static void install_go(void) {
#if SB_PLAT_INSTALLER
  if (!plat_install_start(U.avail.url, U.avail.sha, U.avail.ver)) {
    U.state = UPD_AVAIL;
    snprintf(U.msg, sizeof U.msg, "%s", T("The update failed"));
    changed();
  }
  return;
#endif
  relaunch_arm();
  cJSON *p = cJSON_CreateObject();
  cJSON_AddStringToObject(p, "ipkUrl", U.avail.url);
  cJSON_AddStringToObject(p, "ipkHash", U.avail.sha);
  cJSON_AddStringToObject(p, "id", U.avail.newId ? SB_NEW_APP_ID : SB_APP_ID);
  cJSON_AddTrueToObject(p, "subscribe");
  char *ps = cJSON_PrintUnformatted(p);
  cJSON_Delete(p);
  U.job = plat_luna_subscribe("luna://org.webosbrew.hbchannel.service/install", ps, on_install, NULL);
  free(ps);
  if (!U.job) {
    U.state = UPD_AVAIL;
    snprintf(U.msg, sizeof U.msg, "%s", T("Install it from the Homebrew Channel"));
    relaunch_cancel();
    changed();
  } /* no Channel (or the Mac preview) */
}

/* ---- the notice ---- */
int update_pill_focused(void) {
  return U.pf;
}
float update_dot_hide(void) {
  return U.dot.v;
}
static void pill_show(void) {
  if (!U.has || U.pill)
    return;
  fprintf(stderr, "[update] notice shown: version %s\n", U.avail.ver);
  U.kind = 0;
  U.pill = 1;
  U.shown++;
  U.shownAt = app_now();
  U.goAt = 0;
  U.hideAt = app_now() + 10;
  tween_to(&U.pa, 1, 0.3f, EASE_IN_OUT);
  tween_to(&U.slide, 1, 0.55f, EASE_LINEAR);
  tween_to(&U.dot, 1, 0.2f, EASE_IN_OUT);
}
static void pill_hide(void) {
  if (U.pf || !U.pill)
    return;
  U.pill = 0;
  U.hideAt = 0;
  tween_to(&U.pa, 0, 0.3f, EASE_IN_OUT);
  tween_to(&U.slide, 0, 0.45f, EASE_LINEAR);
  tween_to(&U.dot, 0, 0.2f, EASE_IN_OUT);
}
void update_open(void) {
  U.goAt = app_now();
  U.openAt = app_now();
  U.pf = 0;
  U.pill = 0;
  U.hideAt = 0;
  tween_to(&U.pa, 0, 0.3f, EASE_IN_OUT);
  tween_to(&U.slide, 0, 0.45f, EASE_LINEAR);
  tween_to(&U.dot, 0, 0.2f, EASE_IN_OUT);
  if (app_overlay) {
    app_overlay->key(K_BACK);
    app_overlay = NULL;
  }
  topNav.idx = NAV_SET;
  app_go(SCR_SET, NULL, 1);
  settings_open_about();
}
static void notice_ok(void);
int update_notice_key(void) {
  if (!U.pill || U.pa.v < 0.5f)
    return 0;
  if (U.kind == 1)
    notice_ok();
  else
    update_open();
  return 1;
} /* the red key while a notice is out: straight to what it is about */
static void notice_ok(void) {
  void (*ok)(void) = U.n.ok;
  U.goAt = app_now();
  U.pf = 0;
  pill_hide();
  U.n.ok = NULL;
  if (ok)
    ok();
}
void update_notice(const char *label, const char *badge, void (*ok)(void)) {
  snprintf(U.n.lb, sizeof U.n.lb, "%s", label);
  snprintf(U.n.badge, sizeof U.n.badge, "%s", badge);
  U.n.ok = ok;
  U.n.pend = 1; /* shown by update_tick once the capsule is free */
}
void update_notice_cancel(void (*ok)(void)) {
  if (U.n.ok != ok)
    return;
  U.n.pend = 0;
  U.n.ok = NULL;
  if (U.kind == 1 && U.pill) {
    U.pf = 0;
    pill_hide();
  }
}
int update_pill_key(int k, int onSearch) {
  if (U.pf) {
    if (k == K_OK) {
      if (U.kind == 1)
        notice_ok();
      else
        update_open();
      return 1;
    }
    U.pf = 0;
    U.hideAt = app_now() + 4;
    if (k == K_RIGHT || k == K_UP)
      return 1; /* Right: back onto Search */
    return 0; /* Left (the profile panel) and Down (the page) go on from Search */
  }
  if (k == K_LEFT && U.pill && onSearch) {
    U.pf = 1;
    U.hideAt = 0;
    return 1;
  }
  return 0;
}
static void pill_click(int a, int b) {
  (void)a;
  (void)b;
  if (U.kind == 1)
    notice_ok();
  else
    update_open();
}
static const char *pill_lb(void) {
  return U.kind == 1 ? U.n.lb : T("Update available");
}
static const char *pill_badge(void) {
  return U.kind == 1 ? U.n.badge : U.avail.ver;
}
/* the capsule: the words, for an update its version (plain), then the remote's red key drawn as on the remote: the key
 * that takes you there */
#define KEY_W 46.0f
#define KEY_H 26.0f
static float ver_w(void) {
  return U.kind == 1 ? 0 : 12 + text_width(pill_badge(), 21, 600, 0.2f);
}
static float pill_w(void) {
  return 92 + text_width(pill_lb(), 23, 600, 0.1f) + ver_w() + 16 + KEY_W + 26;
}
void update_pill_draw(float setX) {
  float a = U.pa.v;
  if (a <= 0.001f || (U.kind == 0 && !U.has))
    return;
  const char *lb = pill_lb(), *ver = pill_badge();
  int rtl = app_rtl(), f = U.pf;
  float tw = text_width(lb, 23, 600, 0.1f), vw = ver_w();
  float W = pill_w(), h = 60, y = 26, dx = -36 * (1 - ease_apply(EASE_ROW, U.slide.v)) * (rtl ? -1 : 1);
  float x0 = (rtl ? setX + 76 - W : setX) + dx;
  if (U.pill && a > 0.5f)
    hit_act(rtl ? x0 : setX + 76, y, W - 76, h, pill_click, 0, 0); /* beside the icon: the icon keeps its own clicks */
  /* flat, as the bar: a dark capsule out of the picture; with the highlight, the bar's light pill and dark words */
  gfx_opacity_push(a);
  gfx_shadow(x0, y + 10, W, h, 30, 24, BLACK_A(80));
  double now = app_now();
  float go = U.goAt > 0 && now - U.goAt < 0.5 ? 1 - (float)((now - U.goAt) / 0.5)
                                              : 0; /* OK: it lights up as it takes you there */
  gfx_rrect(x0, y, W, h, 30, f || go > 0 ? RGB(0xd9, 0xd9, 0xd9) : RGBA(32, 32, 34, 240));
  uint32_t ink = f || go > 0 ? RGB(0x14, 0x14, 0x14) : WHITE_A(235);
  float tx = rtl ? x0 + W - 92 : x0 + 92, kx = rtl ? x0 + 26 : x0 + W - 26 - KEY_W, ky = 56 - KEY_H / 2;
  text_draw(lb, tx, 56 - text_line_height(23) / 2.0f, 23, 600, ink, 0.1f, 0, rtl ? ALIGN_RIGHT : ALIGN_LEFT);
  if (vw > 0)
    text_draw(ver, rtl ? tx - tw - 12 : tx + tw + 12, 56 - text_line_height(21) / 2.0f, 21, 600, col_alpha(ink, 0.72f),
              0.2f, 0, rtl ? ALIGN_RIGHT : ALIGN_LEFT);
  if (!plat_color_keys())
    key_hint(kx + KEY_W / 2 - 18, 56, K_OK, NULL, WHITE_A(235),
             0); /* no red key on this remote: OK, once the notice has the highlight */
  else {
    gfx_rrect(kx, ky, KEY_W, KEY_H, 8, RGB(0xef, 0x3b, 0x3b)); /* the red key, as on the remote: a block, two dots */
    gfx_circle(kx + KEY_W / 2 - 7, ky + KEY_H / 2, 2.6f, RGBA(90, 10, 10, 170));
    gfx_circle(kx + KEY_W / 2 + 7, ky + KEY_H / 2, 2.6f, RGBA(90, 10, 10, 170));
  }
  gfx_opacity_pop();
}

void update_tick(float dt) {
  tween_tick(&U.pa, dt);
  tween_tick(&U.slide, dt);
  tween_tick(&U.dot, dt);
  plat_luna_pump();
  if (U.room && cache_room_done()) {
    U.room = 0;
    if (U.state == UPD_INSTALLING)
      install_go();
  }
#if SB_PLAT_INSTALLER
  if (U.state == UPD_INSTALLING && !U.room) {
    char m[240];
    int st = plat_install_poll(m, sizeof m);
    if (st < 0) {
      U.state = UPD_AVAIL;
      snprintf(U.msg, sizeof U.msg, "%s", T1("The update failed: {e}", "e", m));
      fprintf(stderr, "[update] install failed: %s\n", m);
      toast(T("The update failed"), 2500, NULL);
      changed();
    } else if (m[0] && strcmp(m, U.msg)) {
      snprintf(U.msg, sizeof U.msg, "%s", m);
      fprintf(stderr, "[update] %s\n", m);
      changed();
    }
  }
#endif
  if (U.pf && !topNav.active) {
    U.pf = 0;
    U.hideAt = app_now() + 4;
  } /* the highlight left the top bar */
  if (U.pill && U.hideAt && app_now() >= U.hideAt)
    pill_hide();
  if (U.n.pend && !U.pill && U.pa.v <= 0.001f && !player_open()) { /* the other notice, once the capsule is back in */
    U.n.pend = 0;
    U.kind = 1;
    U.pill = 1;
    U.shown++;
    U.shownAt = app_now();
    U.goAt = 0;
    U.hideAt = app_now() + 10;
    tween_to(&U.pa, 1, 0.3f, EASE_IN_OUT);
    tween_to(&U.slide, 1, 0.55f, EASE_LINEAR);
    tween_to(&U.dot, 1, 0.2f, EASE_IN_OUT);
  }
  /* the check, once the start is over and calm: 8 s in, the splash gone, no film, no key for a moment */
  /* every start checks (one small request), then every 6 hours while the app stays open, 10 minutes after a check
     that got no answer; each when nothing plays and nothing is being pressed */
  if (!U.started && app_now() > 8 && app_splash_done && !player_open() && app_idle() > 1) {
    U.started = 1;
    nextAt = app_now() + 6 * 3600;
    forceCheck = 1;
    update_check(0);
    forceCheck = 0;
  } else if (U.started && nextAt && app_now() >= nextAt && !player_open() && app_idle() > 1 &&
             U.state != UPD_CHECKING && U.state != UPD_INSTALLING) {
    nextAt = app_now() + 6 * 3600;
    forceCheck = 1;
    update_check(0);
    forceCheck = 0;
  }
  if (pillPend && !player_open() && !U.pill && U.pa.v <= 0.001f) {
    pillPend = 0;
    pill_show();
  }
  {
    static int autoI = -1;
    if (autoI < 0)
      autoI = getenv("SB_UPD_AUTOINSTALL") !=
              NULL; /* development: Install pressed 3 s after the notice (the whole update, untouched) */
    if (autoI && U.has && U.state == UPD_AVAIL && U.shown && app_now() - U.shownAt > 3) {
      autoI = 0;
      U.openAt = 0;
      update_install();
    }
  }
}
