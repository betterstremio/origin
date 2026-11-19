/* See migrate.h. What is copied is what the 2.1.12 export provider offers: the regular files at the top of the old data
   folder (state.json, collections.json, the tmdb and trakt keys) and the .json files in acct/ (sign-ins and profiles).
   Not copied: logs, temporary files, the .bin caches, the relaunch scripts, anything over 20 MB, and every folder
   (cache and engine rebuild themselves). Every file goes to <name>.tmp and is renamed, state.json last, so its
   presence means the rest landed. Upstream's Stremio Origin keeps the old id: its data is never touched. */
#include "core/migrate.h"
#include "platform/plat.h"
#include "core/json.h"
#include <SDL2/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <jni.h>
#endif

#define OLD_ID "tv.stremioorigin.app"
#define MAX_BYTES (20L << 20)

static long gBytes;
static int gFiles;

static int exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}
static int ends(const char *s, const char *suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && !strcmp(s + a - b, suf);
}
static int wanted(const char *name, int acct) {
  char n[300];
  size_t i = 0;
  for (; name[i] && i < sizeof n - 1; i++)
    n[i] = (char)tolower((unsigned char)name[i]);
  n[i] = 0;
  if (ends(n, ".tmp") || strstr(n, ".tmp.") || ends(n, ".log") || ends(n, ".bin"))
    return 0;
  if (acct)
    return ends(n, ".json");
  return strncmp(n, "relaunch.", 9) && strncmp(n, "applog", 6);
}
/* one file, through <dst>.tmp; 1 when it landed */
static int copy_file(const char *src, const char *dst) {
  char tmp[1200];
  snprintf(tmp, sizeof tmp, "%s.tmp", dst);
  FILE *in = fopen(src, "rb"), *out = in ? fopen(tmp, "wb") : NULL;
  long total = 0;
  int ok = in && out;
  if (ok) {
    char buf[32768];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, in)) > 0) {
      if (fwrite(buf, 1, r, out) != r) {
        ok = 0;
        break;
      }
      total += (long)r;
    }
    if (ferror(in))
      ok = 0;
  }
  if (in)
    fclose(in);
  if (out && fclose(out) != 0)
    ok = 0;
  if (ok) {
#ifdef _WIN32
    remove(dst); /* rename will not replace there */
#endif
    ok = rename(tmp, dst) == 0;
  }
  if (!ok) {
    remove(tmp);
    return 0;
  }
  gBytes += total;
  gFiles++;
  return 1;
}
/* the files of one folder; state.json (top level) is left for the end */
static void copy_dir(const char *from, const char *to, int acct) {
  DIR *d = opendir(from);
  if (!d)
    return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!wanted(e->d_name, acct) || (!acct && !strcmp(e->d_name, "state.json")))
      continue;
    char s[1200], t[1200];
    snprintf(s, sizeof s, "%s/%s", from, e->d_name);
    snprintf(t, sizeof t, "%s/%s", to, e->d_name);
    struct stat st;
    if (stat(s, &st) || !S_ISREG(st.st_mode) || st.st_size > MAX_BYTES)
      continue;
    if (!copy_file(s, t))
      fprintf(stderr, "[migrate] could not copy %s\n", s);
  }
  closedir(d);
}
/* a value in state.json that names the old app (its id, a path inside its data folder) would point at what is about to
   go: those keys are dropped, the app sets them again. Nothing in 2.1.x writes such a value today; this is the safety
   net. Rewritten through a temporary file, so state.json is whole at every moment */
static void sanitize_state(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = n > 0 ? malloc((size_t)n + 1) : NULL;
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(b);
    return;
  }
  fclose(f);
  b[n] = 0;
  if (!strstr(b, OLD_ID)) {
    free(b);
    return;
  }
  cJSON *j = cJSON_Parse(b);
  free(b);
  if (!j)
    return;
  int dropped = 0;
  for (cJSON *c = j->child, *nx; c; c = nx) {
    nx = c->next;
    if (cJSON_IsString(c) && c->valuestring && strstr(c->valuestring, OLD_ID)) {
      fprintf(stderr, "[migrate] state.json: dropped %s (names the old app)\n", c->string);
      cJSON_DeleteItemFromObject(j, c->string);
      dropped++;
    }
  }
  if (dropped) {
    char *out = cJSON_PrintUnformatted(j), tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *o = out ? fopen(tmp, "wb") : NULL;
    if (o) {
      int ok = fwrite(out, 1, strlen(out), o) == strlen(out);
      ok = fclose(o) == 0 && ok;
#ifdef _WIN32
      if (ok)
        remove(path);
#endif
      if (!ok || rename(tmp, path))
        remove(tmp);
    }
    free(out);
  }
  cJSON_Delete(j);
}
static void mark(const char *newDir) {
  char p[1200];
  snprintf(p, sizeof p, "%s/imported", newDir);
  FILE *f = fopen(p, "w");
  if (f) {
    fputs("1\n", f);
    fclose(f);
  }
}
/* the whole import from a readable folder; 1 when state.json landed */
static int import_from(const char *old, const char *newDir) {
  char a[1200], b[1200], s[1200], t[1200];
  snprintf(a, sizeof a, "%s/acct", old);
  snprintf(b, sizeof b, "%s/acct", newDir);
  mkdir(b, 0755);
  copy_dir(old, newDir, 0);
  copy_dir(a, b, 1);
  snprintf(s, sizeof s, "%s/state.json", old);
  snprintf(t, sizeof t, "%s/state.json", newDir);
  struct stat st;
  if (stat(s, &st) || st.st_size > MAX_BYTES || !copy_file(s, t))
    return 0;
  sanitize_state(t);
  return 1;
}
#ifdef _WIN32
static void rmtree(const char *p) {
  struct stat st;
  if (stat(p, &st))
    return;
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(p);
    if (d) {
      struct dirent *e;
      while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
          continue;
        char c[1200];
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        rmtree(c);
      }
      closedir(d);
    }
    rmdir(p);
  } else
    remove(p);
}
#endif

/* ---- webOS ---- */
#if defined(SB_MIGRATE_WEBOS)
static const char *const oldDirs[3] = { "/media/developer/apps/usr/palm/data/" OLD_ID, "/media/developer/." OLD_ID,
                                        "/media/internal/." OLD_ID };
static const char *const infos[2] = { "/media/developer/apps/usr/palm/applications/" OLD_ID "/appinfo.json",
                                      "/media/cryptofs/apps/usr/palm/applications/" OLD_ID "/appinfo.json" };
/* -1: no appinfo readable (nothing imported), 0: not ours (upstream), 1: ours */
static int webos_ours(void) {
  for (int i = 0; i < 2; i++) {
    FILE *f = fopen(infos[i], "rb");
    if (!f)
      continue;
    char b[16384];
    size_t n = fread(b, 1, sizeof b - 1, f);
    fclose(f);
    b[n] = 0;
    cJSON *j = cJSON_Parse(b);
    if (!j)
      continue;
    cJSON *ti = cJSON_GetObjectItemCaseSensitive(j, "title"), *ve = cJSON_GetObjectItemCaseSensitive(j, "version");
    int maj = 0, mi = 0, pa = 0;
    int ok = cJSON_IsString(ti) && ti->valuestring && !strcmp(ti->valuestring, "Origin") && cJSON_IsString(ve) &&
             ve->valuestring && sscanf(ve->valuestring, "%d.%d.%d", &maj, &mi, &pa) >= 1 && maj >= 2;
    fprintf(stderr, "[migrate] old app: title \"%s\" version %s: %s\n",
            cJSON_IsString(ti) && ti->valuestring ? ti->valuestring : "?",
            cJSON_IsString(ve) && ve->valuestring ? ve->valuestring : "?", ok ? "ours" : "not ours, left alone");
    cJSON_Delete(j);
    return ok;
  }
  return -1;
}
/* a command run as root through the Homebrew Channel; the reply (malloc'd) or NULL */
static char *hb_exec(const char *cmd, int ms) {
  cJSON *p = cJSON_CreateObject();
  cJSON_AddStringToObject(p, "command", cmd);
  char *ps = cJSON_PrintUnformatted(p);
  cJSON_Delete(p);
  char *r = plat_luna_call("luna://org.webosbrew.hbchannel.service/exec", ps, ms);
  free(ps);
  return r;
}
static int webos_import_root(const char *newDir) {
  /* the same file set as copy_dir (the name lower-cased for the test), state.json last, then the new folder's owner */
  char cmd[3600];
  snprintf(
      cmd, sizeof cmd,
      "N='%s'; D=; for c in %s %s %s; do [ -f \"$c/state.json\" ] && D=$c && break; done; [ -n \"$D\" ] || exit 3; "
      "mkdir -p \"$N/acct\"; "
      "cp1() { [ \"$(wc -c < \"$1\")\" -le 20971520 ] && cp \"$1\" \"$2.tmp\" && mv \"$2.tmp\" \"$2\"; }; "
      "for f in \"$D\"/*; do [ -f \"$f\" ] || continue; b=${f##*/}; l=$(echo \"$b\" | tr A-Z a-z); "
      "case \"$l\" in *.tmp|*.tmp.*|*.log|*.bin|relaunch.*|applog*|state.json) continue;; esac; cp1 \"$f\" \"$N/$b\"; "
      "done; "
      "for f in \"$D\"/acct/*.json; do [ -f \"$f\" ] || continue; b=${f##*/}; l=$(echo \"$b\" | tr A-Z a-z); "
      "case \"$l\" in *.tmp|*.tmp.*) continue;; esac; cp1 \"$f\" \"$N/acct/$b\"; done; "
      "cp1 \"$D/state.json\" \"$N/state.json\"; r=$?; "
      "chown -R $(stat -c %%u:%%g \"$N\") \"$N\" 2>/dev/null; echo \"from=$D rc=$r\"; exit $r",
      newDir, oldDirs[0], oldDirs[1], oldDirs[2]);
  char *r = hb_exec(cmd, 15000);
  fprintf(stderr, "[migrate] root copy through the Homebrew Channel: %s\n", r ? r : "(no answer)");
  free(r);
  char t[1200];
  snprintf(t, sizeof t, "%s/state.json", newDir);
  return exists(t);
}
/* in the background, so startup does not wait: the system's removal of the old app, then its engine and cache folders
   (the small settings files stay in its data folder: the fallback if anything went wrong) */
#define ENG "/" OLD_ID "-engine"
static void webos_remove(void) {
  char cmd[3300];
  snprintf(cmd, sizeof cmd,
           "setsid sh -c '( luna-send -n 1 luna://com.webos.appInstallService/dev/remove \"{\\\"id\\\":\\\"" OLD_ID
           "\\\"}\"; for d in %s %s %s; do rm -rf \"$d/engine\" \"$d/cache\"; done; "
           "rm -rf /tmp" ENG "* /dev/shm" ENG "* /tmp/usb/*" ENG " /tmp/usb/*/*" ENG " /mnt/usb*" ENG " /mnt/usb*/*" ENG
           " /media/usb*" ENG " /media/usb*/*" ENG " /run/media/*" ENG " /run/media/*/*" ENG
           " ) >/dev/null 2>&1 &' >/dev/null 2>&1 </dev/null &",
           oldDirs[0], oldDirs[1], oldDirs[2]);
  char *r = hb_exec(cmd, 2000);
  fprintf(stderr, "[migrate] removal of the old app started: %s\n",
          r ? r : "(Homebrew Channel not answering: the old app stays)");
  free(r);
}
#endif

/* ---- Windows ---- */
#ifdef _WIN32
static void win_remove(const char *old) {
  char p[1200];
  snprintf(p, sizeof p, "%s/engine", old);
  rmtree(p);
  snprintf(p, sizeof p, "%s/cache", old);
  rmtree(p);
  const char *tmp = getenv("TEMP");
  if (tmp) { /* where p2p.c would have put the engine's temporary folder under the old id */
    snprintf(p, sizeof p, "%s/" OLD_ID "-engine", tmp);
    rmtree(p);
  }
  fprintf(stderr, "[migrate] old app: engine and cache folders deleted (its settings stay as the fallback)\n");
}
#endif

/* ---- Android: the old app's provider, over JNI (packaging/android/.../SbMigrate.java) ---- */
#ifdef __ANDROID__
static int android_import(const char *dst) {
  JNIEnv *e = (JNIEnv *)SDL_AndroidGetJNIEnv();
  if (!e)
    return -1;
  jclass c = (*e)->FindClass(e, "tv/stremioorigin/app/SbMigrate");
  jmethodID m = c ? (*e)->GetStaticMethodID(e, c, "importOld", "(Ljava/lang/String;)I") : NULL;
  if (!m) {
    (*e)->ExceptionClear(e);
    return -1;
  }
  jstring d = (*e)->NewStringUTF(e, dst);
  jint n = (*e)->CallStaticIntMethod(e, c, m, d);
  if ((*e)->ExceptionCheck(e)) {
    (*e)->ExceptionClear(e);
    n = -1;
  }
  (*e)->DeleteLocalRef(e, d);
  (*e)->DeleteLocalRef(e, c);
  return n;
}
static void android_remove(void) {
  JNIEnv *e = (JNIEnv *)SDL_AndroidGetJNIEnv();
  if (!e)
    return;
  jclass c = (*e)->FindClass(e, "tv/stremioorigin/app/SbMigrate");
  jmethodID m = c ? (*e)->GetStaticMethodID(e, c, "removeOld", "()V") : NULL;
  if (!m) {
    (*e)->ExceptionClear(e);
    return;
  }
  (*e)->CallStaticVoidMethod(e, c, m);
  if ((*e)->ExceptionCheck(e)) {
    (*e)->ExceptionDescribe(e);
    (*e)->ExceptionClear(e);
  }
  (*e)->DeleteLocalRef(e, c);
}
#endif

void migrate_run(const char *newDir) {
  char p[1200];
  snprintf(p, sizeof p, "%s/imported", newDir);
  if (exists(p))
    return; /* every normal start: this one stat */
  Uint32 t0 = SDL_GetTicks();
  snprintf(p, sizeof p, "%s/state.json", newDir);
  if (exists(p)) {
    fprintf(stderr, "[migrate] this app already has settings: nothing imported, nothing touched\n");
    mark(newDir);
    return;
  }
  const char *test = getenv("SB_MIGRATE_FROM");
  int got = 0, removeOld = 0;
  const char *from = NULL;
  if (test && *test) {
    fprintf(stderr, "[migrate] test: reading from %s (the old app is not removed)\n", test);
    from = test;
    got = exists(test) && import_from(test, newDir);
  }
#if defined(SB_MIGRATE_WEBOS)
  else {
    int ours = webos_ours();
    if (ours < 0)
      fprintf(stderr, "[migrate] no old app found (no appinfo.json): nothing to import\n");
    if (ours > 0) {
      for (int i = 0; i < 3 && !from; i++)
        if (exists(oldDirs[i]))
          from = oldDirs[i];
      DIR *d = from ? opendir(from) : NULL;
      if (d) {
        closedir(d);
        fprintf(stderr, "[migrate] reading %s directly\n", from);
        got = import_from(from, newDir);
      } else {
        fprintf(stderr, "[migrate] cannot read %s here (%s): copying as root\n", from ? from : "the old data folder",
                from ? strerror(errno) : "not visible");
        from = "root copy";
        got = webos_import_root(newDir);
        if (got)
          sanitize_state(p);
      }
      if (got) {
        removeOld = 1;
        webos_remove();
      } else
        fprintf(stderr, "[migrate] no settings imported: the old app stays\n");
    }
  }
#elif defined(_WIN32)
  else {
    const char *la = getenv("LOCALAPPDATA");
    char old[1200], st[1250];
    snprintf(old, sizeof old, "%s/" OLD_ID, la ? la : ".");
    for (char *c = old; *c; c++)
      if (*c == '\\')
        *c = '/';
    snprintf(st, sizeof st, "%s/state.json", old);
    if (la && exists(st)) {
      from = old;
      fprintf(stderr, "[migrate] reading %s\n", old);
      got = import_from(old, newDir);
      if (got) {
        removeOld = 1;
        win_remove(old);
      }
    } else
      fprintf(stderr, "[migrate] no old app data found: nothing to import\n");
  }
#elif defined(__ANDROID__)
  else {
    int n = android_import(newDir);
    if (n < 0)
      fprintf(stderr, "[migrate] the old app's provider is not there: nothing imported, the old app stays\n");
    else {
      from = "the old app's provider";
      gFiles = n;
      got = exists(p);
      if (got)
        sanitize_state(p);
      if (got) {
        removeOld = 1;
        android_remove();
      }
    }
  }
#endif
  fprintf(stderr, "[migrate] from %s: %d files, %ld bytes, state.json %s, removal %s, %u ms\n", from ? from : "nowhere",
          gFiles, gBytes, got ? "imported" : "not imported", removeOld ? "started" : "not started",
          (unsigned)(SDL_GetTicks() - t0));
  mark(newDir);
}
