#include "platform/plat.h"
#include <SDL2/SDL.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>

static jclass upd; static jmethodID mStart, mPoll;
static JNIEnv *env(void) { return (JNIEnv *)SDL_AndroidGetJNIEnv(); }
static int ready(JNIEnv *e) {
  if (upd) return 1; if (!e) return 0;
  jclass c = (*e)->FindClass(e, "tv/stremioorigin/app/SbUpdater"); if (!c) { (*e)->ExceptionClear(e); return 0; }
  upd = (*e)->NewGlobalRef(e, c); (*e)->DeleteLocalRef(e, c);
  mStart = (*e)->GetStaticMethodID(e, upd, "start", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z");
  mPoll = (*e)->GetStaticMethodID(e, upd, "poll", "()Ljava/lang/String;");
  if (!mStart || !mPoll) { (*e)->ExceptionClear(e); upd = NULL; return 0; }
  return 1;
}
int plat_install_start(const char *url, const char *sha256, const char *version) {
  JNIEnv *e = env(); if (!ready(e)) return 0;
  jstring a = (*e)->NewStringUTF(e, url), b = (*e)->NewStringUTF(e, sha256), c = (*e)->NewStringUTF(e, version);
  jboolean ok = (*e)->CallStaticBooleanMethod(e, upd, mStart, a, b, c);
  if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionDescribe(e); (*e)->ExceptionClear(e); ok = 0; }
  (*e)->DeleteLocalRef(e, a); (*e)->DeleteLocalRef(e, b); (*e)->DeleteLocalRef(e, c);
  return ok != 0;
}
/* the Java side answers "<state>|<message>" */
int plat_install_poll(char *msg, int cap) {
  msg[0] = 0; JNIEnv *e = env(); if (!ready(e)) return -1;
  jstring js = (jstring)(*e)->CallStaticObjectMethod(e, upd, mPoll);
  if ((*e)->ExceptionCheck(e)) { (*e)->ExceptionClear(e); snprintf(msg, (size_t)cap, "internal error"); return -1; }
  const char *s = js ? (*e)->GetStringUTFChars(e, js, NULL) : NULL; int st = 0;
  if (s) { st = s[0] == '-' ? -1 : s[0] == '1' ? 1 : 0; const char *bar = strchr(s, '|'); snprintf(msg, (size_t)cap, "%s", bar ? bar + 1 : ""); (*e)->ReleaseStringUTFChars(e, js, s); }
  if (js) (*e)->DeleteLocalRef(e, js);
  return st;
}
