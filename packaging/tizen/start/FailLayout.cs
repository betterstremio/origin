// A failed start's screen, laid out once for every part of the package that can show one: the picker, and each host.
// The text (the code, what failed, the TV, the stages reached) in a built-in 5 x 7 letter set, and bottom right the
// code to scan. Out come plain rectangles, so the same screen can be drawn with nothing but clears in a GL window
// (host A, FailText) or saved as one picture for a toolkit to show (the picker and the newer hosts): the same look
// everywhere, and no font or text toolkit of the TV's involved.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;

namespace StremioOrigin.Start
{
    public sealed class FailLayout
    {
        // each letter: 7 rows of 5 dots, the top row first, the left dot the high bit
        static readonly Dictionary<char, byte[]> Set = new Dictionary<char, byte[]>
        {
            {'A', new byte[]{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'B', new byte[]{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
            {'C', new byte[]{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D', new byte[]{0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}},
            {'E', new byte[]{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F', new byte[]{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
            {'G', new byte[]{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, {'H', new byte[]{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
            {'I', new byte[]{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}}, {'J', new byte[]{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
            {'K', new byte[]{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L', new byte[]{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
            {'M', new byte[]{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N', new byte[]{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
            {'O', new byte[]{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P', new byte[]{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
            {'Q', new byte[]{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}}, {'R', new byte[]{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
            {'S', new byte[]{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T', new byte[]{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
            {'U', new byte[]{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'V', new byte[]{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
            {'W', new byte[]{0x11,0x11,0x11,0x15,0x15,0x15,0x0A}}, {'X', new byte[]{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
            {'Y', new byte[]{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}}, {'Z', new byte[]{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
            {'0', new byte[]{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1', new byte[]{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
            {'2', new byte[]{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3', new byte[]{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
            {'4', new byte[]{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5', new byte[]{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
            {'6', new byte[]{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7', new byte[]{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
            {'8', new byte[]{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9', new byte[]{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
            {'-', new byte[]{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}}, {'.', new byte[]{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
            {':', new byte[]{0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}}, {'/', new byte[]{0x00,0x01,0x02,0x04,0x08,0x10,0x00}},
            {'(', new byte[]{0x02,0x04,0x08,0x08,0x08,0x04,0x02}}, {')', new byte[]{0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
            {',', new byte[]{0x00,0x00,0x00,0x00,0x0C,0x04,0x08}}, {'_', new byte[]{0x00,0x00,0x00,0x00,0x00,0x00,0x1F}},
            {'=', new byte[]{0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}}, {'+', new byte[]{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
            {'%', new byte[]{0x18,0x19,0x02,0x04,0x08,0x13,0x03}}, {'\'', new byte[]{0x0C,0x04,0x08,0x00,0x00,0x00,0x00}},
            {'?', new byte[]{0x0E,0x11,0x01,0x02,0x04,0x00,0x04}}, {'!', new byte[]{0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
            {'[', new byte[]{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}}, {']', new byte[]{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}},
            {'|', new byte[]{0x04,0x04,0x04,0x04,0x04,0x04,0x04}}, {'*', new byte[]{0x00,0x04,0x15,0x0E,0x15,0x04,0x00}},
            {'>', new byte[]{0x08,0x04,0x02,0x01,0x02,0x04,0x08}}, {'<', new byte[]{0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
            {'"', new byte[]{0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}}, {'#', new byte[]{0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}},
        };

        public int W, H;
        public int[] Light;   // x, y (from the top), w, h: the lit runs of the text
        public int[] Dark;    // the same for the dark modules of the code to scan, over its light square
        public int Qx, Qy, QSide;   // that light square (0 wide: no code)

        // the lines of every failed start's screen, in one order and one set of sizes
        public static List<KeyValuePair<string, int>> Lines(string code, string title, string tv, IEnumerable<string> details, IEnumerable<string> stages, string footer = "Press Back to exit")
        {
            var l = new List<KeyValuePair<string, int>>();
            l.Add(new KeyValuePair<string, int>(code, 18));
            if (!string.IsNullOrEmpty(title)) l.Add(new KeyValuePair<string, int>(title, 6));
            l.Add(new KeyValuePair<string, int>(tv, 4));
            if (details != null) foreach (string d in details) l.Add(new KeyValuePair<string, int>(d, 3));
            if (stages != null) foreach (string s in stages) l.Add(new KeyValuePair<string, int>(s, 3));
            l.Add(new KeyValuePair<string, int>(footer, 4));
            return l;
        }

        // the link the code to scan holds: every line between the code and the last, as a report already written
        public static bool[,] Report(string code, IList<KeyValuePair<string, int>> lines)
        {
            try
            {
                var text = new List<string>(); for (int i = 1; i < lines.Count - 1; i++) text.Add(lines[i].Key);
                return Qr.Encode(System.Text.Encoding.UTF8.GetBytes(Qr.ReportUrl(code, text, 900)));
            }
            catch { return null; }
        }

        // lines: the text and how large (dots are size x size pixels); long lines carry over, the screen's height is the
        // limit. qr: the code's modules ([row, column], true = dark) or null; it sits bottom right, the text keeps to its left
        public static FailLayout Make(IList<KeyValuePair<string, int>> lines, int W, int H, bool[,] qr)
        {
            var f = new FailLayout { W = W, H = H };
            var r = new List<int>();
            int left = W / 16, y = H / 9, bottom = H - H / 12;
            if (qr != null)
            {
                int n = qr.GetLength(0), m = Math.Max(3, Math.Min(6, (H / 2) / (n + 8)));
                f.QSide = (n + 8) * m; f.Qx = W - left - f.QSide; f.Qy = bottom - f.QSide;
                var d = new List<int>();
                for (int row = 0; row < n; row++)
                    for (int col = 0; col < n; col++)
                    {
                        if (!qr[row, col]) continue;
                        int run = 1; while (col + run < n && qr[row, col + run]) run++;
                        d.Add(f.Qx + (4 + col) * m); d.Add(f.Qy + (4 + row) * m); d.Add(run * m); d.Add(m);
                        col += run - 1;
                    }
                f.Dark = d.ToArray();
            }
            foreach (var ln in lines)
            {
                int s = ln.Value < 2 ? 2 : ln.Value;
                string t = (ln.Key ?? "").ToUpperInvariant();
                for (int at = 0, perLine = 1; at == 0 || at < t.Length; at += perLine)
                {
                    if (y + 7 * s > bottom) break;
                    // (a row beside the code's square ends before it)
                    int right = f.QSide > 0 && y + 7 * s > f.Qy - s ? f.Qx - 4 * s : W - left;
                    perLine = Math.Max(1, (right - left) / (6 * s));
                    string part = t.Substring(at, Math.Min(perLine, t.Length - at));
                    for (int i = 0; i < part.Length; i++)
                    {
                        byte[] g; if (!Set.TryGetValue(part[i], out g)) continue;   // (a sign not in the set, and the space: left empty)
                        for (int row = 0; row < 7; row++)
                            for (int col = 0; col < 5; col++)
                            {
                                if ((g[row] & (0x10 >> col)) == 0) continue;
                                int run = 1; while (col + run < 5 && (g[row] & (0x10 >> (col + run))) != 0) run++;
                                r.Add(left + (i * 6 + col) * s); r.Add(y + row * s); r.Add(run * s); r.Add(s);
                                col += run - 1;
                            }
                    }
                    y += 9 * s + s / 2;
                    if (t.Length == 0) break;
                }
                y += s;   // a little more between two texts than between the rows of one
            }
            f.Light = r.ToArray();
            return f;
        }

        // the whole screen as a picture file's bytes (PNG, grey, one byte a pixel), for a toolkit to show as it is
        public byte[] Png()
        {
            var px = new byte[W * H];
            Fill(px, Light, 255);
            if (QSide > 0 && Dark != null) { Fill(px, new[] { Qx, Qy, QSide, QSide }, 255); Fill(px, Dark, 0); }
            var raw = new MemoryStream();
            using (var z = new DeflateStream(raw, CompressionLevel.Fastest, true))
                for (int y = 0; y < H; y++) { z.WriteByte(0); z.Write(px, y * W, W); }
            uint a = 1, b = 0;   // the check zlib wants over the same bytes
            for (int y = 0; y < H; y++) { b = (b + a) % 65521; for (int x = 0; x < W; x++) { a += px[y * W + x]; if (a >= 65521) a -= 65521; b += a; if (b >= 65521) b -= 65521; } }
            var idat = new MemoryStream(); idat.WriteByte(0x78); idat.WriteByte(0x01); raw.WriteTo(idat);
            uint ad = b << 16 | a; idat.WriteByte((byte)(ad >> 24)); idat.WriteByte((byte)(ad >> 16)); idat.WriteByte((byte)(ad >> 8)); idat.WriteByte((byte)ad);
            var o = new MemoryStream();
            o.Write(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A }, 0, 8);
            var hd = new byte[13]; Be(hd, 0, W); Be(hd, 4, H); hd[8] = 8;   // 8 bits, grey, no interlace
            Chunk(o, "IHDR", hd); Chunk(o, "IDAT", idat.ToArray()); Chunk(o, "IEND", new byte[0]);
            return o.ToArray();
        }
        void Fill(byte[] px, int[] rects, byte v)
        {
            for (int i = 0; i + 3 < rects.Length; i += 4)
            {
                int x0 = Math.Max(0, rects[i]), x1 = Math.Min(W, rects[i] + rects[i + 2]), y0 = Math.Max(0, rects[i + 1]), y1 = Math.Min(H, rects[i + 1] + rects[i + 3]);
                for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) px[y * W + x] = v;
            }
        }
        static void Be(byte[] f, int at, int v) { f[at] = (byte)(v >> 24); f[at + 1] = (byte)(v >> 16); f[at + 2] = (byte)(v >> 8); f[at + 3] = (byte)v; }
        static uint[] crcT;
        static void Chunk(Stream o, string type, byte[] data)
        {
            if (crcT == null) { crcT = new uint[256]; for (uint n = 0; n < 256; n++) { uint c = n; for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crcT[n] = c; } }
            var len = new byte[4]; Be(len, 0, data.Length); o.Write(len, 0, 4);
            var t = System.Text.Encoding.ASCII.GetBytes(type); uint crc = 0xFFFFFFFFu;
            foreach (byte x in t) crc = crcT[(crc ^ x) & 0xFF] ^ (crc >> 8);
            foreach (byte x in data) crc = crcT[(crc ^ x) & 0xFF] ^ (crc >> 8);
            crc ^= 0xFFFFFFFFu;
            o.Write(t, 0, 4); o.Write(data, 0, data.Length);
            var cb = new byte[4]; Be(cb, 0, unchecked((int)crc)); o.Write(cb, 0, 4);
        }
    }
}
