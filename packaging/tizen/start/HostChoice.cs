// Which host a Samsung TV gets. One rule, no memory: the same TV always gets the same host.
// Nothing here touches the TV: a version string and a "does this type exist" question go in, a letter comes out,
// so every case is tested off the TV (tests/).
using System;

namespace StremioOrigin.Start
{
    public enum HostId { None = 0, A, B, C, D }

    public struct Choice
    {
        public HostId Host;     // None: no host can run here (Reason says why)
        public int Reason;      // 0 fine, else a picker reason (StartCode.Picker*)
        public string Note;     // for the trace: what decided it
    }

    public static class HostChoice
    {
        // What each host needs from the TV (type name, assembly). Checked before a host is loaded: a host whose
        // class the TV lacks is never started. B and C differ in where the GL version enum lives.
        public static readonly string[] NeedsA = { "Tizen.TV.NUI.GLApplication.TVGLApplication, Tizen.TV.NUI.GLApplication" };
        public static readonly string[] NeedsB = { "Tizen.NUI.GLWindow, Tizen.NUI", "Tizen.NUI.GLWindow+GLESVersion, Tizen.NUI" };
        public static readonly string[] NeedsC = { "Tizen.NUI.GLWindow, Tizen.NUI", "Tizen.NUI.GLESVersion, Tizen.NUI" };
        public static readonly string[] NeedsD = { "Tizen.NUI.BaseComponents.GLView, Tizen.NUI" };

        public static string[] Needs(HostId h)
        {
            switch (h) { case HostId.A: return NeedsA; case HostId.B: return NeedsB; case HostId.C: return NeedsC; case HostId.D: return NeedsD; }
            return new string[0];
        }

        public static char Letter(HostId h) { return h == HostId.None ? 'X' : (char)('A' + ((int)h - 1)); }

        public static HostId FromLetter(string s)
        {
            if (string.IsNullOrEmpty(s)) return HostId.None;
            switch (char.ToUpperInvariant(s.Trim().Length > 0 ? s.Trim()[0] : ' '))
            { case 'A': return HostId.A; case 'B': return HostId.B; case 'C': return HostId.C; case 'D': return HostId.D; }
            return HostId.None;
        }

        // "5.5", "6.0", "6.5.0", " 9.0\n", "10.0": the leading major.minor. Anything else is unreadable.
        public static bool TryParseVersion(string s, out int major, out int minor)
        {
            major = minor = 0;
            if (s == null) return false;
            int i = 0, n = s.Length;
            while (i < n && char.IsWhiteSpace(s[i])) i++;
            int start = i; long v = 0;
            while (i < n && s[i] >= '0' && s[i] <= '9' && i - start < 4) { v = v * 10 + (s[i] - '0'); i++; }
            if (i == start) return false;
            if (i < n && s[i] >= '0' && s[i] <= '9') return false;   // more than 4 digits: not a version
            major = (int)v;
            if (i < n && s[i] == '.')
            {
                i++; int ms = i; long m = 0;
                while (i < n && s[i] >= '0' && s[i] <= '9' && i - ms < 4) { m = m * 10 + (s[i] - '0'); i++; }
                if (i == ms) return false;                          // "6." is not a version
                minor = (int)m;
            }
            return major > 0;
        }

        // 4.0-5.5 -> A; 6.0 -> B; 6.5 -> C; 7 and later (versions not known yet too) -> D; below 4 -> none.
        // 7 goes to D, not C: Samsung's reference files for the Tizen 7 level no longer have the GL window calls C is
        // built on (they were renamed there), and already have the GL view D uses.
        public static HostId ByVersion(int major, int minor)
        {
            if (major < 4) return HostId.None;
            if (major <= 5) return HostId.A;
            if (major == 6) return minor < 5 ? HostId.B : HostId.C;
            return HostId.D;
        }

        public static string Missing(HostId h, Func<string, bool> typeExists)
        {
            foreach (string t in Needs(h)) { bool ok; try { ok = typeExists(t); } catch { ok = false; } if (!ok) return t; }
            return null;
        }

        public static Choice Decide(string version, Func<string, bool> typeExists)
        {
            var c = new Choice();
            int major, minor;
            if (TryParseVersion(version, out major, out minor))
            {
                HostId h = ByVersion(major, minor);
                if (h == HostId.None) { c.Reason = StartCode.PickerTooOld; c.Note = "tizen " + major + "." + minor; return c; }
                string miss = Missing(h, typeExists);
                if (miss != null) { c.Reason = StartCode.PickerClassMissing; c.Note = Letter(h) + " lacks " + miss; return c; }   // the version's host, or none: never another one
                c.Host = h; c.Note = "tizen " + major + "." + minor; return c;
            }
            // the version cannot be read: the newest host whose class the TV has
            foreach (HostId h in new[] { HostId.D, HostId.C, HostId.B, HostId.A })
                if (Missing(h, typeExists) == null) { c.Host = h; c.Note = "by class (version unreadable)"; return c; }
            c.Reason = StartCode.PickerNoClass; c.Note = "version unreadable, no known class"; return c;
        }
    }
}
