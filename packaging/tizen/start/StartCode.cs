// The failure code: host letter (X = the picker) + the last stage reached + a reason number, e.g. "C4-12".
// Built from the start trace alone, so the picker, every host and the app's shared log all say the same thing.
using System;
using System.Collections.Generic;

namespace StremioOrigin.Start
{
    public static class StartCode
    {
        // picker reasons (hosts and the app number theirs from 10 up, in the same table when they are added)
        public const int Unknown = 0;             // the start ended without saying why (killed, crashed)
        public const int PickerTooOld = 1;        // Tizen below 4
        public const int PickerNoClass = 2;       // version unreadable and no known graphics class
        public const int PickerClassMissing = 3;  // the version's host needs a class this TV lacks
        public const int PickerNoHostFile = 4;    // no host for this TV in the package (yet)
        public const int PickerHostLoad = 5;      // the host file would not load
        public const int PickerHostEntry = 6;     // the host file has no entry point
        public const int PickerHostThrew = 7;     // the host threw before its loop ran
        public const int PickerError = 9;         // anything else in the picker
        public const int NoPicture = 30;          // the start before ran 10 s without a first picture (a "blank" line, no "frame")

        // The start before this one, when it ran 10 s without its first picture and showed nothing of its own (no fail
        // screen, no exit): its stages, else null. The current start is the last in the trace.
        public static List<string> BlankBefore(IList<string> stages)
        {
            var starts = Starts(stages);
            if (starts.Count < 2) return null;
            var prev = starts[starts.Count - 2];
            bool blank = false, frame = false, fail = false;
            foreach (string l in prev)
            {
                string st = l.Split(' ')[0];
                if (st == "blank") blank = true; else if (st == "frame") frame = true; else if (st == "fail") fail = true;
            }
            return blank && !frame && !fail ? prev : null;
        }

        // stage names as the trace writes them -> stage numbers
        public static int StageOf(string stage)
        {
            switch (stage)
            {
                case "picker": return 1;
                case "pick": case "start": return 2;      // "start": the host's own first line
                case "library": case "init": return 3;
                case "gl": return 4;
                case "frame": return 5;
                case "steady": return 6;
                case "key": return 7;
                case "player": return 8;
                case "picture": return 9;
            }
            return 0;
        }

        public struct Summary
        {
            public char Host;       // 'X' until a host was picked
            public int Stage;       // highest stage reached
            public int Reason;      // from a "fail <n> ..." line, else Unknown
            public bool Failed;     // a "fail" line was written
            public bool Ended;      // an "exit" line was written (the app closed itself)
            public string Code { get { return Format(Host, Stage, Reason); } }
            public bool Bad { get { return Failed || (!Ended && Stage < 6); } }   // died before thirty steady frames
        }

        public static string Format(char host, int stage, int reason)
        {
            if (stage < 0) stage = 0; if (stage > 9) stage = 9;
            if (reason < 0) reason = 0; if (reason > 99) reason = 99;
            return host.ToString() + stage.ToString() + "-" + reason.ToString("00");
        }

        // lines: the trace's stage text without its time ("picker 5.5 ...", "pick A", "frame ...", "fail 12 ...")
        public static Summary Summarise(IList<string> lines)
        {
            var s = new Summary { Host = 'X' };
            if (lines == null) return s;
            foreach (string raw in lines)
            {
                if (string.IsNullOrEmpty(raw)) continue;
                int sp = raw.IndexOf(' ');
                string stage = sp < 0 ? raw : raw.Substring(0, sp), rest = sp < 0 ? "" : raw.Substring(sp + 1).TrimStart();
                int n = StageOf(stage); if (n > s.Stage) s.Stage = n;
                if ((stage == "pick" || stage == "start") && rest.Length > 0)
                { char c = char.ToUpperInvariant(rest[0]); if (c >= 'A' && c <= 'D' && (rest.Length == 1 || rest[1] == ' ')) s.Host = c; }
                else if (stage == "exit") s.Ended = true;
                else if (stage == "fail")
                {
                    s.Failed = true; int v = 0, i = 0;
                    while (i < rest.Length && i < 2 && rest[i] >= '0' && rest[i] <= '9') { v = v * 10 + (rest[i] - '0'); i++; }
                    if (i > 0 && (i == rest.Length || rest[i] == ' ')) s.Reason = v;
                }
            }
            return s;
        }

        // the trace cut into starts: a new start begins at each "picker" line (or at "start" when no picker ran before it)
        public static List<List<string>> Starts(IList<string> stages)
        {
            var all = new List<List<string>>(); List<string> cur = null; bool pickerOpen = false;
            foreach (string l in stages)
            {
                string st = l; int sp = l.IndexOf(' '); if (sp >= 0) st = l.Substring(0, sp);
                if (st == "picker") { cur = new List<string>(); all.Add(cur); pickerOpen = true; }
                else if (st == "start") { if (!pickerOpen || cur == null) { cur = new List<string>(); all.Add(cur); } pickerOpen = false; }
                if (cur == null) { cur = new List<string>(); all.Add(cur); }
                cur.Add(l);
            }
            return all;
        }
    }
}
