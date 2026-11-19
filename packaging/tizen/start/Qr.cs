// A QR code for a failed start's screen: the report's text as a link a phone opens (a new issue, already filled in),
// so nobody has to photograph or type the screen. Bytes in, a square of dark and light modules out: byte mode, the
// lowest error-correction level (the screen is clean and still), the smallest version that holds the text.
// Plain .NET only: used by the picker and by every host.
using System;
using System.Collections.Generic;
using System.Text;

namespace StremioOrigin.Start
{
    public static class Qr
    {
        // error-correction level L, versions 1 to 40: check bytes in each block, and how many blocks
        static readonly int[] EccPerBlock = { 0, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30 };
        static readonly int[] Blocks = { 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25 };

        static int RawModules(int ver)
        {
            int r = (16 * ver + 128) * ver + 64;
            if (ver >= 2) { int n = ver / 7 + 2; r -= (25 * n - 10) * n - 55; if (ver >= 7) r -= 36; }
            return r;
        }
        static int DataBytes(int ver) { return RawModules(ver) / 8 - EccPerBlock[ver] * Blocks[ver]; }

        // the largest text (in bytes) a code can hold at all
        public static int MaxBytes { get { return DataBytes(40) - 3; } }

        // null when the bytes do not fit the largest code; else [row, column], true = dark
        public static bool[,] Encode(byte[] data)
        {
            if (data == null) return null;
            int ver = 0;
            for (int v = 1; v <= 40; v++) { int need = 4 + (v < 10 ? 8 : 16) + data.Length * 8; if (need <= DataBytes(v) * 8) { ver = v; break; } }
            if (ver == 0) return null;

            // the bit stream: byte mode, the count, the bytes, the end mark, padding
            int cap = DataBytes(ver);
            var bits = new List<bool>(cap * 8);
            Put(bits, 4, 4); Put(bits, data.Length, ver < 10 ? 8 : 16);
            foreach (byte b in data) Put(bits, b, 8);
            for (int i = 0; i < 4 && bits.Count < cap * 8; i++) bits.Add(false);
            while (bits.Count % 8 != 0) bits.Add(false);
            for (int pad = 0xEC; bits.Count < cap * 8; pad ^= 0xEC ^ 0x11) Put(bits, pad, 8);
            var words = new byte[cap];
            for (int i = 0; i < bits.Count; i++) if (bits[i]) words[i >> 3] |= (byte)(0x80 >> (i & 7));

            byte[] all = WithEcc(words, ver);
            int size = ver * 4 + 17;
            var m = new bool[size, size]; var fn = new bool[size, size];
            Fixed(m, fn, ver, size);
            Place(m, fn, all, size);

            int best = 0, bestScore = int.MaxValue;
            for (int mask = 0; mask < 8; mask++)
            {
                Mask(m, fn, mask, size); Format(m, fn, mask, size);
                int s = Penalty(m, size);
                if (s < bestScore) { bestScore = s; best = mask; }
                Mask(m, fn, mask, size);   // (the same mask again takes it off)
            }
            Mask(m, fn, best, size); Format(m, fn, best, size);
            return m;
        }

        static void Put(List<bool> bits, int value, int n) { for (int i = n - 1; i >= 0; i--) bits.Add(((value >> i) & 1) != 0); }

        // ---- the check bytes (Reed-Solomon over GF(256), polynomial 0x11D), blocks interleaved ----
        static byte Mul(byte x, byte y)
        {
            int z = 0;
            for (int i = 7; i >= 0; i--) { z = (z << 1) ^ ((z >> 7) * 0x11D); z ^= ((y >> i) & 1) * x; }
            return (byte)z;
        }
        static byte[] Divisor(int degree)
        {
            var r = new byte[degree]; r[degree - 1] = 1; byte root = 1;
            for (int i = 0; i < degree; i++)
            {
                for (int j = 0; j < degree; j++) { r[j] = Mul(r[j], root); if (j + 1 < degree) r[j] ^= r[j + 1]; }
                root = Mul(root, 2);
            }
            return r;
        }
        static byte[] Remainder(byte[] data, int from, int len, byte[] div)
        {
            var r = new byte[div.Length];
            for (int k = 0; k < len; k++)
            {
                byte factor = (byte)(data[from + k] ^ r[0]);
                Array.Copy(r, 1, r, 0, r.Length - 1); r[r.Length - 1] = 0;
                for (int i = 0; i < r.Length; i++) r[i] ^= Mul(div[i], factor);
            }
            return r;
        }
        static byte[] WithEcc(byte[] data, int ver)
        {
            int nb = Blocks[ver], ecc = EccPerBlock[ver], raw = RawModules(ver) / 8;
            int nShort = nb - raw % nb, shortLen = raw / nb;
            byte[] div = Divisor(ecc);
            var blocks = new byte[nb][];
            for (int i = 0, k = 0; i < nb; i++)
            {
                int dl = shortLen - ecc + (i < nShort ? 0 : 1);
                var b = new byte[shortLen + 1];
                Array.Copy(data, k, b, 0, dl);
                byte[] rem = Remainder(data, k, dl, div);
                Array.Copy(rem, 0, b, b.Length - ecc, ecc);
                k += dl; blocks[i] = b;
            }
            var all = new byte[raw]; int n = 0;
            for (int i = 0; i <= shortLen; i++)
                for (int j = 0; j < nb; j++)
                    if (i != shortLen - ecc || j >= nShort) all[n++] = blocks[j][i];   // (the short blocks have one data byte fewer: that place skipped)
            return all;
        }

        // ---- the fixed patterns ----
        static void Set(bool[,] m, bool[,] fn, int x, int y, bool dark) { m[y, x] = dark; fn[y, x] = true; }
        static void Fixed(bool[,] m, bool[,] fn, int ver, int size)
        {
            for (int i = 0; i < size; i++) { Set(m, fn, 6, i, i % 2 == 0); Set(m, fn, i, 6, i % 2 == 0); }   // the timing lines
            Finder(m, fn, 3, 3, size); Finder(m, fn, size - 4, 3, size); Finder(m, fn, 3, size - 4, size);
            if (ver >= 2)
            {
                int n = ver / 7 + 2, step = ver == 32 ? 26 : (ver * 4 + n * 2 + 1) / (n * 2 - 2) * 2;
                var pos = new int[n]; pos[0] = 6;
                for (int i = n - 1, p = ver * 4 + 10; i >= 1; i--, p -= step) pos[i] = p;
                for (int i = 0; i < n; i++)
                    for (int j = 0; j < n; j++)
                    {
                        if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue;   // (the finders' corners)
                        for (int dy = -2; dy <= 2; dy++) for (int dx = -2; dx <= 2; dx++) Set(m, fn, pos[i] + dx, pos[j] + dy, Math.Max(Math.Abs(dx), Math.Abs(dy)) != 1);
                    }
            }
            Format(m, fn, 0, size);   // (its places kept; the real bits come with the mask)
            if (ver >= 7)
            {
                int rem = ver; for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
                int v = ver << 12 | rem;
                for (int i = 0; i < 18; i++) { bool bit = ((v >> i) & 1) != 0; int a = size - 11 + i % 3, b = i / 3; Set(m, fn, a, b, bit); Set(m, fn, b, a, bit); }
            }
        }
        static void Finder(bool[,] m, bool[,] fn, int cx, int cy, int size)
        {
            for (int dy = -4; dy <= 4; dy++)
                for (int dx = -4; dx <= 4; dx++)
                {
                    int d = Math.Max(Math.Abs(dx), Math.Abs(dy)), x = cx + dx, y = cy + dy;
                    if (x >= 0 && x < size && y >= 0 && y < size) Set(m, fn, x, y, d != 2 && d != 4);
                }
        }
        static void Format(bool[,] m, bool[,] fn, int mask, int size)
        {
            int data = 1 << 3 | mask, rem = data;   // (level L is 01)
            for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
            int v = (data << 10 | rem) ^ 0x5412;
            Func<int, bool> bit = i => ((v >> i) & 1) != 0;
            for (int i = 0; i <= 5; i++) Set(m, fn, 8, i, bit(i));
            Set(m, fn, 8, 7, bit(6)); Set(m, fn, 8, 8, bit(7)); Set(m, fn, 7, 8, bit(8));
            for (int i = 9; i < 15; i++) Set(m, fn, 14 - i, 8, bit(i));
            for (int i = 0; i < 8; i++) Set(m, fn, size - 1 - i, 8, bit(i));
            for (int i = 8; i < 15; i++) Set(m, fn, 8, size - 15 + i, bit(i));
            Set(m, fn, 8, size - 8, true);
        }

        // ---- the data, in its zigzag from the bottom right ----
        static void Place(bool[,] m, bool[,] fn, byte[] data, int size)
        {
            int i = 0;
            for (int right = size - 1; right >= 1; right -= 2)
            {
                if (right == 6) right = 5;
                for (int vert = 0; vert < size; vert++)
                    for (int j = 0; j < 2; j++)
                    {
                        int x = right - j; bool up = ((right + 1) & 2) == 0; int y = up ? size - 1 - vert : vert;
                        if (!fn[y, x] && i < data.Length * 8) { m[y, x] = ((data[i >> 3] >> (7 - (i & 7))) & 1) != 0; i++; }
                    }
            }
        }
        static void Mask(bool[,] m, bool[,] fn, int mask, int size)
        {
            for (int y = 0; y < size; y++)
                for (int x = 0; x < size; x++)
                {
                    if (fn[y, x]) continue;
                    bool inv;
                    switch (mask)
                    {
                        case 0: inv = (x + y) % 2 == 0; break;
                        case 1: inv = y % 2 == 0; break;
                        case 2: inv = x % 3 == 0; break;
                        case 3: inv = (x + y) % 3 == 0; break;
                        case 4: inv = (x / 3 + y / 2) % 2 == 0; break;
                        case 5: inv = x * y % 2 + x * y % 3 == 0; break;
                        case 6: inv = (x * y % 2 + x * y % 3) % 2 == 0; break;
                        default: inv = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                    }
                    if (inv) m[y, x] = !m[y, x];
                }
        }

        // how hard a pattern is to read (the standard's four counts): runs, blocks, finder look-alikes, balance
        static int Penalty(bool[,] m, int size)
        {
            int p = 0, dark = 0;
            for (int pass = 0; pass < 2; pass++)
                for (int a = 0; a < size; a++)
                {
                    int run = 0; bool last = false; int hist = 0;   // hist: the last 11 modules of this line as bits
                    for (int b = 0; b < size; b++)
                    {
                        bool c = pass == 0 ? m[a, b] : m[b, a];
                        if (b > 0 && c == last) { run++; if (run == 5) p += 3; else if (run > 5) p++; } else { run = 1; last = c; }
                        hist = ((hist << 1) | (c ? 1 : 0)) & 0x7FF;
                        if (b >= 10 && (hist == 0x05D || hist == 0x5D0)) p += 40;   // 1:1:3:1:1 beside four light ones
                        if (pass == 0 && c) dark++;
                    }
                }
            for (int y = 0; y + 1 < size; y++)
                for (int x = 0; x + 1 < size; x++)
                    if (m[y, x] == m[y, x + 1] && m[y, x] == m[y + 1, x] && m[y, x] == m[y + 1, x + 1]) p += 3;
            int total = size * size, k = (Math.Abs(dark * 20 - total * 10) + total - 1) / total - 1;
            return p + Math.Max(0, k) * 10;
        }

        // ---- what the code says: a link that opens a new issue with the report already written ----
        public const string IssuesUrl = "https://github.com/betterstremio/origin/issues/new";

        // the code as the title, the lines as the text; shortened from the end until it fits maxBytes of link
        public static string ReportUrl(string code, IList<string> lines, int maxBytes)
        {
            var body = new List<string>(lines ?? new string[0]);
            for (;;)
            {
                string url = IssuesUrl + "?title=" + Esc("Start code " + code) + "&body=" + Esc(string.Join("\n", body.ToArray()) + "\n\nWhat happened: ");
                if (Encoding.UTF8.GetByteCount(url) <= maxBytes || body.Count == 0) return url;
                int last = body.Count - 1;
                if (body[last].Length > 60) body[last] = body[last].Substring(0, body[last].Length / 2); else body.RemoveAt(last);
            }
        }
        static string Esc(string s)
        {
            var sb = new StringBuilder();
            foreach (byte b in Encoding.UTF8.GetBytes(s ?? ""))
            {
                char c = (char)b;
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') sb.Append(c);
                else sb.Append('%').Append(b.ToString("X2"));
            }
            return sb.ToString();
        }

        // the code as a picture file's bytes (24-bit BMP, bottom row first), each module `scale` pixels, a light margin of 4 modules
        public static byte[] Bmp(bool[,] m, int scale)
        {
            int n = m.GetLength(0), side = (n + 8) * scale, row = (side * 3 + 3) & ~3, size = 54 + row * side;
            var f = new byte[size];
            f[0] = (byte)'B'; f[1] = (byte)'M'; Le(f, 2, size); Le(f, 10, 54); Le(f, 14, 40); Le(f, 18, side); Le(f, 22, side); f[26] = 1; f[28] = 24; Le(f, 34, row * side);
            for (int y = 0; y < side; y++)
                for (int x = 0; x < side; x++)
                {
                    int mx = x / scale - 4, my = (side - 1 - y) / scale - 4;
                    bool dark = mx >= 0 && my >= 0 && mx < n && my < n && m[my, mx];
                    int o = 54 + y * row + x * 3; byte v = dark ? (byte)0 : (byte)255; f[o] = v; f[o + 1] = v; f[o + 2] = v;
                }
            return f;
        }
        static void Le(byte[] f, int at, int v) { f[at] = (byte)v; f[at + 1] = (byte)(v >> 8); f[at + 2] = (byte)(v >> 16); f[at + 3] = (byte)(v >> 24); }
    }
}
