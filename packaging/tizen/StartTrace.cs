// The start trace: one line per stage of every start (data/start-trace.log), written by the host and by the app
// (src/platform/tizen/host_bridge.c) in the same form: "<unix time> <stage> <detail>". Each line is a single append, so
// a start that ends in a crash still leaves the stages it reached. Read back only to show on the error screen and in
// the app's log; nothing depends on it.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace StremioOrigin
{
    static class StartTrace
    {
        static readonly DateTime Epoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);
        const long KeepBelow = 48 * 1024, KeepTail = 24 * 1024;
        static string path;

        public static void Open(string dataDir)
        {
            path = Path.Combine(dataDir, "start-trace.log");
            try
            {
                var info = new FileInfo(path);
                if (info.Exists && info.Length > KeepBelow)
                {
                    byte[] all = File.ReadAllBytes(path);
                    int from = (int)(all.Length - KeepTail);
                    while (from < all.Length && all[from - 1] != (byte)'\n') from++;
                    var tail = new byte[all.Length - from]; Array.Copy(all, from, tail, 0, tail.Length); File.WriteAllBytes(path, tail);
                }
            }
            catch { }
        }

        public static void Mark(string stage, string detail = null)
        {
            if (path == null) return;
            double t = (DateTime.UtcNow - Epoch).TotalSeconds;
            string line = t.ToString("F3", System.Globalization.CultureInfo.InvariantCulture) + " " + stage
                          + (string.IsNullOrEmpty(detail) ? "" : " " + detail.Replace('\n', ' ')) + "\n";
            try
            {
                using (var f = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite))
                {
                    byte[] b = Encoding.UTF8.GetBytes(line);
                    f.Write(b, 0, b.Length);
                }
            }
            catch { }
        }

        // every stage in the trace, oldest first
        public static List<string> All()
        {
            var lines = new List<string>();
            try { foreach (string l in File.ReadAllLines(path)) { int sp = l.IndexOf(' '); lines.Add(sp < 0 ? l : l.Substring(sp + 1)); } }
            catch { }
            return lines;
        }

        // this start's stages, oldest first (from its "start" line on)
        public static List<string> ThisStart()
        {
            var lines = new List<string>();
            try
            {
                foreach (string l in File.ReadAllLines(path))
                {
                    int sp = l.IndexOf(' ');
                    string stage = sp < 0 ? l : l.Substring(sp + 1);
                    if (stage == "start" || stage.StartsWith("start ")) lines.Clear();
                    lines.Add(stage);
                }
            }
            catch { }
            return lines;
        }
    }
}
