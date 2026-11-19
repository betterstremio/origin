// The picker's own error screen: the code, the TV, the stages reached. No sentences. Drawn with the scene toolkit the
// TV has; when none can draw, the trace on storage is all there is (the caller has already written it).
// Compiled against the oldest reference set on purpose: only members that exist from Tizen 4 on are used.
using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using Tizen.NUI;
using Tizen.NUI.BaseComponents;

namespace StremioOrigin.Start
{
    static class ErrorScreen
    {
        // dataDir: where the scannable code's picture is written for the toolkit to show (null: no code, the text alone)
        public static bool Show(string code, string model, string tizen, List<string> stages, string dataDir)
        {
            if (!Tv.TypeExists("Tizen.NUI.NUIApplication, Tizen.NUI")) return false;
            string whole = WholePicture(code, model, tizen, stages, dataDir);
            try { Draw(code, model, tizen, stages, whole == null ? ReportPicture(code, model, tizen, stages, dataDir) : null, whole); return true; }
            catch { return false; }
        }

        // apart and never inlined: the toolkit's types are only touched once the check above has passed
        [MethodImpl(MethodImplOptions.NoInlining)]
        static void Draw(string code, string model, string tizen, List<string> stages, string picture, string whole)
        {
            new Scene(code, model, tizen, stages, picture, whole).Run(new string[0]);
        }

        // the whole screen as one picture in the app's data folder: its path, or null when it could not be made
        public static string WholePicture(string code, string model, string tizen, IList<string> stages, string dataDir)
        {
            try
            {
                if (dataDir == null) return null;
                var lines = FailLayout.Lines(code, null, "TV " + model + " / Tizen " + tizen, null, stages);
                byte[] png = FailLayout.Make(lines, 1920, 1080, FailLayout.Report(code, lines)).Png();
                string file = System.IO.Path.Combine(dataDir, "start-screen.png");
                System.IO.File.WriteAllBytes(file, png);
                return file;
            }
            catch { return null; }
        }

        // the same text as a code to scan (it opens a report already written), saved as a picture: its path, or null
        public static int PictureSide;
        public static string ReportPicture(string code, string model, string tizen, IList<string> stages, string dataDir)
        {
            PictureSide = 0;
            try
            {
                if (dataDir == null) return null;
                var text = new List<string> { "TV " + model + " / Tizen " + tizen }; text.AddRange(stages);
                bool[,] m = Qr.Encode(System.Text.Encoding.UTF8.GetBytes(Qr.ReportUrl(code, text, 900)));
                if (m == null) return null;
                int scale = Math.Max(3, Math.Min(6, 540 / (m.GetLength(0) + 8)));
                string file = System.IO.Path.Combine(dataDir, "start-report.bmp");
                System.IO.File.WriteAllBytes(file, Qr.Bmp(m, scale));
                PictureSide = (m.GetLength(0) + 8) * scale;
                return file;
            }
            catch { PictureSide = 0; return null; }
        }

        // kept in a class of its own so that a TV without the toolkit never has to load it
        sealed class Scene : NUIApplication
        {
            readonly string code, model, tizen, picture, whole; readonly List<string> stages;
            static int textWidth = 1920;   // (narrower with the code's picture bottom right: the text keeps to its left)
            public Scene(string code, string model, string tizen, List<string> stages, string picture, string whole) { this.code = code; this.model = model; this.tizen = tizen; this.stages = stages; this.picture = picture; this.whole = whole; }

            protected override void OnCreate()
            {
                base.OnCreate();
                Window w = Window.Instance;
                w.BackgroundColor = Color.Black;
                Layer root = w.GetDefaultLayer();
                w.KeyEvent += (o, e) => { if (e.Key.State == Key.StateType.Up) Exit(); };   // any key leaves
                // the screen as every failed start shows it (FailLayout), saved as one picture and shown as it is: the same
                // look as a host's, and none of the TV's fonts. Only if that cannot be done, the toolkit's own text below.
                if (whole != null)
                {
                    try { root.Add(new ImageView(whole) { Size2D = new Size2D(1920, 1080), Position2D = new Position2D(0, 0) }); return; }
                    catch { }
                }
                int perLine = 110;
                if (picture != null && PictureSide > 0)
                {
                    int side = PictureSide;
                    try { root.Add(new ImageView(picture) { Size2D = new Size2D(side, side), Position2D = new Position2D(1920 - 80 - side, 1080 - 70 - side) }); textWidth = 1920 - 80 - side - 40; perLine = 70; }
                    catch { textWidth = 1920; }
                }
                root.Add(Line(code, 64, 380));
                root.Add(Line(model + "  ·  Tizen " + tizen, 28, 500));
                int y = 580, shown = 0;
                foreach (string s in stages)
                {
                    // (a long line, such as the folders looked in, goes on over the next lines instead of being cut)
                    for (int at = 0; at < s.Length && at < 440 && shown < 12; at += perLine, shown++)
                    { root.Add(Line(s.Substring(at, Math.Min(perLine, s.Length - at)), 18, y)); y += 34; }
                    if (shown >= 12) break;
                }
            }

            static TextLabel Line(string text, float points, int y)
            {
                return new TextLabel(text)
                {
                    TextColor = Color.White, PointSize = points, MultiLine = false,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    Size2D = new Size2D(textWidth, (int)(points * 2.2f)), Position2D = new Position2D(0, y),
                };
            }
        }
    }
}
