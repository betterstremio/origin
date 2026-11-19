// A failed start's screen in host A's own drawing window, with nothing but clears of small rectangles: no toolkit, no
// font, no shader and none of the app's library (which may be what failed). Where the TV can show the app's window,
// it can show this. (A separate toolkit window for the error stayed invisible on a Tizen 5.5 TV: a black screen.)
// The screen itself is laid out by FailLayout, the same for the picker and every host.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using StremioOrigin.Start;

namespace StremioOrigin
{
    static class FailText
    {
        [DllImport("libGLESv2.so")] static extern void glClearColor(float r, float g, float b, float a);
        [DllImport("libGLESv2.so")] static extern void glClear(uint mask);
        [DllImport("libGLESv2.so")] static extern void glEnable(uint cap);
        [DllImport("libGLESv2.so")] static extern void glDisable(uint cap);
        [DllImport("libGLESv2.so")] static extern void glScissor(int x, int y, int w, int h);
        const uint GL_COLOR_BUFFER_BIT = 0x4000, GL_SCISSOR_TEST = 0x0C11;

        static FailLayout f;

        public static void Show(IList<KeyValuePair<string, int>> lines, int W, int H, bool[,] qr) { f = FailLayout.Make(lines, W, H, qr); }

        // every frame, with the drawing window's context current: black, the text in white, the code's square and its dark modules
        public static void Draw(int H)
        {
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0f, 0f, 0f, 1f); glClear(GL_COLOR_BUFFER_BIT);
            if (f == null) return;
            glEnable(GL_SCISSOR_TEST);
            glClearColor(1f, 1f, 1f, 1f);
            int[] r = f.Light;
            for (int i = 0; i + 3 < r.Length; i += 4) { glScissor(r[i], H - r[i + 1] - r[i + 3], r[i + 2], r[i + 3]); glClear(GL_COLOR_BUFFER_BIT); }
            if (f.QSide > 0 && f.Dark != null)
            {
                glScissor(f.Qx, H - f.Qy - f.QSide, f.QSide, f.QSide); glClear(GL_COLOR_BUFFER_BIT);
                glClearColor(0f, 0f, 0f, 1f);
                int[] d = f.Dark;
                for (int i = 0; i + 3 < d.Length; i += 4) { glScissor(d[i], H - d[i + 1] - d[i + 3], d[i + 2], d[i + 3]); glClear(GL_COLOR_BUFFER_BIT); }
            }
            glDisable(GL_SCISSOR_TEST);
        }
    }
}
