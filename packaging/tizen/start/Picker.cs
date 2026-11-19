// The package's entry program. It picks the host for this TV and hands over; it draws nothing and loads no native
// code of ours. Order: the trace line first (so even a hard failure later leaves evidence), then the choice, then a
// check that the TV has what the chosen host needs, then the host's own entry point. Whatever goes wrong ends in the
// error screen with a code, never in a silent exit.
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace StremioOrigin.Start
{
    public static class Picker
    {
        const string HostFilePattern = "StremioOrigin.Host{0}.dll";   // beside this file: HostA, HostB, HostC, HostD
        const string ForceFile = "force-host";                         // development: data/force-host holding A, B, C or D

        public static int Main(string[] args)
        {
            string ver = null, model = null; char letter = 'X';
            try
            {
                string data = Tv.DataDir();
                Trace.Open(data);
                ver = Tv.Info("http://tizen.org/feature/platform.version");
                model = Tv.Info("http://tizen.org/system/model_name");
                Trace.Mark("picker", "tizen " + (ver ?? "?") + " model " + (model ?? "?") + " cores " + Tv.AllowedCores()
                                     + " runtime " + RuntimeInformation.FrameworkDescription);

                Choice c = HostChoice.Decide(ver, Tv.TypeExists);
                HostId forced = HostChoice.FromLetter(Tv.ReadSmall(data == null ? null : Path.Combine(data, ForceFile)));
                if (forced != HostId.None) { c = new Choice { Host = forced, Note = "forced" }; }
                if (c.Host == HostId.None) return Fail(letter, c.Reason, c.Note, ver, model);
                letter = HostChoice.Letter(c.Host);

                string looked;
                string file = Tv.FindBeside(string.Format(HostFilePattern, letter), args, data, out looked);
                if (file == null) return Fail(letter, StartCode.PickerNoHostFile, "no host " + letter + " in " + looked, ver, model);
                if (forced != HostId.None)
                { string miss = HostChoice.Missing(c.Host, Tv.TypeExists); if (miss != null) return Fail(letter, StartCode.PickerClassMissing, letter + " lacks " + miss, ver, model); }

                Assembly host;
                try { host = Assembly.LoadFrom(file); }
                catch (Exception e) { return Fail(letter, StartCode.PickerHostLoad, e.GetType().Name + ": " + e.Message, ver, model); }
                MethodInfo entry = host.EntryPoint;
                if (entry == null) return Fail(letter, StartCode.PickerHostEntry, Path.GetFileName(file), ver, model);

                Trace.Mark("pick", letter + " " + c.Note);
                try
                {
                    object r = entry.Invoke(null, entry.GetParameters().Length == 0 ? null : new object[] { args });
                    return r is int ? (int)r : 0;
                }
                catch (TargetInvocationException e)
                {
                    Exception x = e.InnerException ?? e;
                    return Fail(letter, StartCode.PickerHostThrew, x.GetType().Name + ": " + x.Message, ver, model);
                }
            }
            catch (Exception e)
            {
                return Fail(letter, StartCode.PickerError, e.GetType().Name + ": " + e.Message, ver, model);
            }
        }

        static int Fail(char letter, int reason, string detail, string ver, string model)
        {
            try { Trace.Mark("fail", reason.ToString("00") + " " + (detail ?? "")); } catch { }
            try
            {
                var s = StartCode.Summarise(Trace.ThisStart());
                string code = StartCode.Format(letter, s.Stage, reason);
                ErrorScreen.Show(code, model ?? "?", ver ?? "?", Trace.ThisStart(), Trace.Dir);   // returns when the viewer leaves; false = nothing could draw
            }
            catch { }
            return 1;
        }
    }

    // What the picker asks the TV. Every call is guarded and answers null / a default instead of throwing.
    static class Tv
    {
        public static bool TypeExists(string assemblyQualified)
        {
            try { return Type.GetType(assemblyQualified, false) != null; } catch { return false; }
        }

        // system information without a compile-time reference: Tizen.System.Information.TryGetValue<string>(key, out value)
        public static string Info(string key)
        {
            try
            {
                Type t = Type.GetType("Tizen.System.Information, Tizen.System.Information", false);
                if (t == null) return null;
                foreach (MethodInfo m in t.GetMethods(BindingFlags.Public | BindingFlags.Static))
                {
                    if (m.Name != "TryGetValue" || !m.IsGenericMethodDefinition) continue;
                    object[] a = { key, null };
                    object ok = m.MakeGenericMethod(typeof(string)).Invoke(null, a);
                    return ok is bool && (bool)ok ? a[1] as string : null;
                }
            }
            catch { }
            return null;
        }

        [DllImport("libcapi-appfw-app-common.so.0", EntryPoint = "app_get_data_path")] static extern IntPtr app_get_data_path();
        [DllImport("libc.so.6", EntryPoint = "free")] static extern void c_free(IntPtr p);

        [DllImport("libcapi-appfw-app-common.so.0", EntryPoint = "app_get_resource_path")] static extern IntPtr app_get_resource_path();

        public static string BinDir()
        {
            try { return Path.GetDirectoryName(typeof(Picker).GetTypeInfo().Assembly.Location); } catch { return "."; }
        }

        // a file packed beside this program (the package's bin/). Where this program says it is loaded from is not
        // always that folder: a TV may run its own compiled copy kept in a folder under bin/, or give no place at all.
        // So every place the package's bin/ can be told from is tried, in order; the first that holds the file wins.
        public static string FindBeside(string name, string[] args, string data, out string looked)
        {
            var dirs = new System.Collections.Generic.List<string>();
            Action<Func<string>> add = f =>
            {
                try
                {
                    string d = f(); if (string.IsNullOrEmpty(d)) return;
                    d = Path.GetFullPath(d); if (!dirs.Contains(d)) dirs.Add(d);
                }
                catch { }
            };
            add(() => BinDir());
            add(() => Path.Combine(BinDir(), ".."));
            add(() => AppContext.BaseDirectory);
            add(() => Path.Combine(AppContext.BaseDirectory, ".."));
            add(() => { IntPtr p = app_get_resource_path(); if (p == IntPtr.Zero) return null; string r = Marshal.PtrToStringAnsi(p); c_free(p); return string.IsNullOrEmpty(r) ? null : Path.Combine(r, "..", "bin"); });
            add(() => data == null ? null : Path.Combine(data, "..", "bin"));
            add(() => args != null && args.Length > 0 && args[0].IndexOf('/') >= 0 ? Path.GetDirectoryName(args[0]) : null);
            add(() => { string[] c = Environment.GetCommandLineArgs(); return c.Length > 0 && c[0].IndexOf('/') >= 0 ? Path.GetDirectoryName(c[0]) : null; });
            add(() => Directory.GetCurrentDirectory());
            add(() => Path.Combine(Directory.GetCurrentDirectory(), "bin"));
            foreach (string d in dirs)
            {
                try { string f = Path.Combine(d, name); if (File.Exists(f)) { looked = d; return f; } } catch { }
            }
            looked = dirs.Count == 0 ? "no folder known" : string.Join(" | ", dirs.ToArray());
            return null;
        }

        // the app's writable data folder, before any application object exists: the platform's own answer first,
        // else the folder beside bin/ (where the package's data link is)
        public static string DataDir()
        {
            try
            {
                IntPtr p = app_get_data_path();
                if (p != IntPtr.Zero) { string s = Marshal.PtrToStringAnsi(p); c_free(p); if (!string.IsNullOrEmpty(s) && Directory.Exists(s)) return s; }
            }
            catch { }
            try { string d = Path.GetFullPath(Path.Combine(BinDir(), "..", "data")); if (Directory.Exists(d)) return d; } catch { }
            return null;
        }

        public static string ReadSmall(string path)
        {
            try { if (path != null && File.Exists(path)) { string s = File.ReadAllText(path); return s.Length > 64 ? s.Substring(0, 64) : s; } } catch { }
            return null;
        }

        // how many cores this app may run on (a Samsung may give a third-party app one): "Cpus_allowed_list: 0-3" -> 4
        public static string AllowedCores()
        {
            try
            {
                foreach (string l in File.ReadAllLines("/proc/self/status"))
                    if (l.StartsWith("Cpus_allowed_list:")) return CoresProbe.Count(l.Substring(l.IndexOf(':') + 1)).ToString();
            }
            catch { }
            return "?";
        }
    }

    // The start trace, in the form the hosts and the app already write: "<unix time> <stage> <detail>", one append a line.
    static class Trace
    {
        static readonly DateTime Epoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);
        static string path;

        public static string Dir;   // the app's data folder (null when it could not be found)
        public static void Open(string dataDir) { Dir = dataDir; path = dataDir == null ? null : Path.Combine(dataDir, "start-trace.log"); }

        public static void Mark(string stage, string detail)
        {
            if (path == null) return;
            try
            {
                double t = (DateTime.UtcNow - Epoch).TotalSeconds;
                string line = t.ToString("F3", System.Globalization.CultureInfo.InvariantCulture) + " " + stage
                              + (string.IsNullOrEmpty(detail) ? "" : " " + detail.Replace('\n', ' ').Replace('\r', ' ')) + "\n";
                byte[] b = Encoding.UTF8.GetBytes(line);
                using (var f = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite)) f.Write(b, 0, b.Length);
            }
            catch { }
        }

        // the stages of the start now running (from its "picker" line on), without their times
        public static List<string> ThisStart()
        {
            var stages = new List<string>();
            try
            {
                if (path == null || !File.Exists(path)) return stages;
                foreach (string l in File.ReadAllLines(path)) { int sp = l.IndexOf(' '); if (sp > 0) stages.Add(l.Substring(sp + 1)); }
                var starts = StartCode.Starts(stages);
                return starts.Count == 0 ? new List<string>() : starts[starts.Count - 1];
            }
            catch { return stages; }
        }
    }
}
