/* Windows: SDL keeps the window and the input. The GL surface is made here, not by SDL: ANGLE draws the UI on Direct3D
   11 into a DirectComposition layer that floats above the video's child window (video_win.c), so where the UI is
   transparent the video shows through, as the TVs' video plane does. Settings and caches live in
   %LOCALAPPDATA%\<app id>; the bundled assets beside the exe. */
#include "platform/plat.h"
#include "core/keys.h"
#include "core/applog.h"
#include <SDL2/SDL_syswm.h>
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <io.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char assetDir[1024], dataDir[1024], devName[96], locale[32];
static HWND top;

static void utf8(const wchar_t *w, char *out, int cap) { if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) out[0] = 0; }
static void slashes(char *p) { for (; *p; p++) if (*p == '\\') *p = '/'; }

/* A program built for the Windows subsystem and started from Explorer has no stderr. The kept log copies fd 2
   (applog.c), and everything writes to stderr: point fds 1 and 2 at NUL first, then the log takes over fd 2. */
static void std_streams(void) {
  HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
  if (e && e != INVALID_HANDLE_VALUE) { _setmode(2, _O_BINARY); return; } /* started from a console or redirected: keep it */
  int nul = _open("NUL", _O_WRONLY | _O_BINARY); if (nul < 0) return;
  if (nul != 1) _dup2(nul, 1); if (nul != 2) _dup2(nul, 2); if (nul > 2) _close(nul);
  if (_fileno(stderr) != 2) { FILE *f = freopen("NUL", "wb", stderr); (void)f; }
}

/* A crash: the exception, then where, as module+offset for each frame of the unwound stack (resolve the exe's with
   addr2line -e Origin.dbg.exe 0x140000000+offset), written straight to the first stderr and to <data>/crash.log, then
   handed on (the C library's handler, which raises SIGSEGV for applog.c, then Windows Error Reporting). */
static HANDLE crashErr; static wchar_t crashPath[1100]; static LPTOP_LEVEL_EXCEPTION_FILTER prevFilter;
static int where(char *b, int cap, DWORD64 pc) {
  HMODULE m = NULL; wchar_t mn[MAX_PATH] = L"?";
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)pc, &m)) GetModuleFileNameW(m, mn, MAX_PATH);
  const wchar_t *s = wcsrchr(mn, L'\\'); return snprintf(b, (size_t)cap, " %ls+0x%llx", s ? s + 1 : mn, (unsigned long long)(pc - (DWORD64)(uintptr_t)m));
}
static LONG WINAPI on_fault(EXCEPTION_POINTERS *x) {
  static volatile LONG once; if (InterlockedExchange(&once, 1)) return EXCEPTION_CONTINUE_SEARCH;
  char b[1600]; int n = snprintf(b, sizeof b, "[crash] exception %08lx thread %lu at", (unsigned long)x->ExceptionRecord->ExceptionCode, (unsigned long)GetCurrentThreadId());
  CONTEXT c = *x->ContextRecord;
  for (int i = 0; i < 16 && c.Rip && n < (int)sizeof b - 80; i++) {
    n += where(b + n, (int)sizeof b - n, c.Rip);
    DWORD64 base; PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, NULL);
    if (!f) { c.Rip = *(DWORD64 *)(uintptr_t)c.Rsp; c.Rsp += 8; continue; } /* a leaf function */
    void *hd; DWORD64 fr; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &hd, &fr, NULL); }
  b[n++] = '\n'; DWORD w;
  if (crashErr && crashErr != INVALID_HANDLE_VALUE) WriteFile(crashErr, b, (DWORD)n, &w, NULL);
  HANDLE h = CreateFileW(crashPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
  if (h != INVALID_HANDLE_VALUE) { WriteFile(h, b, (DWORD)n, &w, NULL); CloseHandle(h); }
  return prevFilter ? prevFilter(x) : EXCEPTION_CONTINUE_SEARCH;
}

void plat_pre_init(const char *appId) {
  _fmode = _O_BINARY; /* files read and written byte for byte, never with line ends translated */
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); /* (the manifest says so too) */
  std_streams();
  crashErr = GetStdHandle(STD_ERROR_HANDLE); prevFilter = SetUnhandledExceptionFilter(on_fault);
  WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
  SDL_SetMainReady();
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1"); /* a desktop's monitor may sleep while nothing plays */
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
  const char *a = getenv("SB_ASSETS");
  if (a) snprintf(assetDir, sizeof assetDir, "%s", a);
  else { wchar_t w[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, w, MAX_PATH); char p[1024]; utf8(n ? w : L".", p, sizeof p); slashes(p);
    char *s = strrchr(p, '/'); if (s) *s = 0; snprintf(assetDir, sizeof assetDir, "%s/assets", p); }
  const char *d = getenv("SB_DATA");
  if (d) snprintf(dataDir, sizeof dataDir, "%s", d);
  else { PWSTR la = NULL; char p[900] = "."; if (SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &la) == S_OK) utf8(la, p, sizeof p); CoTaskMemFree(la); slashes(p);
    snprintf(dataDir, sizeof dataDir, "%s/%s", p, appId); }
  mkdir(dataDir, 0755);
  { char cp[1100]; snprintf(cp, sizeof cp, "%s/crash.log", dataDir); MultiByteToWideChar(CP_UTF8, 0, cp, -1, crashPath, 1100); }
  applog_start(dataDir);
  { /* what the screen and decoders can do, for choosing streams (best.c): mpv decodes everything in software if it
       must, so only the screen limits the size; HDR is tone-mapped to SDR for now */
    int lines = GetSystemMetrics(SM_CYSCREEN); char caps[160];
    snprintf(caps, sizeof caps, "maxRes=%d,hdr=0,dv=0,av1=1,dts=1,truehd=1,ac3=1,eac3=1", lines >= 1440 ? 2160 : 1080);
    if (!getenv("SB_TVCAPS")) _putenv_s("SB_TVCAPS", caps); }
  { OSVERSIONINFOW v = { sizeof v }; LONG (WINAPI *rtl)(OSVERSIONINFOW *) = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rtl && !rtl(&v)) snprintf(devName, sizeof devName, "Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    else snprintf(devName, sizeof devName, "Windows"); }
  fprintf(stderr, "[start] %s, data %s, assets %s\n", devName, dataDir, assetDir);
}
static void find_hwnd(SDL_Window *w) { SDL_SysWMinfo wi; SDL_VERSION(&wi.version); if (!top && SDL_GetWindowWMInfo(w, &wi)) top = wi.info.win.window; }
void plat_post_window(SDL_Window *w) {
  find_hwnd(w);
  if (top && getenv("SB_TOPMOST")) SetWindowPos(top, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); /* tests: in view, so a screen capture holds only this window */
}
void plat_cursor_visible(int on) { (void)on; }
const char *plat_asset_dir(void) { return assetDir; }
const char *plat_data_dir(void) { return dataDir; }
/* 16:9, as big as 80 % of the screen's free area allows; SB_WINDOW=1280x720 for tests */
void plat_window_size(int *w, int *h, Uint32 *flags) {
  int ww = 1280, hh = 720; const char *s = getenv("SB_WINDOW");
  if (!(s && sscanf(s, "%dx%d", &ww, &hh) == 2 && ww > 100 && hh > 100)) {
    SDL_Rect ub; if (SDL_GetDisplayUsableBounds(0, &ub) == 0) { ww = ub.w * 8 / 10; hh = ww * 9 / 16; if (hh > ub.h * 8 / 10) { hh = ub.h * 8 / 10; ww = hh * 16 / 9; } } }
  *w = ww; *h = hh; *flags = SDL_WINDOW_ALLOW_HIGHDPI; /* (no SDL_WINDOW_OPENGL: the surface is ANGLE's, made in plat_gl_create) */
}
int plat_map_key(const SDL_KeyboardEvent *e) {
  switch (e->keysym.sym) {
    case SDLK_LEFT: return K_LEFT; case SDLK_RIGHT: return K_RIGHT; case SDLK_UP: return K_UP; case SDLK_DOWN: return K_DOWN;
    case SDLK_RETURN: case SDLK_KP_ENTER: return K_OK;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_AC_BACK: return K_BACK;
    case SDLK_SPACE: case SDLK_AUDIOPLAY: return K_PLAYPAUSE;
    case SDLK_AUDIOSTOP: return K_STOP;
    case SDLK_F1: return K_RED; case SDLK_F2: return K_GREEN; case SDLK_F3: return K_YELLOW; case SDLK_F4: return K_BLUE;
    case SDLK_COMMA: case SDLK_AUDIOREWIND: return K_REW; case SDLK_PERIOD: case SDLK_AUDIOFASTFORWARD: return K_FF;
    case SDLK_PAGEUP: return K_CH_UP; case SDLK_PAGEDOWN: return K_CH_DOWN;
    default: return K_NONE;
  }
}
const char *plat_device_name(void) { return devName; }
int plat_text_input(void) { return getenv("SB_NO_TEXTINPUT") ? 0 : 1; } /* a keyboard types; SB_NO_TEXTINPUT=1 shows the TV's own */
void plat_thread_background(int nice) {
  if (nice > 0) SetThreadPriority(GetCurrentThread(), nice >= 10 ? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_BELOW_NORMAL);
}
int plat_mem_total_mb(void) {
  if (getenv("SB_MEMMB")) return atoi(getenv("SB_MEMMB")); /* development: act like a small device */
  MEMORYSTATUSEX m; m.dwLength = sizeof m; return GlobalMemoryStatusEx(&m) ? (int)(m.ullTotalPhys >> 20) : 0;
}
/* minimised and back stands in for the TV sending the app away and back; focus moving to another window is not that */
int plat_lifecycle(const SDL_Event *e) {
  if (e->type == SDL_WINDOWEVENT) { if (e->window.event == SDL_WINDOWEVENT_RESTORED) return 1; if (e->window.event == SDL_WINDOWEVENT_MINIMIZED) return -1; }
  return 0;
}
const char *plat_ui_locale(void) {
  if (!locale[0]) { wchar_t w[LOCALE_NAME_MAX_LENGTH]; if (GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH)) utf8(w, locale, sizeof locale); }
  return locale[0] ? locale : NULL;
}
char *plat_luna_call(const char *uri, const char *payload, int timeoutMs) { (void)uri; (void)payload; (void)timeoutMs; return NULL; }
unsigned long plat_luna_subscribe(const char *uri, const char *payload, PlatLunaCb cb, void *ud) { (void)uri; (void)payload; (void)cb; (void)ud; return 0; }
void plat_luna_cancel(unsigned long handle) { (void)handle; }
void plat_luna_pump(void) {}
int plat_luna_send(const char *uri, const char *payload) { (void)uri; (void)payload; return 0; }
int plat_panel_lines(void) { return GetSystemMetrics(SM_CYSCREEN); }
int plat_color_keys(void) { return 1; } /* F1-F4 stand in for them */
int plat_random(void *buf, int n) { return n >= 0 && BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0; }
/* the installer, downloaded and run silently: phase 5 of the Windows plan; until then Settings says the update failed */
int plat_install_start(const char *url, const char *sha256, const char *version) { (void)url; (void)sha256; fprintf(stderr, "[update] %s: installing is not built yet on Windows\n", version); return 0; }
int plat_install_poll(char *msg, int cap) { snprintf(msg, (size_t)cap, "not built yet"); return -1; }

/* ---- the GL surface: ANGLE on Direct3D 11, as a DirectComposition layer ----
   EGL is looked up at run time from libEGL.dll beside the exe (only the names used here; the GLES2 calls are linked). */
typedef void *EGLDisplay, *EGLSurface, *EGLContext, *EGLConfig; typedef int EGLint; typedef unsigned EGLBoolean;
static struct {
  EGLDisplay (*GetPlatformDisplayEXT)(unsigned, void *, const EGLint *); EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
  EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *); EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
  EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *); EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
  EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface); EGLBoolean (*SwapInterval)(EGLDisplay, EGLint); const char *(*QueryString)(EGLDisplay, EGLint);
  EGLint (*GetError)(void); void *(*GetProcAddress)(const char *); EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
  EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext); EGLBoolean (*Terminate)(EGLDisplay);
} E;
static EGLDisplay dpy; static EGLSurface surf; static EGLContext ctxt;
enum { EGL_NONE_ = 0x3038, EGL_EXTENSIONS_ = 0x3055, EGL_PLATFORM_ANGLE_ANGLE = 0x3202, EGL_PLATFORM_ANGLE_TYPE_ANGLE = 0x3203,
       EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE = 0x3208, EGL_DIRECT_COMPOSITION_ANGLE = 0x33A5 };
void *plat_gl_create(SDL_Window *w) {
  find_hwnd(w); if (!top) { fprintf(stderr, "[gl] no window handle: %s\n", SDL_GetError()); return NULL; }
  wchar_t p[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, p, MAX_PATH); wchar_t *s = n ? wcsrchr(p, L'\\') : NULL;
  if (s) wcscpy(s + 1, L"libEGL.dll"); HMODULE egl = LoadLibraryW(s ? p : L"libEGL.dll"); /* (by full path: never another program's copy) */
  if (!egl) { fprintf(stderr, "[gl] libEGL.dll missing beside the exe\n"); return NULL; }
#define L(f, name) if (!(*(FARPROC *)&E.f = GetProcAddress(egl, name))) { fprintf(stderr, "[gl] %s missing\n", name); return NULL; }
  L(GetProcAddress, "eglGetProcAddress") L(Initialize, "eglInitialize") L(ChooseConfig, "eglChooseConfig") L(CreateWindowSurface, "eglCreateWindowSurface")
  L(CreateContext, "eglCreateContext") L(MakeCurrent, "eglMakeCurrent") L(SwapBuffers, "eglSwapBuffers") L(SwapInterval, "eglSwapInterval")
  L(QueryString, "eglQueryString") L(GetError, "eglGetError") L(DestroySurface, "eglDestroySurface") L(DestroyContext, "eglDestroyContext") L(Terminate, "eglTerminate")
#undef L
  *(void **)&E.GetPlatformDisplayEXT = E.GetProcAddress("eglGetPlatformDisplayEXT");
  if (!E.GetPlatformDisplayEXT) { fprintf(stderr, "[gl] eglGetPlatformDisplayEXT missing\n"); return NULL; }
  const EGLint da[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE, EGL_NONE_ };
  dpy = E.GetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, NULL, da); EGLint ma, mi;
  if (!dpy || !E.Initialize(dpy, &ma, &mi)) { fprintf(stderr, "[gl] Direct3D 11 display failed: %x\n", E.GetError()); return NULL; }
  const EGLint ca[] = { 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3025, 0, 0x3026, 0, 0x3040, 4 /* ES2 */, 0x3033, 4 /* window */, EGL_NONE_ };
  EGLConfig cfg; EGLint nc = 0;
  if (!E.ChooseConfig(dpy, ca, &cfg, 1, &nc) || nc < 1) { fprintf(stderr, "[gl] no RGBA8 config\n"); return NULL; }
  const char *ext = E.QueryString(dpy, EGL_EXTENSIONS_); int comp = ext && strstr(ext, "EGL_ANGLE_direct_composition");
  const EGLint sa[] = { EGL_DIRECT_COMPOSITION_ANGLE, 1, EGL_NONE_ };
  surf = comp ? E.CreateWindowSurface(dpy, cfg, top, sa) : NULL;
  if (!surf) { fprintf(stderr, "[gl] no composition layer (%s): the video will not show through the UI\n", comp ? "refused" : "not offered");
    surf = E.CreateWindowSurface(dpy, cfg, top, NULL); }
  if (!surf) { fprintf(stderr, "[gl] window surface failed: %x\n", E.GetError()); return NULL; }
  const EGLint xa[] = { 0x3098, 2, EGL_NONE_ };
  ctxt = E.CreateContext(dpy, cfg, NULL, xa);
  if (!ctxt || !E.MakeCurrent(dpy, surf, surf, ctxt)) { fprintf(stderr, "[gl] context failed: %x\n", E.GetError()); return NULL; }
  fprintf(stderr, "[gl] ANGLE %d.%d on Direct3D 11, %s\n", ma, mi, comp ? "composition layer" : "plain window surface");
  return ctxt;
}
void plat_gl_swap(SDL_Window *w) { (void)w; E.SwapBuffers(dpy, surf); }
void plat_gl_interval(int n) { if (dpy) E.SwapInterval(dpy, n); }
void plat_gl_size(SDL_Window *w, int *pw, int *ph) { RECT r; (void)w; if (top && GetClientRect(top, &r)) { *pw = r.right - r.left; *ph = r.bottom - r.top; } else { *pw = *ph = 0; } }
void plat_gl_destroy(void *c) {
  (void)c; if (!dpy) return;
  E.MakeCurrent(dpy, NULL, NULL, NULL); if (ctxt) E.DestroyContext(dpy, ctxt); if (surf) E.DestroySurface(dpy, surf); E.Terminate(dpy); dpy = NULL;
}
