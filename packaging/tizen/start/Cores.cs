// "Cpus_allowed_list" from /proc/self/status -> how many cores: "3" -> 1, "0-3" -> 4, "0-1,4-7" -> 6.
namespace StremioOrigin.Start
{
    public static class CoresProbe
    {
        public static int Count(string list)
        {
            int n = 0;
            foreach (string part in (list ?? "").Split(','))
            {
                string p = part.Trim(); if (p.Length == 0) continue;
                int dash = p.IndexOf('-'); int a, b;
                if (dash < 0) { if (int.TryParse(p, out a)) n++; }
                else if (int.TryParse(p.Substring(0, dash), out a) && int.TryParse(p.Substring(dash + 1), out b) && b >= a) n += b - a + 1;
            }
            return n;
        }
    }
}
