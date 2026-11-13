/* Cast receiver: a phone on the same network finds the TV (UDP 8766), pairs once with a 4-digit code shown here,
   then drives it over a WebSocket (TCP 8765): play a stream, pause, resume, seek, stop, remote keys; it is told the
   state about once a second. Platform-neutral (webOS, Android TV): the app gives it hooks and calls cast_tick each
   frame. No threads: two non-blocking sockets polled per frame while idle.
   The messages are JSON objects named by "type": the phone sends hello, then pair (the code) once, and later play,
   pause, resume, seek, stop, key, sub, subdelay, audio, tracks?, settings and account; the TV answers each and sends
   its state (what plays, where, paused or not) about once a second. */
#ifndef SB_CAST_H
#define SB_CAST_H

#define CAST_UDP_PORT 8766
#define CAST_WS_PORT 8765

typedef struct {
  const char *url, *lang;
} CastSub;
typedef struct {
  const char *url, *title, *type, *id; /* type "movie" / "series" ("" when not sent), id "tt..." ("" when not sent) */
  const char *poster, *background, *logo; /* the title's pictures as the phone has them ("" when not sent) */
  double pos;
  int season, episode;
  const CastSub *subs;
  int nsubs;
} CastPlay;
enum { CAST_IDLE, CAST_LOADING, CAST_PLAYING, CAST_PAUSED, CAST_ENDED, CAST_ERROR };
enum {
  CAST_K_UP = 1,
  CAST_K_DOWN,
  CAST_K_LEFT,
  CAST_K_RIGHT,
  CAST_K_OK,
  CAST_K_BACK,
  CAST_K_HOME,
  CAST_K_PLAY,
  CAST_K_PAUSE
};
typedef struct {
  const char *plat, *ver; /* "webos" / "androidtv", the app's version */
  const char *(*name)(void); /* the TV's name, as the phone lists it */
  const char *(*get)(const char *key, const char *def); /* the app's kept settings (the TV's id, the phones' tokens) */
  void (*set)(const char *key, const char *value);
  void (*play)(const CastPlay *p);
  void (*cmd)(int cmd, double pos); /* 1 pause, 2 resume, 3 seek, 4 stop */
  void (*key)(int castKey);
  int (*state)(double *pos, double *dur, char *title, int cap); /* CAST_* */
  void (*pairing)(const char *code); /* show the code ("" : take it away) */
  void (*connected)(int phones); /* authorised phones now (0: none) */
  void (*account)(const char *authKey,
                  const char *userJson); /* the phone's account ("" signed out), after ready and on a change */
  void (*pref)(const char *key, const char *value); /* one of the phone's settings (only known keys reach it) */
  char *(*tracks)(void); /* the "tracks" message now (malloc'd JSON), NULL when nothing plays */
  void (*track)(const char *kind, const char *id, int ms); /* the phone's pick: "sub", "audio", "subdelay" */
} CastHooks;

int cast_start(const CastHooks *h); /* 0: the ports could not be opened (the app goes on without casting) */
void cast_tick(void); /* per frame: never blocks */
int cast_phones(void); /* authorised phones connected now */
void cast_stop(void);
#endif
