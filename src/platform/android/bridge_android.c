#include "platform/plat.h"
#include <SDL2/SDL.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>

static jclass upd;
static jmethodID mStart, mPoll;
static JNIEnv *env(void) {
  return (JNIEnv *)SDL_AndroidGetJNIEnv();
}
static int ready(JNIEnv *e) {
  if (upd)
    return 1;
  if (!e)
    return 0;
  jclass c = (*e)->FindClass(e, "tv/stremioorigin/app/SbUpdater");
  if (!c) {
    (*e)->ExceptionClear(e);
    return 0;
  }
  upd = (*e)->NewGlobalRef(e, c);
  (*e)->DeleteLocalRef(e, c);
  mStart = (*e)->GetStaticMethodID(e, upd, "start", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z");
  mPoll = (*e)->GetStaticMethodID(e, upd, "poll", "()Ljava/lang/String;");
  if (!mStart || !mPoll) {
    (*e)->ExceptionClear(e);
    upd = NULL;
    return 0;
  }
  return 1;
}
int plat_install_start(const char *url, const char *sha256, const char *version) {
  JNIEnv *e = env();
  if (!ready(e))
    return 0;
  jstring a = (*e)->NewStringUTF(e, url), b = (*e)->NewStringUTF(e, sha256), c = (*e)->NewStringUTF(e, version);
  jboolean ok = (*e)->CallStaticBooleanMethod(e, upd, mStart, a, b, c);
  if ((*e)->ExceptionCheck(e)) {
    (*e)->ExceptionDescribe(e);
    (*e)->ExceptionClear(e);
    ok = 0;
  }
  (*e)->DeleteLocalRef(e, a);
  (*e)->DeleteLocalRef(e, b);
  (*e)->DeleteLocalRef(e, c);
  return ok != 0;
}
/* the Java side answers "<state>|<message>" */
int plat_install_poll(char *msg, int cap) {
  msg[0] = 0;
  JNIEnv *e = env();
  if (!ready(e))
    return -1;
  jstring js = (jstring)(*e)->CallStaticObjectMethod(e, upd, mPoll);
  if ((*e)->ExceptionCheck(e)) {
    (*e)->ExceptionClear(e);
    snprintf(msg, (size_t)cap, "internal error");
    return -1;
  }
  const char *s = js ? (*e)->GetStringUTFChars(e, js, NULL) : NULL;
  int st = 0;
  if (s) {
    st = s[0] == '-' ? -1 : s[0] == '1' ? 1 : 0;
    const char *bar = strchr(s, '|');
    snprintf(msg, (size_t)cap, "%s", bar ? bar + 1 : "");
    (*e)->ReleaseStringUTFChars(e, js, s);
  }
  if (js)
    (*e)->DeleteLocalRef(e, js);
  return st;
}

/* ---- phones and tablets: MainActivity's ui* methods (each hops to the UI thread itself) ---- */
static jclass act;
static jmethodID mOri, mImm, mHap, mSafe, mOpen, mBrG, mBrS, mVoG, mVoS, mGest, mDownloadNetwork, mDownloadStatus;
static SDL_atomic_t touchCancelEv; /* the event type, registered once on the app thread (0 not yet, -1 none to be had);
                                      SDL_PushEvent is thread-safe */
static int act_ready(JNIEnv *e) {
  if (act)
    return 1;
  if (!e)
    return 0;
  jclass c = (*e)->FindClass(e, "tv/stremioorigin/app/MainActivity");
  if (!c) {
    (*e)->ExceptionClear(e);
    return 0;
  }
  act = (*e)->NewGlobalRef(e, c);
  (*e)->DeleteLocalRef(e, c);
  mOri = (*e)->GetStaticMethodID(e, act, "uiOrientation", "(I)V");
  mImm = (*e)->GetStaticMethodID(e, act, "uiImmersive", "(I)V");
  mHap = (*e)->GetStaticMethodID(e, act, "uiHaptic", "()V");
  mSafe = (*e)->GetStaticMethodID(e, act, "uiSafeArea", "()Ljava/lang/String;");
  mOpen = (*e)->GetStaticMethodID(e, act, "uiOpenUrl", "(Ljava/lang/String;)Z");
  mBrG = (*e)->GetStaticMethodID(e, act, "uiBrightnessGet", "()I");
  mBrS = (*e)->GetStaticMethodID(e, act, "uiBrightnessSet", "(I)V");
  mVoG = (*e)->GetStaticMethodID(e, act, "uiVolumeGet", "()I");
  mVoS = (*e)->GetStaticMethodID(e, act, "uiVolumeSet", "(I)I");
  mGest = (*e)->GetStaticMethodID(e, act, "uiGestureInsets", "()Ljava/lang/String;");
  mDownloadNetwork = (*e)->GetStaticMethodID(e, act, "uiDownloadNetwork", "(Z)Z");
  mDownloadStatus = (*e)->GetStaticMethodID(e, act, "uiDownloadStatus", "(IZ)Z");
  if (!mOri || !mImm || !mHap || !mSafe || !mOpen || !mBrG || !mBrS || !mVoG || !mVoS || !mGest || !mDownloadNetwork ||
      !mDownloadStatus) {
    (*e)->ExceptionClear(e);
    (*e)->DeleteGlobalRef(e, act);
    act = NULL;
    return 0;
  }
  return 1;
}
int plat_download_network(int wifiOnly) {
  JNIEnv *e = env();
  if (!act_ready(e))
    return 0;
  jboolean allowed = (*e)->CallStaticBooleanMethod(e, act, mDownloadNetwork, wifiOnly ? JNI_TRUE : JNI_FALSE);
  if ((*e)->ExceptionCheck(e)) {
    (*e)->ExceptionClear(e);
    return 0;
  }
  return allowed != 0;
}
int plat_download_status(int progress, int visible) {
  JNIEnv *e = env();
  if (!act_ready(e))
    return 0;
  jboolean running =
      (*e)->CallStaticBooleanMethod(e, act, mDownloadStatus, (jint)progress, visible ? JNI_TRUE : JNI_FALSE);
  if ((*e)->ExceptionCheck(e)) {
    (*e)->ExceptionClear(e);
    return 0;
  }
  return running != 0;
}
static void call_int(jmethodID m, int v) {
  JNIEnv *e = env();
  if (!act_ready(e))
    return;
  (*e)->CallStaticVoidMethod(e, act, m, (jint)v);
  if ((*e)->ExceptionCheck(e))
    (*e)->ExceptionClear(e);
}
void plat_orientation(int landscape) {
  if (act_ready(env()))
    call_int(mOri, landscape);
}
void plat_immersive(int on) {
  if (act_ready(env()))
    call_int(mImm, on);
}
void plat_haptic(void) {
  JNIEnv *e = env();
  if (!act_ready(e))
    return;
  (*e)->CallStaticVoidMethod(e, act, mHap);
  if ((*e)->ExceptionCheck(e))
    (*e)->ExceptionClear(e);
}
void plat_safe_area(int *top, int *bottom, int *left, int *right) {
  *top = *bottom = *left = *right = 0;
  JNIEnv *e = env();
  if (!act_ready(e))
    return;
  jstring js = (jstring)(*e)->CallStaticObjectMethod(e, act, mSafe);
  if ((*e)->ExceptionCheck(e)) {
    (*e)->ExceptionClear(e);
    return;
  }
  const char *s = js ? (*e)->GetStringUTFChars(e, js, NULL) : NULL;
  if (s) {
    sscanf(s, "%d,%d,%d,%d", top, bottom, left, right);
    (*e)->ReleaseStringUTFChars(e, js, s);
  }
  if (js)
    (*e)->DeleteLocalRef(e, js);
}
int plat_open_url(const char *url) {
  JNIEnv *e = env();
  if (!act_ready(e))
    return 0;
  jstring a = (*e)->NewStringUTF(e, url);
  jboolean ok = (*e)->CallStaticBooleanMethod(e, act, mOpen, a);
  if ((*e)->ExceptionCheck(e)) {
    (*e)->ExceptionClear(e);
    ok = 0;
  }
  (*e)->DeleteLocalRef(e, a);
  return ok != 0;
}
static int call_ret_int(jmethodID m, int arg, int hasArg, int fail) {
  JNIEnv *e = env();
  if (!act_ready(e))
    return fail;
  jint r = hasArg ? (*e)->CallStaticIntMethod(e, act, m, (jint)arg) : (*e)->CallStaticIntMethod(e, act, m);
  if ((*e)->ExceptionCheck(e)) {
    (*e)->ExceptionClear(e);
    return fail;
  }
  return (int)r;
}
int plat_brightness_get(void) {
  return call_ret_int(mBrG, 0, 0, -1);
}
int plat_brightness_set(int pct) {
  if (!act_ready(env()))
    return 0;
  call_int(mBrS, pct);
  return 1;
}
int plat_sysvol_get(void) {
  return call_ret_int(mVoG, 0, 0, -1);
}
int plat_sysvol_set(int pct) {
  return call_ret_int(mVoS, pct, 1, 0);
}
void plat_gesture_insets(int *left, int *top, int *right, int *bottom) {
  *left = *top = *right = *bottom = 0;
  JNIEnv *e = env();
  if (!act_ready(e))
    return;
  jstring js = (jstring)(*e)->CallStaticObjectMethod(e, act, mGest);
  if ((*e)->ExceptionCheck(e)) {
    (*e)->ExceptionClear(e);
    return;
  }
  const char *s = js ? (*e)->GetStringUTFChars(e, js, NULL) : NULL;
  if (s) {
    sscanf(s, "%d,%d,%d,%d", left, top, right, bottom);
    (*e)->ReleaseStringUTFChars(e, js, s);
  }
  if (js)
    (*e)->DeleteLocalRef(e, js);
}
/* MainActivity.dispatchTouchEvent calls this on the UI thread before SDL turns an ACTION_CANCEL into lifts */
JNIEXPORT void JNICALL Java_tv_stremioorigin_app_MainActivity_nativeTouchCancel(JNIEnv *e, jclass c) {
  (void)e;
  (void)c;
  int t = SDL_AtomicGet(&touchCancelEv); /* (read once: the SDL thread publishes it) */
  if (t <= 0)
    return;
  SDL_Event ev;
  SDL_zero(ev);
  ev.type = (Uint32)t;
  SDL_PushEvent(&ev);
}
unsigned plat_touch_cancel_event(void) {
  int t = SDL_AtomicGet(&touchCancelEv);
  if (!t) {
    Uint32 r = SDL_RegisterEvents(1);
    t = r == (Uint32)-1 ? -1 : (int)r;
    SDL_AtomicSet(&touchCancelEv, t);
  }
  return t > 0 ? (unsigned)t : 0;
}
