/* Windows: SDL keeps the window and the input. The GL surface is made here, not by SDL: ANGLE draws the UI on Direct3D
   11 into a DirectComposition layer that floats above the video's child window (video_win.c), so where the UI is
   transparent the video shows through, as the TVs' video plane does. Settings and caches live in
   %LOCALAPPDATA%\<app id>; the bundled assets beside the exe. */
#include "platform/plat.h"
#include "core/keys.h"
#include "core/applog.h"
#include "core/store.h"
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
#include <pthread.h>
#include <curl/curl.h>
int win_sha256_file(const wchar_t *path, char hex[65]); /* (sha_win.c) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char assetDir[1024], dataDir[1024], devName[96], locale[32];
static HWND top;
static SDL_Window *sw; static struct { int x, y, w, h, max, full, have; } G; static int testSize; /* the window, and its saved place */

static void utf8(const wchar_t *w, char *out, int cap) { if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) out[0] = 0; }
static void slashes(char *p) { for (; *p; p++) if (*p == '\\') *p = '/'; }

/* A program built for the Windows subsystem and started from Explorer has no stderr. The kept log copies fd 2
   (applog.c), and everything writes to stderr: point fds 1 and 2 at NUL first, then the log takes over fd 2. */
static void std_streams(void) {
  HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
  if (e && e != INVALID_HANDLE_VALUE) { _setmode(2, _O_BINARY); setvbuf(stderr, NULL, _IONBF, 0); return; } /* (sent to a file, stderr would be fully buffered: the last lines lost when the app is ended) */ /* started from a console or redirected: keep it */
  /* Here fds 0-2 are closed and the C library's stdout and stderr are bound to none (fd -2). freopen binds a stream to
     the lowest free fd, so fill 0 and 1 first: stderr then lands on fd 2, the one the kept log takes over. Without
     this stderr went to another fd and the kept log stayed empty. */
  int fd; while ((fd = _open("NUL", _O_RDWR | _O_BINARY)) >= 0 && fd < 2) {}
  if (fd >= 2) _close(fd); /* (fd 2 itself, opened here, is closed again so stderr can take it) */
  if (_fileno(stdout) != 1) { FILE *f = freopen("NUL", "wb", stdout); (void)f; }
  if (_fileno(stderr) != 2) { FILE *f = freopen("NUL", "wb", stderr); (void)f; }
  if (_fileno(stderr) != 2) { int n = _open("NUL", _O_WRONLY | _O_BINARY); if (n >= 0) { _dup2(n, 2); _close(n); } } /* (last resort: fd 2 at least exists) */
  setvbuf(stderr, NULL, _IONBF, 0); /* reopened on a file it is fully buffered, and Windows has no line buffering: unbuffered, as a console's is */
}

/* A crash: the exception, then where, as module+offset for each frame of the unwound stack (resolve the exe's with
   addr2line -e Origin.dbg.exe 0x140000000+offset), written straight to the first stderr and to <data>/crash.log, then
   handed on (the C library's handler, which raises SIGSEGV for applog.c, then Windows Error Reporting). */
static HANDLE crashErr; static wchar_t crashPath[1100], crashLog[1100]; static LPTOP_LEVEL_EXCEPTION_FILTER prevFilter;
static int where(char *b, int cap, DWORD64 pc) {
  HMODULE m = NULL; wchar_t mn[MAX_PATH] = L"?";
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(uintptr_t)pc, &m)) GetModuleFileNameW(m, mn, MAX_PATH);
  const wchar_t *s = wcsrchr(mn, L'\\'); return snprintf(b, (size_t)cap, " %ls+0x%llx", s ? s + 1 : mn, (unsigned long long)(pc - (DWORD64)(uintptr_t)m));
}
static void crash_write(const CONTEXT *ctx, unsigned long code) {
  static volatile LONG once; if (InterlockedExchange(&once, 1)) return;
  char b[1600]; int n = snprintf(b, sizeof b, "[crash] exception %08lx thread %lu at", code, (unsigned long)GetCurrentThreadId());
  CONTEXT c = *ctx;
  for (int i = 0; i < 16 && c.Rip && n < (int)sizeof b - 80; i++) {
    n += where(b + n, (int)sizeof b - n, c.Rip);
    DWORD64 base; PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, NULL);
    if (!f) { c.Rip = *(DWORD64 *)(uintptr_t)c.Rsp; c.Rsp += 8; continue; } /* a leaf function */
    void *hd; DWORD64 fr; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &hd, &fr, NULL); }
  b[n++] = '\n'; DWORD w;
  if (crashErr && crashErr != INVALID_HANDLE_VALUE) WriteFile(crashErr, b, (DWORD)n, &w, NULL);
  for (int k = 0; k < 2; k++) { /* crash.log, and the kept log (Share logs sends it) */
    HANDLE h = CreateFileW(k ? crashLog : crashPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, b, (DWORD)n, &w, NULL); CloseHandle(h); } }
}
/* mingw's runtime turns a fault in the exe into SIGSEGV (and the like) before Windows' last-chance filter: the vectored
   handler only notes where it happened (a fault caught on purpose costs a copy, nothing more), and the signal, raised
   only when nobody caught it, writes the report from that note (compat_win.c calls win_crash_hook) */
static __thread CONTEXT faultCtx; static __thread unsigned long faultCode; static __thread int faultSeen;
static LONG WINAPI note_fault(EXCEPTION_POINTERS *x) {
  DWORD c = x->ExceptionRecord->ExceptionCode;
  if (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION || c == EXCEPTION_INT_DIVIDE_BY_ZERO || c == EXCEPTION_PRIV_INSTRUCTION ||
      c == EXCEPTION_IN_PAGE_ERROR || c == EXCEPTION_STACK_OVERFLOW || c == EXCEPTION_ARRAY_BOUNDS_EXCEEDED) { faultCtx = *x->ContextRecord; faultCode = c; faultSeen = 1; }
  return EXCEPTION_CONTINUE_SEARCH;
}
void win_crash_hook(int sig) { CONTEXT c; if (faultSeen) crash_write(&faultCtx, faultCode); else { RtlCaptureContext(&c); crash_write(&c, 0xC0000000ul | (unsigned)sig); } }
static LONG WINAPI on_fault(EXCEPTION_POINTERS *x) { /* a fault nobody turned into a signal (a library's own thread) */
  crash_write(x->ContextRecord, x->ExceptionRecord->ExceptionCode);
  return prevFilter ? prevFilter(x) : EXCEPTION_CONTINUE_SEARCH;
}

void plat_pre_init(const char *appId) {
  _fmode = _O_BINARY; /* files read and written byte for byte, never with line ends translated */
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); /* (the manifest says so too) */
  std_streams();
  crashErr = GetStdHandle(STD_ERROR_HANDLE); prevFilter = SetUnhandledExceptionFilter(on_fault); AddVectoredExceptionHandler(1, note_fault);
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
  { char cp[1100]; snprintf(cp, sizeof cp, "%s/crash.log", dataDir); MultiByteToWideChar(CP_UTF8, 0, cp, -1, crashPath, 1100);
    snprintf(cp, sizeof cp, "%s/applog.log", dataDir); MultiByteToWideChar(CP_UTF8, 0, cp, -1, crashLog, 1100); }
  { /* one app per data folder (two would write over each other's settings): a second start brings the first to the front */
    unsigned hsh = 2166136261u; for (const char *c = dataDir; *c; c++) hsh = (hsh ^ (unsigned char)*c) * 16777619u;
    wchar_t mn[64]; _snwprintf(mn, 64, L"Local\\OriginApp-%08x", hsh); mn[63] = 0;
    HANDLE m = CreateMutexW(NULL, TRUE, mn);
    if (m && GetLastError() == ERROR_ALREADY_EXISTS) { HWND o = FindWindowW(L"SDL_app", L"Origin"); if (o) { ShowWindow(o, IsIconic(o) ? SW_RESTORE : SW_SHOW); SetForegroundWindow(o); }
      fprintf(stderr, "[start] already running for %s: brought to the front\n", dataDir); exit(0); } }
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
void *plat_win_hwnd(void) { return top; }
static void find_hwnd(SDL_Window *w) { SDL_SysWMinfo wi; SDL_VERSION(&wi.version); if (!top && SDL_GetWindowWMInfo(w, &wi)) top = wi.info.win.window; }
/* While the border or title bar is dragged, Windows runs its own message loop and the app's stops: the picture would
   freeze until the button is let go. The window's messages are seen here first: from the start of a drag to its end a
   timer, and each new size, draw a frame through the app's callback (main.c). */
static WNDPROC sdlProc; static void (*liveDraw)(void); static int inDrag, dragFrames; enum { LIVE_TIMER = 0x51D };
static void live_now(void) { if (inDrag && liveDraw) { liveDraw(); dragFrames++; } }
static LRESULT CALLBACK live_proc(HWND h, UINT m, WPARAM a, LPARAM b) {
  if (m == WM_ENTERSIZEMOVE) { inDrag = 1; dragFrames = 0; SetTimer(h, LIVE_TIMER, USER_TIMER_MINIMUM, NULL); }
  else if (m == WM_EXITSIZEMOVE) { inDrag = 0; KillTimer(h, LIVE_TIMER); fprintf(stderr, "[window] %d frames drawn while the window was dragged\n", dragFrames); }
  else if (m == WM_TIMER && a == LIVE_TIMER) { live_now(); return 0; }
  LRESULT r = CallWindowProcW(sdlProc, h, m, a, b);
  if (m == WM_SIZE) live_now(); /* (after SDL has heard of the size) */
  return r;
}
void plat_live_draw(void (*fn)(void)) { liveDraw = fn; if (top && !sdlProc) sdlProc = (WNDPROC)SetWindowLongPtrW(top, GWLP_WNDPROC, (LONG_PTR)live_proc); }
void plat_post_window(SDL_Window *w) {
  find_hwnd(w); sw = w;
  SDL_SetWindowMinimumSize(w, 640, 360);
  if (G.have) { SDL_SetWindowPosition(w, G.x, G.y); if (G.max) SDL_MaximizeWindow(w); fprintf(stderr, "[window] as left: %d,%d %dx%d%s%s\n", G.x, G.y, G.w, G.h, G.max ? ", maximised" : "", G.full ? ", fullscreen" : ""); }
  SDL_ShowWindow(w);
  if (G.have && G.full) SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (top && getenv("SB_TOPMOST")) SetWindowPos(top, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); /* tests: in view, so a screen capture holds only this window */
}
void plat_cursor_visible(int on) { SDL_ShowCursor(on ? SDL_ENABLE : SDL_DISABLE); } /* (the app hides it while the arrows are used, shows it when the mouse moves) */
const char *plat_asset_dir(void) { return assetDir; }
const char *plat_data_dir(void) { return dataDir; }
/* ---- the window: where it was left (store key winGeom: "x y w h maximised fullscreen"), else 16:9 at 80 % of the screen's
   free area; SB_WINDOW=1280x720 for tests (centred, never the saved place) ---- */
static int on_a_screen(int x, int y, int w, int h) { /* most of it on some monitor (one unplugged since must not strand it) */
  for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) { SDL_Rect b; if (SDL_GetDisplayUsableBounds(i, &b)) continue;
    int ix = (x > b.x ? x : b.x), iy = (y > b.y ? y : b.y), ax = (x + w < b.x + b.w ? x + w : b.x + b.w), ay = (y + h < b.y + b.h ? y + h : b.y + b.h);
    if (ax - ix >= w / 2 && ay - iy >= h / 2) return 1; }
  return 0;
}
void plat_window_size(int *w, int *h, Uint32 *flags) {
  int ww = 1280, hh = 720; const char *s = getenv("SB_WINDOW"); int test = testSize = s && sscanf(s, "%dx%d", &ww, &hh) == 2 && ww > 100 && hh > 100;
  if (!test) {
    SDL_Rect ub; if (SDL_GetDisplayUsableBounds(0, &ub) == 0) { ww = ub.w * 8 / 10; hh = ww * 9 / 16; if (hh > ub.h * 8 / 10) { hh = ub.h * 8 / 10; ww = hh * 16 / 9; } }
    const char *g = store_get("winGeom", NULL);
    if (g && sscanf(g, "%d %d %d %d %d %d", &G.x, &G.y, &G.w, &G.h, &G.max, &G.full) == 6 && G.w >= 640 && G.h >= 360 && on_a_screen(G.x, G.y, G.w, G.h)) { G.have = 1; ww = G.w; hh = G.h; } }
  *w = ww; *h = hh; *flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN; /* (shown in plat_post_window, in its place; no SDL_WINDOW_OPENGL: the surface is ANGLE's) */
}
static void save_geom(void) {
  if (!sw || testSize) return; Uint32 f = SDL_GetWindowFlags(sw); /* (a test's fixed size is never kept) */
  if (!(f & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MINIMIZED))) { SDL_GetWindowPosition(sw, &G.x, &G.y); SDL_GetWindowSize(sw, &G.w, &G.h); } /* (the normal place, kept under a maximised one) */
  G.max = (f & SDL_WINDOW_MAXIMIZED) != 0; G.full = (f & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
  if (G.w < 640 || G.h < 360) return;
  char b[96]; snprintf(b, sizeof b, "%d %d %d %d %d %d", G.x, G.y, G.w, G.h, G.max, G.full); const char *o = store_get("winGeom", ""); if (strcmp(o, b)) store_set("winGeom", b);
}
int plat_fullscreen_on(void) { return sw && (SDL_GetWindowFlags(sw) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP; }
void plat_fullscreen_toggle(void) {
  if (!sw) return; int full = (SDL_GetWindowFlags(sw) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
  SDL_SetWindowFullscreen(sw, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP); fprintf(stderr, "[window] %s\n", full ? "windowed" : "fullscreen"); save_geom();
}
/* the keyboard: arrows, Enter and Escape are the remote's arrows, OK and Back; Backspace deletes where text is typed;
   F11 or Alt+Enter: fullscreen; Ctrl+F: Search; Ctrl+V: paste on a code card; F1-F4: the colour keys; Space and the media keys: play and pause */
int plat_map_key(const SDL_KeyboardEvent *e) {
  SDL_Keycode k = e->keysym.sym; int alt = (e->keysym.mod & KMOD_ALT) != 0, ctrl = (e->keysym.mod & KMOD_CTRL) != 0;
  if (k == SDLK_F11 || ((k == SDLK_RETURN || k == SDLK_KP_ENTER) && alt)) { if (e->type == SDL_KEYDOWN && !e->repeat) plat_fullscreen_toggle(); return K_NONE; }
  if (ctrl && k == SDLK_f) return K_GREEN;
  if (ctrl && k == SDLK_v) return K_PASTE;
  if (alt || ctrl) return K_NONE; /* (Alt+Tab, Ctrl+C...: not the app's keys) */
  switch (k) {
    case SDLK_LEFT: return K_LEFT; case SDLK_RIGHT: return K_RIGHT; case SDLK_UP: return K_UP; case SDLK_DOWN: return K_DOWN;
    case SDLK_RETURN: case SDLK_KP_ENTER: return K_OK;
    case SDLK_BACKSPACE: return K_ERASE;
    case SDLK_ESCAPE: case SDLK_AC_BACK: return K_BACK;
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
  if (e->type == SDL_WINDOWEVENT && (e->window.event == SDL_WINDOWEVENT_MOVED || e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e->window.event == SDL_WINDOWEVENT_MAXIMIZED || e->window.event == SDL_WINDOWEVENT_RESTORED)) save_geom();
  if (e->type == SDL_WINDOWEVENT) { if (e->window.event == SDL_WINDOWEVENT_RESTORED) return 1; if (e->window.event == SDL_WINDOWEVENT_MINIMIZED) return -1; }
  return 0;
}
/* Windows keeps the country apart from the format locale (Settings > Time & language > Region): an "en-US" format in
   Ireland is common, and the picks' region (Top 10, where titles stream) follows the country */
const char *plat_country(void) {
  static char cc[4]; static int done; if (done) return cc[0] ? cc : NULL; done = 1;
  wchar_t g[16]; if (GetUserDefaultGeoName(g, 16) > 0 && g[0] && g[1] && !g[2]) { cc[0] = (char)towupper(g[0]); cc[1] = (char)towupper(g[1]); cc[2] = 0; } /* (a two-letter code: never a UN M.49 number such as "001") */
  return cc[0] ? cc : NULL;
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

/* phones only (core/form.h): nothing to turn, hide or buzz here */
void plat_orientation(int landscape) { (void)landscape; }
void plat_immersive(int on) { (void)on; }
void plat_haptic(void) {}
void plat_safe_area(int *top, int *bottom, int *left, int *right) { *top = *bottom = *left = *right = 0; }
int  plat_open_url(const char *url) { (void)url; return 0; }
/* ---- updates: the release's setup file, downloaded on a thread into %TEMP%, its SHA-256 checked against the one
   GitHub lists, then run silently (/S) with /RELAUNCH; the app closes so the files can be replaced, and the installer
   opens the new version when it is done ---- */
static struct { char url[600], sha[72], ver[24]; wchar_t path[MAX_PATH]; volatile LONG st; int pct; char why[160]; int quit; } UP; /* st: 0 working, 1 handed over, -1 failed */
static int up_progress(void *ud, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un) { (void)ud; (void)ut; (void)un; if (total > 0) UP.pct = (int)(now * 100 / total); return 0; }
static size_t up_write(char *p, size_t s, size_t n, void *ud) { return fwrite(p, s, n, ud); }
static void *up_thread(void *a) {
  (void)a; plat_thread_background(5);
  wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); wchar_t wv[24]; MultiByteToWideChar(CP_UTF8, 0, UP.ver, -1, wv, 24);
  _snwprintf(UP.path, MAX_PATH, L"%lsOrigin-setup-%ls.exe", tmp, wv); UP.path[MAX_PATH - 1] = 0;
  FILE *f = _wfopen(UP.path, L"wb"); if (!f) { snprintf(UP.why, sizeof UP.why, "cannot write the download"); InterlockedExchange(&UP.st, -1); return NULL; }
  CURL *c = curl_easy_init(); char ca[1100]; snprintf(ca, sizeof ca, "%s/cacert.pem", assetDir); long code = 0;
  curl_easy_setopt(c, CURLOPT_URL, UP.url); curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L); curl_easy_setopt(c, CURLOPT_CAINFO, ca);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, up_write); curl_easy_setopt(c, CURLOPT_WRITEDATA, f); curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, up_progress); curl_easy_setopt(c, CURLOPT_USERAGENT, "StremioOrigin (Windows; native)");
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L); curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L); curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
  CURLcode r = curl_easy_perform(c); curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code); curl_easy_cleanup(c); fclose(f);
  if (r != CURLE_OK || code != 200) { snprintf(UP.why, sizeof UP.why, "download failed (%s, http %ld)", curl_easy_strerror(r), code); _wremove(UP.path); InterlockedExchange(&UP.st, -1); return NULL; }
  /* the file as GitHub listed it, or nothing runs */
  char hex[65] = "";
  if (!win_sha256_file(UP.path, hex) || _stricmp(hex, UP.sha)) { snprintf(UP.why, sizeof UP.why, "the download does not match its checksum"); fprintf(stderr, "[update] checksum %s, expected %s\n", hex, UP.sha); _wremove(UP.path); InterlockedExchange(&UP.st, -1); return NULL; }
  wchar_t cmd[MAX_PATH + 40]; _snwprintf(cmd, MAX_PATH + 40, L"\"%ls\" /S /RELAUNCH", UP.path); cmd[MAX_PATH + 39] = 0;
  STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
  if (!CreateProcessW(UP.path, cmd, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi)) { snprintf(UP.why, sizeof UP.why, "the installer did not start (error %lu)", GetLastError()); InterlockedExchange(&UP.st, -1); return NULL; }
  CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
  fprintf(stderr, "[update] %s checked and handed to the installer\n", UP.ver); InterlockedExchange(&UP.st, 1); return NULL;
}
int plat_install_start(const char *url, const char *sha256, const char *version) {
  if (!url || !url[0] || !sha256 || strlen(sha256) != 64) return 0;
  memset(&UP, 0, sizeof UP); snprintf(UP.url, sizeof UP.url, "%s", url); snprintf(UP.sha, sizeof UP.sha, "%s", sha256); snprintf(UP.ver, sizeof UP.ver, "%s", version ? version : "new");
  pthread_t t; if (pthread_create(&t, NULL, up_thread, NULL)) return 0; pthread_detach(t);
  fprintf(stderr, "[update] downloading %s\n", UP.url); return 1;
}
int plat_install_poll(char *msg, int cap) {
  LONG st = UP.st;
  if (st < 0) { snprintf(msg, (size_t)cap, "%s", UP.why); return -1; }
  if (st == 1) { snprintf(msg, (size_t)cap, "Installing: Origin opens again in a moment");
    if (!UP.quit) { UP.quit = 1; SDL_Event q; memset(&q, 0, sizeof q); q.type = SDL_QUIT; SDL_PushEvent(&q); } /* (the installer waits for the app to close) */
    return 1; }
  if (UP.pct > 0) snprintf(msg, (size_t)cap, "Downloading… %d%%", UP.pct); else msg[0] = 0;
  return 0;
}

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
  int warp = getenv("SB_WARP") != NULL; /* development: Direct3D's software renderer, as a PC with no usable GPU driver has */
  const EGLint da[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE, warp ? 0x3209 : EGL_NONE_, 0x320B /* WARP device */, EGL_NONE_ };
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
int win_screen_shot(void *hwnd, const char *path); /* (dev_win.c) */
void plat_gl_swap(SDL_Window *w) {
  (void)w; E.SwapBuffers(dpy, surf);
  static int n, at = -2; if (at == -2) at = getenv("SB_SCREEN_SHOT") && getenv("SB_SCREEN_FRAME") ? atoi(getenv("SB_SCREEN_FRAME")) : -1; /* development: the composed screen at that frame */
  ++n; if (at >= 0 && n == at) win_screen_shot(top, getenv("SB_SCREEN_SHOT"));
  { void win_dev_input(void *hwnd, int frame); win_dev_input(top, n); } /* development: SB_POST */
}
void plat_gl_interval(int n) { if (dpy) E.SwapInterval(dpy, n); }
void plat_gl_size(SDL_Window *w, int *pw, int *ph) { RECT r; (void)w; if (top && GetClientRect(top, &r)) { *pw = r.right - r.left; *ph = r.bottom - r.top; } else { *pw = *ph = 0; } }
void plat_gl_destroy(void *c) {
  (void)c; if (!dpy) return;
  E.MakeCurrent(dpy, NULL, NULL, NULL); if (ctxt) E.DestroyContext(dpy, ctxt); if (surf) E.DestroySurface(dpy, surf); E.Terminate(dpy); dpy = NULL;
}
