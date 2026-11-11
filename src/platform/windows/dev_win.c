/* Development: the screen as the viewer sees it (the video's window and the UI's layer composed), for tests. GDI here,
   which the other Windows files keep out (wingdi.h's RGB). Only with the window kept on top (SB_TOPMOST), so the
   capture can hold nothing but this app. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes); /* (main.c has it) */
int win_screen_shot(void *hwnd, const char *path) {
  if (!getenv("SB_TOPMOST")) { fprintf(stderr, "[shot] screen capture refused: the window is not kept on top (SB_TOPMOST)\n"); return 0; }
  HWND w = hwnd; RECT r; if (!w || !GetClientRect(w, &r)) return 0; int cw = r.right, ch = r.bottom; if (cw <= 0 || ch <= 0) return 0;
  POINT o = { 0, 0 }; ClientToScreen(w, &o);
  HDC s = GetDC(NULL), m = CreateCompatibleDC(s); BITMAPINFO bi; memset(&bi, 0, sizeof bi);
  bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = cw; bi.bmiHeader.biHeight = -ch; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
  void *bits = NULL; HBITMAP b = CreateDIBSection(m, &bi, DIB_RGB_COLORS, &bits, NULL, 0); HGDIOBJ old = SelectObject(m, b);
  BitBlt(m, 0, 0, cw, ch, s, o.x, o.y, SRCCOPY | CAPTUREBLT); GdiFlush();
  unsigned char *p = bits; for (int i = 0; i < cw * ch; i++) { unsigned char t = p[i * 4]; p[i * 4] = p[i * 4 + 2]; p[i * 4 + 2] = t; p[i * 4 + 3] = 255; } /* BGRA to RGBA */
  int ok = stbi_write_png(path, cw, ch, 4, bits, cw * 4);
  SelectObject(m, old); DeleteObject(b); DeleteDC(m); ReleaseDC(NULL, s);
  fprintf(stderr, "[shot] screen %s\n", path); return ok;
}
