/* Development: the screen as the viewer sees it (the video's window and the UI's layer composed), for tests. GDI here,
   which the other Windows files keep out (wingdi.h's RGB). Only with the window kept on top (SB_TOPMOST), so the
   capture can hold nothing but this app. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
/* Development: SB_POST="300:key F11,320:type abc,340:move 640 360,360:click 640 360,380:rclick 640 360,400:dbl 640 360,
   420:wheel -120 640 360,440:altenter" posts real Windows input messages to this window at those frames (window
   pixels), so tests go through SDL, the key map and the mouse mapping as a person's input would, without moving their
   mouse */
int win_screen_shot(void *hwnd, const char *path);
void win_dev_input(void *hwnd, int frame) {
  static const char *spec;
  static int init;
  if (!init) {
    init = 1;
    spec = getenv("SB_POST");
  }
  if (!spec || !hwnd)
    return;
  HWND w = hwnd;
  const char *p = spec;
  while (*p) {
    int f = atoi(p);
    const char *c = strchr(p, ':'), *end = strchr(p, ',');
    if (!end)
      end = p + strlen(p);
    if (!c || c > end) {
      p = *end ? end + 1 : end;
      continue;
    }
    char a[160];
    snprintf(a, sizeof a, "%.*s", (int)(end - c - 1), c + 1);
    if (f == frame) {
      int x = 0, y = 0, d = 0, d2 = 0;
      char nm[32] = "";
      if (sscanf(a, "move %d %d", &x, &y) == 2)
        PostMessageW(w, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
      else if (sscanf(a, "click %d %d", &x, &y) == 2) {
        PostMessageW(w, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
        PostMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        PostMessageW(w, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
      } else if (sscanf(a, "rclick %d %d", &x, &y) == 2) {
        PostMessageW(w, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(x, y));
        PostMessageW(w, WM_RBUTTONUP, 0, MAKELPARAM(x, y));
      } else if (sscanf(a, "dbl %d %d", &x, &y) == 2) {
        for (int i = 0; i < 2; i++) {
          PostMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
          PostMessageW(w, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        }
      } else if (sscanf(a, "wheel %d %d %d", &d, &x, &y) == 3) {
        POINT s = { x, y };
        ClientToScreen(w, &s);
        PostMessageW(w, WM_MOUSEWHEEL, MAKEWPARAM(0, (short)d), MAKELPARAM(s.x, s.y));
      } else if (sscanf(a, "place %d %d %d %d", &x, &y, &d, &d2) == 4) {
        RECT r = { 0, 0, d, d2 };
        AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(w, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(w, GWL_EXSTYLE));
        SetWindowPos(w, NULL, x, y, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
      } /* "place X Y W H": the window moved and resized (client W x H) */
      else if (sscanf(a, "drag %d %d", &d, &d2) == 2) { /* "drag W H": a border drag to client W x H, as Windows runs
          one: its own loop (the app's waits), the size in 20 steps; SB_DRAG_SHOT: the screen half way */
        RECT c;
        GetClientRect(w, &c);
        int w0 = c.right, h0 = c.bottom;
        RECT wr;
        GetWindowRect(w, &wr);
        SendMessageW(w, WM_ENTERSIZEMOVE, 0, 0);
        for (int i = 1; i <= 20; i++) {
          RECT r = { 0, 0, w0 + (d - w0) * i / 20, h0 + (d2 - h0) * i / 20 };
          AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(w, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(w, GWL_EXSTYLE));
          SetWindowPos(w, NULL, wr.left, wr.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
          DWORD until = GetTickCount() + 30;
          MSG m;
          while (GetTickCount() < until) {
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
              TranslateMessage(&m);
              DispatchMessageW(&m);
            }
            Sleep(1);
          }
          if (i == 10 && getenv("SB_DRAG_SHOT"))
            win_screen_shot(w, getenv("SB_DRAG_SHOT"));
        }
        SendMessageW(w, WM_EXITSIZEMOVE, 0, 0);
      } else if (!strncmp(a, "type ", 5)) {
        for (const char *t = a + 5; *t; t++)
          PostMessageW(w, WM_CHAR, (WPARAM)(unsigned char)*t, 1);
      } else if (!strcmp(a, "altenter")) {
        UINT sa = MapVirtualKeyW(VK_MENU, MAPVK_VK_TO_VSC),
             sr = MapVirtualKeyW(
                 VK_RETURN, MAPVK_VK_TO_VSC); /* Alt down, Enter, Alt up: SDL takes the modifier from the keys before */
        PostMessageW(w, WM_SYSKEYDOWN, VK_MENU, 1 | (sa << 16) | (1 << 29));
        PostMessageW(w, WM_SYSKEYDOWN, VK_RETURN, 1 | (sr << 16) | (1 << 29));
        PostMessageW(w, WM_SYSKEYUP, VK_RETURN, 1 | (sr << 16) | (1 << 29) | (3u << 30));
        PostMessageW(w, WM_KEYUP, VK_MENU, 1 | (sa << 16) | (3u << 30));
      } else if (sscanf(a, "key %31s", nm) == 1) {
        static const struct {
          const char *n;
          int vk;
        } K[] = { { "F11", VK_F11 },        { "ENTER", VK_RETURN }, { "ESC", VK_ESCAPE },
                  { "BACKSPACE", VK_BACK }, { "LEFT", VK_LEFT },    { "RIGHT", VK_RIGHT },
                  { "UP", VK_UP },          { "DOWN", VK_DOWN },    { "SPACE", VK_SPACE } };
        for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
          if (!strcmp(K[i].n, nm)) {
            UINT sc = MapVirtualKeyW(K[i].vk, MAPVK_VK_TO_VSC);
            int ext = K[i].vk >= VK_LEFT && K[i].vk <= VK_DOWN;
            PostMessageW(w, WM_KEYDOWN, K[i].vk, 1 | (sc << 16) | (ext << 24));
            PostMessageW(w, WM_KEYUP, K[i].vk, 1 | (sc << 16) | (ext << 24) | (3u << 30));
          }
      }
      fprintf(stderr, "[post] %d: %s\n", frame, a);
    }
    p = *end ? end + 1 : end;
  }
}
int stbi_write_png(char const *filename, int w, int h, int comp, const void *data,
                   int stride_in_bytes); /* (main.c has it) */
int win_screen_shot(void *hwnd, const char *path) {
  if (!getenv("SB_TOPMOST")) {
    fprintf(stderr, "[shot] screen capture refused: the window is not kept on top (SB_TOPMOST)\n");
    return 0;
  }
  HWND w = hwnd;
  RECT r;
  if (!w || !GetClientRect(w, &r))
    return 0;
  int cw = r.right, ch = r.bottom;
  if (cw <= 0 || ch <= 0)
    return 0;
  POINT o = { 0, 0 };
  ClientToScreen(w, &o);
  HDC s = GetDC(NULL), m = CreateCompatibleDC(s);
  BITMAPINFO bi;
  memset(&bi, 0, sizeof bi);
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = cw;
  bi.bmiHeader.biHeight = -ch;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void *bits = NULL;
  HBITMAP b = CreateDIBSection(m, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  HGDIOBJ old = SelectObject(m, b);
  BitBlt(m, 0, 0, cw, ch, s, o.x, o.y, SRCCOPY | CAPTUREBLT);
  GdiFlush();
  unsigned char *p = bits;
  for (int i = 0; i < cw * ch; i++) {
    unsigned char t = p[i * 4];
    p[i * 4] = p[i * 4 + 2];
    p[i * 4 + 2] = t;
    p[i * 4 + 3] = 255;
  } /* BGRA to RGBA */
  int ok = stbi_write_png(path, cw, ch, 4, bits, cw * 4);
  SelectObject(m, old);
  DeleteObject(b);
  DeleteDC(m);
  ReleaseDC(NULL, s);
  fprintf(stderr, "[shot] screen %s\n", path);
  return ok;
}
