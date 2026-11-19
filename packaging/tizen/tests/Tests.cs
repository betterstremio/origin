// Off-TV tests of the decision rule and the failure code. Plain console program: exit code 0 = all passed.
using System;
using System.Collections.Generic;
using StremioOrigin.Start;

static class Tests
{
    static int failed, run;
    static void Eq<T>(T got, T want, string what) { run++; if (!EqualityComparer<T>.Default.Equals(got, want)) { failed++; Console.WriteLine("FAIL " + what + ": got " + got + ", want " + want); } }

    // a TV that has exactly the classes of one generation
    static Func<string, bool> Has(params string[] have) { var set = new HashSet<string>(have); return t => set.Contains(t); }
    static readonly Func<string, bool> Tv45 = Has(HostChoice.NeedsA);
    static readonly Func<string, bool> Tv60 = Has("Tizen.NUI.GLWindow, Tizen.NUI", "Tizen.NUI.GLWindow+GLESVersion, Tizen.NUI");
    static readonly Func<string, bool> Tv65 = Has("Tizen.NUI.GLWindow, Tizen.NUI", "Tizen.NUI.GLESVersion, Tizen.NUI");
    static readonly Func<string, bool> Tv8 = Has("Tizen.NUI.GLWindow, Tizen.NUI", "Tizen.NUI.GLESVersion, Tizen.NUI", "Tizen.NUI.BaseComponents.GLView, Tizen.NUI");
    static readonly Func<string, bool> Nothing = t => false;
    static readonly Func<string, bool> Throws = t => { throw new InvalidOperationException(); };

    static int Main()
    {
        // version -> host, every version Samsung has shipped on TVs since 2018, and the ones to come
        var table = new Dictionary<string, HostId> {
            { "4.0", HostId.A }, { "5.0", HostId.A }, { "5.5", HostId.A }, { "6.0", HostId.B }, { "6.5", HostId.C },
            { "7.0", HostId.D }, { "8.0", HostId.D }, { "9.0", HostId.D }, { "10.0", HostId.D }, { "11.0", HostId.D }, { "25.0", HostId.D },
            { "5.5.0", HostId.A }, { " 6.5\n", HostId.C }, { "6", HostId.B }, { "6.4", HostId.B }, { "6.9", HostId.C }, { "7.9", HostId.D } };
        foreach (var kv in table)
        {
            int ma, mi; Eq(HostChoice.TryParseVersion(kv.Key, out ma, out mi), true, "parse '" + kv.Key + "'");
            Eq(HostChoice.ByVersion(ma, mi), kv.Value, "host for '" + kv.Key + "'");
        }
        foreach (string bad in new[] { null, "", " ", "abc", "v6.0", ".5", "6.", "-1", "0.9", "99999.0", "six" })
        { int ma, mi; Eq(HostChoice.TryParseVersion(bad, out ma, out mi), false, "unreadable '" + bad + "'"); }
        Eq(HostChoice.ByVersion(3, 0), HostId.None, "tizen 3"); Eq(HostChoice.ByVersion(2, 4), HostId.None, "tizen 2.4");

        // the whole decision on TVs that have what their version says
        Eq(HostChoice.Decide("5.5", Tv45).Host, HostId.A, "5.5 TV"); Eq(HostChoice.Decide("4.0", Tv45).Host, HostId.A, "4.0 TV");
        Eq(HostChoice.Decide("6.0", Tv60).Host, HostId.B, "6.0 TV"); Eq(HostChoice.Decide("6.5", Tv65).Host, HostId.C, "6.5 TV");
        Eq(HostChoice.Decide("7.0", Tv8).Host, HostId.D, "7.0 TV"); Eq(HostChoice.Decide("8.0", Tv8).Host, HostId.D, "8.0 TV");
        Eq(HostChoice.Decide("10.0", Tv8).Host, HostId.D, "10.0 TV");

        // the version's host or none: a missing class never moves the choice to another host
        var c = HostChoice.Decide("6.0", Tv65); Eq(c.Host, HostId.None, "6.0 without B's class"); Eq(c.Reason, StartCode.PickerClassMissing, "6.0 without B's class: reason");
        c = HostChoice.Decide("8.0", Tv65); Eq(c.Host, HostId.None, "8.0 without the view"); Eq(c.Reason, StartCode.PickerClassMissing, "8.0 without the view: reason");
        c = HostChoice.Decide("5.5", Nothing); Eq(c.Host, HostId.None, "5.5 without A's class"); Eq(c.Reason, StartCode.PickerClassMissing, "5.5 without A's class: reason");
        c = HostChoice.Decide("3.0", Tv45); Eq(c.Host, HostId.None, "tizen 3 TV"); Eq(c.Reason, StartCode.PickerTooOld, "tizen 3 TV: reason");
        c = HostChoice.Decide("5.5", Throws); Eq(c.Host, HostId.None, "type check throws"); Eq(c.Reason, StartCode.PickerClassMissing, "type check throws: reason");

        // version unreadable: the newest host whose class is there
        Eq(HostChoice.Decide(null, Tv45).Host, HostId.A, "no version, 4-5 classes"); Eq(HostChoice.Decide("", Tv60).Host, HostId.B, "no version, 6.0 classes");
        Eq(HostChoice.Decide("?", Tv65).Host, HostId.C, "no version, 6.5 classes"); Eq(HostChoice.Decide("x", Tv8).Host, HostId.D, "no version, 8 classes");
        c = HostChoice.Decide(null, Nothing); Eq(c.Host, HostId.None, "no version, no class"); Eq(c.Reason, StartCode.PickerNoClass, "no version, no class: reason");

        // letters
        Eq(HostChoice.Letter(HostId.A), 'A', "letter A"); Eq(HostChoice.Letter(HostId.D), 'D', "letter D"); Eq(HostChoice.Letter(HostId.None), 'X', "letter none");
        Eq(HostChoice.FromLetter("c\n"), HostId.C, "force c"); Eq(HostChoice.FromLetter(" B "), HostId.B, "force B"); Eq(HostChoice.FromLetter("E"), HostId.None, "force E");
        Eq(HostChoice.FromLetter(null), HostId.None, "force null"); Eq(HostChoice.FromLetter(""), HostId.None, "force empty");

        // failure codes from a trace
        var good = new List<string> { "picker tizen 5.5 model X cores 1 runtime .NET", "pick A tizen 5.5", "start A X tizen 5.5", "library own", "init the app's thread started", "gl 1920x1080, alpha 8 bits", "frame the app's first frame", "steady 30 more frames in 0.52 s", "key Return" };
        var s = StartCode.Summarise(good); Eq(s.Code, "A7-00", "a good start"); Eq(s.Bad, false, "a good start is not bad");
        var died = new List<string> { "picker tizen 6.5 model Y cores 4 runtime .NET", "pick C tizen 6.5", "start C Y tizen 6.5", "library system", "gl 1920x1080, alpha 8 bits" };
        s = StartCode.Summarise(died); Eq(s.Code, "C4-00", "died after gl"); Eq(s.Bad, true, "died after gl is bad");
        var failed12 = new List<string> { "picker tizen 8.0 model Z cores 4 runtime .NET", "pick D tizen 8.0", "start D Z tizen 8.0", "fail 12 the surface could not be made" };
        s = StartCode.Summarise(failed12); Eq(s.Code, "D2-12", "fail 12"); Eq(s.Bad, true, "fail is bad");
        var nohost = new List<string> { "picker tizen 9.0 model Q cores 4 runtime .NET", "fail 04 no host D in the package" };
        s = StartCode.Summarise(nohost); Eq(s.Code, "X1-04", "no host in the package"); Eq(StartCode.Format('D', 1, StartCode.PickerNoHostFile), "D1-04", "the picker names the host it wanted");
        var freeText = new List<string> { "start A X tizen 5.5", "fail The library could not be loaded" };
        s = StartCode.Summarise(freeText); Eq(s.Code, "A2-00", "a fail line without a number"); Eq(s.Failed, true, "a fail line without a number still fails");
        var closed = new List<string> { "picker tizen 5.5 model X cores 1 runtime .NET", "pick A tizen 5.5", "start A X tizen 5.5", "library own", "exit 0" };
        s = StartCode.Summarise(closed); Eq(s.Bad, false, "a start the viewer left early is not bad");
        Eq(StartCode.Summarise(null).Code, "X0-00", "no trace"); Eq(StartCode.Format('B', 42, 123), "B9-99", "clamped");

        // starts: picker lines open a start; a host's "start" line right after belongs to it; an old trace without a picker still splits
        var two = new List<string>(); two.AddRange(died); two.AddRange(good);
        var starts = StartCode.Starts(two); Eq(starts.Count, 2, "two starts"); Eq(StartCode.Summarise(starts[0]).Code, "C4-00", "first of two"); Eq(StartCode.Summarise(starts[1]).Code, "A7-00", "second of two");
        var old = new List<string> { "start A X tizen 5.5", "library own", "frame f", "start A X tizen 5.5", "library own" };
        Eq(StartCode.Starts(old).Count, 2, "an old trace without picker lines");
        Eq(StartCode.Starts(new List<string>()).Count, 0, "an empty trace");

        // the allowed-cores list
        Eq(StremioOrigin.Start.CoresProbe.Count("3"), 1, "one core"); Eq(StremioOrigin.Start.CoresProbe.Count(" 0-3"), 4, "four cores");
        Eq(StremioOrigin.Start.CoresProbe.Count("0-1,4-7"), 6, "two ranges"); Eq(StremioOrigin.Start.CoresProbe.Count(""), 0, "empty list"); Eq(StremioOrigin.Start.CoresProbe.Count("x"), 0, "garbage list");

        // the scannable code: the smallest version that holds the text, none past the largest, a report cut to its limit
        Eq(StremioOrigin.Start.Qr.Encode(new byte[1]).GetLength(0), 21, "one byte: the smallest code");
        Eq(StremioOrigin.Start.Qr.Encode(new byte[17]).GetLength(0), 21, "17 bytes still fit the smallest");
        Eq(StremioOrigin.Start.Qr.Encode(new byte[18]).GetLength(0), 25, "18 bytes take the next one");
        Eq(StremioOrigin.Start.Qr.Encode(new byte[2953]).GetLength(0), 177, "2953 bytes: the largest code");
        Eq(StremioOrigin.Start.Qr.Encode(new byte[2954]) == null ? 1 : 0, 1, "2954 bytes: no code");
        { var q = StremioOrigin.Start.Qr.Encode(System.Text.Encoding.ASCII.GetBytes("HELLO")); int n = q.GetLength(0);
          Eq(q[0, 0] && q[6, 6] && !q[1, 1] && q[3, 3] && q[0, n - 1] && q[n - 1, 0] && !q[7, 7] ? 1 : 0, 1, "the three corner marks");
          Eq(q[n - 8, 8] ? 1 : 0, 1, "the always-dark module"); }
        { var many = new List<string>(); for (int i = 0; i < 60; i++) many.Add("a line of the start record, number " + i + ", with a path /opt/usr/apps/x");
          string u = StremioOrigin.Start.Qr.ReportUrl("A2-20", many, 900);
          Eq(System.Text.Encoding.UTF8.GetByteCount(u) <= 900 ? 1 : 0, 1, "a long report is cut to the limit");
          Eq(u.StartsWith(StremioOrigin.Start.Qr.IssuesUrl + "?title=Start%20code%20A2-20&body=a%20line") ? 1 : 0, 1, "the link's start");
          Eq(StremioOrigin.Start.Qr.ReportUrl("B4-14", new List<string> { "x y", "é/&" }, 900).Contains("body=x%20y%0A%C3%A9%2F%26") ? 1 : 0, 1, "text made safe for a link"); }

        // every failed start's screen: laid out once, the text clear of the code's square, a picture of the screen's size
        { var ls = StremioOrigin.Start.FailLayout.Lines("A2-20", "what failed", "TV X / Tizen 5.5", new[] { new string('d', 400) }, new[] { "picker tizen 5.5", "fail 20 what failed" });
          Eq(ls.Count, 7, "the lines: code, title, TV, detail, two stages, the way out"); Eq(ls[0].Value, 18, "the code is the largest");
          var f = StremioOrigin.Start.FailLayout.Make(ls, 1920, 1080, StremioOrigin.Start.FailLayout.Report("A2-20", ls));
          Eq(f.QSide > 300 && f.Qx + f.QSide <= 1920 && f.Qy + f.QSide <= 1080 ? 1 : 0, 1, "the code's square is on the screen, bottom right");
          int over = 0; for (int i = 0; i + 3 < f.Light.Length; i += 4) if (f.Light[i] + f.Light[i + 2] > f.Qx && f.Light[i + 1] + f.Light[i + 3] > f.Qy) over++;
          Eq(over, 0, "no letter under the code's square");
          byte[] png = f.Png(); Eq(png.Length > 1000 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G' ? 1 : 0, 1, "the screen as a PNG");
          Eq((png[16] << 24 | png[17] << 16 | png[18] << 8 | png[19]) == 1920 && (png[20] << 24 | png[21] << 16 | png[22] << 8 | png[23]) == 1080 ? 1 : 0, 1, "of the screen's size"); }

        // the start before that showed no picture (a "blank" line, no "frame", no "fail"): its code comes first
        { var b = new List<string> { "picker tizen 5.5 model X", "pick A tizen 5.5", "start A X tizen 5.5", "library own", "init ok", "gl 1920x1080", "blank no picture 10 s after the start",
                                     "picker tizen 5.5 model X", "pick A tizen 5.5", "start A X tizen 5.5" };
          var prev = StartCode.BlankBefore(b); Eq(prev != null ? 1 : 0, 1, "a blank start before: found");
          var su = StartCode.Summarise(prev); Eq(StartCode.Format(su.Host, su.Stage, StartCode.NoPicture), "A4-30", "its code: host A, stage 4 (gl), reason 30");
          var f = new List<string>(b); f.Insert(7, "frame the app's first frame"); Eq(StartCode.BlankBefore(f) == null ? 1 : 0, 1, "a picture came after the note: nothing shown");
          var e = new List<string>(b); e.Insert(7, "fail 20 the library"); Eq(StartCode.BlankBefore(e) == null ? 1 : 0, 1, "a start that showed its own failure: nothing more");
          Eq(StartCode.BlankBefore(b.GetRange(0, 7)) == null ? 1 : 0, 1, "the blank start is this one: nothing yet");
          var ok = new List<string> { "picker tizen 5.5", "pick A", "start A X", "frame f", "steady s", "picker tizen 5.5", "pick A", "start A X" };
          Eq(StartCode.BlankBefore(ok) == null ? 1 : 0, 1, "a good start before: nothing");
          var ls = StremioOrigin.Start.FailLayout.Lines("A4-30", "t", "TV", null, prev, "Press OK to start"); Eq(ls[ls.Count - 1].Key, "Press OK to start", "its own last line"); }

        Console.WriteLine((failed == 0 ? "PASS " : "FAILED ") + (run - failed) + " of " + run);
        return failed == 0 ? 0 : 1;
    }
}
