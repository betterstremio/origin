// The host for Samsung TVs from 2021 on. One source, three builds, each against its own reference set:
//   HOST_B  Tizen 6.0      a GL window over the app's main window; the frame callback returns nothing
//   HOST_C  Tizen 6.5      the same window, its newer setup; the frame callback says whether it drew
//   HOST_D  Tizen 7+       a GL view inside the main window (built against the Tizen 7 level, where the view first exists)
// What a host does: load the app's library with the system's loader, give it a GL surface (it calls sb_host_frame
// once per frame with the context current), pass keys and front/back, hand over the main window for the player's
// picture, and show a code when something fails. Settings are fixed here at build time; nothing adapts at run time.
// Nothing optional happens before the first frames: key grabs wait until the app has drawn thirty.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Tizen.NUI;
using Tizen.NUI.BaseComponents;
using Path = System.IO.Path;   // (the scene toolkit has a Path of its own)

#pragma warning disable 162, 414          // the build-time settings below make some branches constant on purpose

namespace StremioOrigin.Host
{
    static class Build
    {
#if HOST_B
        public const char Letter = 'B';
        public const bool MainTransparent = false;   // the main window covers what was on screen before
        public const bool MainOpaqueState = true;
        public const bool NoVsyncWait = false;       // frames come on the main thread; the system swaps each one
        public const bool RaiseOnResume = false;
#elif HOST_C
        public const char Letter = 'C';
        public const bool MainTransparent = true;    // (some 2022 sets show black behind an opaque main window)
        public const bool MainOpaqueState = false;
        public const bool NoVsyncWait = true;        // own render thread: the app paces itself
        public const bool RaiseOnResume = true;
#else
        public const char Letter = 'D';
        public const bool MainTransparent = true;
        public const bool MainOpaqueState = true;    // one window: it counts as covering the TV's own screen, which showed through every clear pixel otherwise
        public const bool NoVsyncWait = true;
        public const bool RaiseOnResume = false;     // one window: nothing of ours to raise
#endif
        public const int W = 1920, H = 1080;
        public const int FramesBeforeExtras = 30;    // key grabs and the like wait for this many drawn frames
        public const int ForcedEndMs = 4000;         // a process still alive this long after its exit is ended
#if HOST_B
        public const int FrameWaitMs = 0;            // frames come on the main thread and each is swapped: the app's frame is waited for
#else
        public const int FrameWaitMs = 50;           // frames come on the system's drawing thread: a frame of the app's later than this is skipped, not waited for
#endif
    }

    // fail reasons of these hosts (the picker uses 01-09; see StartCode)
    static class Why
    {
        public const int NoLibrary = 10, LibraryLoad = 11, MissingExport = 12, InitFailed = 13, NoSurface = 14, HostError = 19;
    }

    // The app's library: found beside the package's own files, opened with the system's loader, its calls by name.
    static class Lib
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int InitFn(string art, string data, int w, int h);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int FrameFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void KeyFn(string name, int down);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void LifeFn(int front);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void WindowFn(IntPtr native);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr ErrorFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void ConfigFn(int waitMs);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int PlayingFn();

        public static InitFn Init; public static FrameFn Frame; public static KeyFn Key; public static LifeFn Life;
        public static WindowFn Window; public static ErrorFn Error; public static ConfigFn Config; public static PlayingFn Playing;

        [DllImport("libdl.so.2")] static extern IntPtr dlopen(string path, int flags);
        [DllImport("libdl.so.2")] static extern IntPtr dlsym(IntPtr handle, string name);
        [DllImport("libdl.so.2")] static extern IntPtr dlerror();
        const int RTLD_NOW = 2, RTLD_GLOBAL = 0x100;

        // every copy of the library in the package, the usual place first. Which folder a TV lets code be loaded from
        // is the firmware's own choice (some refuse lib/ and allow res/), so each is tried.
        public static List<string> Find(string resource)
        {
            var found = new List<string>();
            string root = Path.GetFullPath(Path.Combine(resource, ".."));
            foreach (string p in new[] { Path.Combine(root, "lib", "libstremioorigin.so"), Path.Combine(resource, "libstremioorigin.so") })
                if (File.Exists(p)) found.Add(p);
            return found;
        }

        // null = loaded; else why not (reason set)
        public static string Load(List<string> paths, out int reason, out string from)
        {
            reason = 0; from = null;
            IntPtr h = IntPtr.Zero; string refused = null;
            foreach (string path in paths)
            {
                dlerror();
                h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
                if (h != IntPtr.Zero) { from = Path.GetFileName(Path.GetDirectoryName(path)); break; }
                IntPtr e = dlerror();
                string said = e == IntPtr.Zero ? path + ": refused without a message" : Marshal.PtrToStringAnsi(e);
                refused = refused == null ? said : refused + " | " + said;
            }
            if (h == IntPtr.Zero) { reason = Why.LibraryLoad; return refused ?? "the system's loader refused it"; }
            string miss = null;
            Init = Get<InitFn>(h, "sb_host_init", ref miss); Frame = Get<FrameFn>(h, "sb_host_frame", ref miss);
            Key = Get<KeyFn>(h, "sb_host_key", ref miss); Error = Get<ErrorFn>(h, "sb_host_error", ref miss);
            string optional = null;
            Life = Get<LifeFn>(h, "sb_host_life", ref optional); Window = Get<WindowFn>(h, "sb_host_window", ref optional);
            Config = Get<ConfigFn>(h, "sb_host_config", ref optional); Playing = Get<PlayingFn>(h, "sb_host_playing", ref optional);
            if (miss != null) { reason = Why.MissingExport; return miss; }
            return null;
        }

        static T Get<T>(IntPtr h, string name, ref string missing) where T : class
        {
            IntPtr p = dlsym(h, name);
            if (p == IntPtr.Zero) { if (missing == null) missing = name; return null; }
            return Marshal.GetDelegateForFunctionPointer(p, typeof(T)) as T;
        }
    }

    // The same start trace the picker and the app write: "<unix time> <stage> <detail>", one append a line.
    static class Trace
    {
        static readonly DateTime Epoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);
        static string path;
        public static void Open(string dataDir) { path = Path.Combine(dataDir, "start-trace.log"); }
        public static void Mark(string stage, string detail)
        {
            if (path == null) return;
            try
            {
                double t = (DateTime.UtcNow - Epoch).TotalSeconds;
                byte[] b = Encoding.UTF8.GetBytes(t.ToString("F3", System.Globalization.CultureInfo.InvariantCulture) + " " + stage
                                                  + (string.IsNullOrEmpty(detail) ? "" : " " + detail.Replace('\n', ' ').Replace('\r', ' ')) + "\n");
                using (var f = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite)) f.Write(b, 0, b.Length);
            }
            catch { }
        }
        public static List<string> All()
        {
            var lines = new List<string>();
            try { foreach (string l in File.ReadAllLines(path)) { int sp = l.IndexOf(' '); lines.Add(sp < 0 ? l : l.Substring(sp + 1)); } }
            catch { }
            return lines;
        }

        public static List<string> ThisStart()
        {
            try
            {
                var stages = new List<string>();
                foreach (string l in File.ReadAllLines(path)) { int sp = l.IndexOf(' '); if (sp > 0) stages.Add(l.Substring(sp + 1)); }
                var starts = StremioOrigin.Start.StartCode.Starts(stages);
                return starts.Count == 0 ? stages : starts[starts.Count - 1];
            }
            catch { return new List<string>(); }
        }
    }

    public sealed class Program : NUIApplication
    {
        [DllImport("libecore_wl2.so.1")] static extern IntPtr ecore_wl2_window_find(uint id);
        [DllImport("libecore_wl2.so.1")] static extern byte ecore_wl2_window_keygrab_set(IntPtr win, string key, int mod, int notMod, int priority, int mode);
        [DllImport("libEGL.so.1")] static extern IntPtr eglGetCurrentDisplay();
        [DllImport("libEGL.so.1")] static extern uint eglSwapInterval(IntPtr display, int interval);
        const int KeygrabTopmost = 2;

        // the remote's keys the TV would otherwise keep for itself (names as the app's bridge knows them)
        static readonly string[] GrabbedKeys = {
            "XF86AudioPlay", "XF86AudioPause", "XF86AudioStop", "XF86AudioRewind", "XF86AudioForward", "XF86PlayBack",
            "XF86RaiseChannel", "XF86LowerChannel", "XF86Red", "XF86Green", "XF86Yellow", "XF86Blue" };

        Window main; Timer tick;
#if HOST_D
        GLView surface;
#else
        GLWindow surface;
#endif
        string model = "?", tizen = "?";
        volatile bool running, appEnded, failed; volatile int frames; bool extrasDone, leaving;
        volatile bool mainVisible = true, surfaceVisible = true;
        bool keySeen;                               // a key has gone to the app
        string dataDir; static int textWidth = Build.W;

        Program() : base("", Build.MainTransparent ? WindowMode.Transparent : WindowMode.Opaque) { }

        public static void Main(string[] args) { new Program().Run(args); }

        protected override void OnCreate()
        {
            base.OnCreate();
            try { Start(); }
            catch (Exception e) { Fail(Why.HostError, e.GetType().Name + ": " + e.Message); }
        }

        void Start()
        {
            var dir = Tizen.Applications.Application.Current.DirectoryInfo;
            Trace.Open(dir.Data); dataDir = dir.Data;
            try { string v; if (Tizen.System.Information.TryGetValue("http://tizen.org/feature/platform.version", out v)) tizen = v; } catch { }
            try { string m; if (Tizen.System.Information.TryGetValue("http://tizen.org/system/model_name", out m)) model = m; } catch { }
            Trace.Mark("start", Build.Letter + " " + model + " tizen " + tizen);

            main = Window.Instance;
            main.BackgroundColor = Build.MainTransparent ? Color.Transparent : Color.Black;
            if (Build.MainOpaqueState) { try { main.SetOpaqueState(true); } catch { } }
            main.KeyEvent += (o, e) => OnKey(e.Key);
            try { main.VisibilityChanged += (o, e) => { mainVisible = e.Visibility; if (!e.Visibility) ToBack(); }; } catch { }

            // the start before ran 10 s and showed no picture (a black screen): its code first, then this start on a key
            List<string> before = null;
            try { before = StremioOrigin.Start.StartCode.BlankBefore(Trace.All()); } catch { }
            if (before != null)
            {
                Trace.Mark("note", "the start before showed no picture: its code shown");
                try
                {
                    var su = StremioOrigin.Start.StartCode.Summarise(before);
                    string code = StremioOrigin.Start.StartCode.Format(su.Host, su.Stage, StremioOrigin.Start.StartCode.NoPicture);
                    var lines = StremioOrigin.Start.FailLayout.Lines(code, "The last start showed no picture", "TV " + model + " / Tizen " + tizen, null, before, "Press OK to start");
                    string file = Path.Combine(dataDir, "start-screen.png");
                    File.WriteAllBytes(file, StremioOrigin.Start.FailLayout.Make(lines, Build.W, Build.H, StremioOrigin.Start.FailLayout.Report(code, lines)).Png());
                    notice = new ImageView(file) { Size2D = new Size2D(Build.W, Build.H), Position2D = new Position2D(0, 0) };
                    main.BackgroundColor = Color.Black; main.GetDefaultLayer().Add(notice);
                    return;   // (a key goes on with StartRest)
                }
                catch { notice = null; }
            }
            StartRest(dir);
        }

        ImageView notice;   // the start before showed no picture: its code is up, a key starts this one

        void StartRest(Tizen.Applications.DirectoryInfo dir)
        {

            List<string> so = Lib.Find(dir.Resource);
            if (so.Count == 0) { Fail(Why.NoLibrary, "libstremioorigin.so is not in the package"); return; }
            int why; string from; string err = Lib.Load(so, out why, out from);
            if (err != null) { Fail(why, err); return; }
            Trace.Mark("library", "system " + from);

            // the player's picture goes under the app's main window: the app is told which one that is
            try { IntPtr native = ecore_wl2_window_find((uint)main.GetNativeId()); if (native != IntPtr.Zero && Lib.Window != null) Lib.Window(native); } catch { }

            int w = Build.W, h = Build.H;
            try { Size2D s = main.WindowSize; if (s != null && s.Width >= 640 && s.Height >= 360) { w = s.Width; h = s.Height; } } catch { }
            if (Lib.Init(Path.Combine(dir.Resource, "art"), dir.Data, w, h) != 0)
            { IntPtr e = Lib.Error(); Fail(Why.InitFailed, (e == IntPtr.Zero ? null : Marshal.PtrToStringAnsi(e)) ?? "sb_host_init said no"); return; }
            if (Build.FrameWaitMs > 0 && Lib.Config != null) { try { Lib.Config(Build.FrameWaitMs); } catch { } }
            running = true;

            try { MakeSurface(w, h); }
            catch (Exception e) { running = false; Fail(Why.NoSurface, e.GetType().Name + ": " + e.Message); return; }

            // the main thread's small jobs: leave when the app has ended, and the extras once frames are steady
            tick = new Timer(100);
            tick.Tick += (o, e) => { Housekeeping(); return true; };
            tick.Start();

            // a start with no picture 10 s on is noted, for the next start to show its code (a frame later clears it);
            // a timer of its own thread, so a main loop that has stopped does not stop it
            blankWatch = new System.Threading.Timer(_ =>
            {
                try
                {
                    bool frame = false; foreach (string l in Trace.ThisStart()) if (l.StartsWith("frame")) frame = true;
                    if (!frame && !failed) Trace.Mark("blank", "no picture 10 s after the start");
                }
                catch { }
            }, null, 10000, System.Threading.Timeout.Infinite);
        }
        System.Threading.Timer blankWatch;

        void MakeSurface(int w, int h)
        {
#if HOST_D
            surface = new GLView(GLView.ColorFormat.RGBA8888) { Size2D = new Size2D(w, h), Position2D = new Position2D(0, 0) };
            surface.SetGraphicsConfig(false, false, 0, GLESVersion.Version20);
            surface.RenderingMode = GLRenderingMode.Continuous;
            surface.BackgroundColor = Color.Transparent;
            surface.RegisterGLCallbacks(GlInit, GlFrame, GlEnd);
            // (the view's own mode does not keep the window it sits in drawing: a still picture over the video would stop getting frames)
            try { main.RenderingBehavior = RenderingBehaviorType.Continuously; } catch { }
            main.GetDefaultLayer().Add(surface);
#else
            surface = new GLWindow("StremioOrigin", new Rectangle(0, 0, w, h), true);
#if HOST_B
            surface.SetEglConfig(false, false, 0, GLWindow.GLESVersion.Version_2_0);
#else
            surface.SetEglConfig(false, false, 0, GLESVersion.Version20);
            surface.RenderingMode = GLRenderingMode.Continuous;
#endif
            surface.RegisterGlCallback(GlInit, GlFrame, GlEnd);
            surface.KeyEvent += (o, e) => OnKey(e.Key);
            try { surface.VisibilityChanged += (o, e) => { surfaceVisible = e.Visibility; if (!e.Visibility) ToBack(); }; } catch { }
            surface.Show();
#endif
        }

        // ---- the surface's own callbacks (its drawing thread: the main one on HOST_B, a render thread otherwise) ----
        void GlInit()
        {
            if (Build.NoVsyncWait) { try { eglSwapInterval(eglGetCurrentDisplay(), 0); } catch { } }
        }

#if HOST_B
        void GlFrame() { DrawOne(); }
#else
        int GlFrame() { return DrawOne() ? 1 : 0; }
#endif
        void GlEnd() { }

        bool DrawOne()
        {
            if (!running || appEnded) return false;
            int r;
            try { r = Lib.Frame(); }
            catch { r = -1; }
            if (r < 0) { appEnded = true; return false; }   // the app's loop has returned: the main thread closes the host
            if (r == 0) return false;                        // the app was late for this frame (only with a wait limit): nothing new to show
            if (frames < int.MaxValue) frames++;
            return true;
        }

        // ---- the main thread ----
        void Housekeeping()
        {
            if (appEnded && !leaving) { Leave(); return; }
#if HOST_D
            try { SilentClip(); } catch (Exception e) { clipStep = 9; Trace.Mark("note", "silent clip failed: " + e.GetType().Name + " " + e.Message); }
#endif
            if (!extrasDone && frames >= Build.FramesBeforeExtras)
            {
                extrasDone = true;
                try
                {
                    IntPtr native = ecore_wl2_window_find((uint)main.GetNativeId());
                    if (native != IntPtr.Zero) foreach (string k in GrabbedKeys) ecore_wl2_window_keygrab_set(native, k, 0, 0, 0, KeygrabTopmost);
                }
                catch { }
            }
        }

#if HOST_D
        // ---- the silent clip ----
        // On the sets from 2024 the TV's own channels app keeps sounding under an app until that app plays something.
        // So 1.5 s after the app's first picture one second of silence (res/silence.wav) is played, with no picture,
        // and its player closed when it ends, 5 s on at the latest. Once a start; never beside a film or trailer of the
        // app's. Driven from the main thread's 100 ms tick, nothing waited for; a failure is one note and nothing else.
        int clipStep, clipTicks; DateTime clipAt; object clipPlayer; System.Threading.Tasks.Task clipReady; volatile bool clipEnded;

        void SilentClip()
        {
            if (clipStep == 9) return;
            if (leaving || failed || !running) { if (clipStep >= 2) ClipClose(); clipStep = 9; return; }
            if (clipStep == 0)
            {   // the app's first picture: its line in the start trace (the frames counted here include the black ones before it)
                if (frames == 0 || ++clipTicks % 5 != 0) return;
                if (clipTicks > 600) { clipStep = 9; return; }
                foreach (string l in Trace.ThisStart()) if (l.StartsWith("frame")) { clipStep = 1; clipAt = DateTime.UtcNow.AddMilliseconds(1500); break; }
                return;
            }
            if (clipStep == 1)
            {
                if (DateTime.UtcNow < clipAt) return;
                // the app's player in use, or (a library that cannot say) a key already pressed: not played at all
                bool busy = Lib.Playing != null ? Lib.Playing() != 0 : keySeen;
                if (busy) { clipStep = 9; Trace.Mark("note", "silent clip skipped: the player is in use"); return; }
                string file = Path.Combine(Tizen.Applications.Application.Current.DirectoryInfo.Resource, "silence.wav");
                if (!File.Exists(file)) { clipStep = 9; Trace.Mark("note", "silent clip skipped: no file"); return; }
                clipStep = 2; clipAt = DateTime.UtcNow.AddMilliseconds(5000);
                ClipOpen(file);
                return;
            }
            bool late = DateTime.UtcNow >= clipAt;
            if (clipStep == 2 && clipReady != null && clipReady.IsCompleted)
            {
                if (clipReady.IsFaulted || clipReady.IsCanceled)
                {
                    Exception x = clipReady.Exception == null ? null : clipReady.Exception.GetBaseException();
                    ClipClose(); clipStep = 9; Trace.Mark("note", "silent clip failed: " + (x == null ? "cancelled" : x.GetType().Name + " " + x.Message)); return;
                }
                if (Lib.Playing != null && Lib.Playing() != 0) { ClipClose(); clipStep = 9; Trace.Mark("note", "silent clip dropped: the player is in use"); return; }
                ClipStart(); clipStep = 3; return;
            }
            if (late || (clipStep == 3 && clipEnded))
            {
                bool played = clipStep == 3;
                ClipClose(); clipStep = 9;
                Trace.Mark("note", played ? "silent clip played" : "silent clip failed: not ready in 5 s");
            }
        }

        // (the player's types only in these three, never inlined: a set without them fails here, inside SilentClip's catch)
        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
        void ClipOpen(string file)
        {
            var p = new Tizen.Multimedia.Player(); clipPlayer = p;
            p.PlaybackCompleted += (o, e) => { clipEnded = true; };
            p.SetSource(new Tizen.Multimedia.MediaUriSource(file));
            clipReady = p.PrepareAsync();
        }

        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
        void ClipStart() { ((Tizen.Multimedia.Player)clipPlayer).Start(); }

        [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
        void ClipCloseNow()
        {
            var p = (Tizen.Multimedia.Player)clipPlayer; clipPlayer = null;
            if (p == null) return;
            try { p.Stop(); } catch { }
            try { p.Unprepare(); } catch { }
            try { p.Dispose(); } catch { }
        }
        void ClipClose()
        {
            try { ClipCloseNow(); } catch { }
            // (a prepare left behind: its failure looked at, so it is not reported as never seen)
            try { if (clipReady != null && !clipReady.IsCompleted) clipReady.ContinueWith(t => { var x = t.Exception; }); } catch { }
            clipReady = null;
        }
#endif

        void OnKey(Key k)
        {
            if (k == null) return;
            bool down = k.State == Key.StateType.Down;
            if (failed) { if (!down) Leave(); return; }      // on the error screen any key leaves
            if (notice != null)
            {
                if (down) return;
                if (k.KeyPressedName == "XF86Back" || k.KeyPressedName == "Escape") { Leave(); return; }
                try { notice.Unparent(); notice.Dispose(); } catch { }
                notice = null;
                main.BackgroundColor = Build.MainTransparent ? Color.Transparent : Color.Black;
                try { StartRest(Tizen.Applications.Application.Current.DirectoryInfo); } catch (Exception e) { Fail(Why.HostError, e.GetType().Name + ": " + e.Message); }
                return;
            }
            if (!running) return;
            keySeen = true;
            try { Lib.Key(k.KeyPressedName, down ? 1 : 0); } catch { }
        }

        protected override void OnPause() { base.OnPause(); ToBack(); }

        // (a window of ours hidden: the app is told at once, some sets hide the window without a pause)
        void ToBack() { try { if (running && Lib.Life != null) Lib.Life(0); } catch { } }

        // The TV's menu opens and reopens an app with an app control, on some sets without a resume: our windows are
        // brought back then, the main one first when it was hidden (the player's picture is under it), the drawing
        // surface above it. On HOST_B only when one of them is hidden, so a first start is left alone.
        protected override void OnAppControlReceived(Tizen.Applications.AppControlReceivedEventArgs e)
        {
            try { base.OnAppControlReceived(e); } catch { }
#if !HOST_D
            if (failed || leaving || surface == null) return;
#if HOST_B
            if (mainVisible && surfaceVisible) return;
#endif
            try { var t = new Timer(300); t.Tick += (o, a) => { RaiseOurs(); return false; }; t.Start(); } catch { }
#endif
        }

#if !HOST_D
        void RaiseOurs()
        {
            if (failed || leaving || surface == null) return;
            if (!mainVisible) { try { main.Show(); main.Raise(); } catch { } }
            try { surface.Show(); surface.Raise(); } catch { }
        }
#endif

        protected override void OnResume()
        {
            base.OnResume();
            try { if (running && Lib.Life != null) Lib.Life(1); } catch { }
#if !HOST_D
            if (Build.RaiseOnResume) RaiseOurs();
#endif
        }

        protected override void OnTerminate() { running = false; ForceEndLater(); base.OnTerminate(); }

        void Leave()
        {
            if (leaving) return; leaving = true; running = false;
            Trace.Mark("exit", appEnded ? "the app ended" : "left from the error screen");
#if !HOST_D
            try { if (surface != null) surface.Hide(); } catch { }
#endif
            try { main.Hide(); } catch { }
            ForceEndLater();
            Exit();
        }

        // some sets keep a black screen up if the process lingers after its exit
        static void ForceEndLater()
        {
            var t = new System.Threading.Thread(() => { System.Threading.Thread.Sleep(Build.ForcedEndMs); Environment.Exit(0); });
            t.IsBackground = true; t.Start();
        }

        // ---- the error screen: the code, the TV, the stages reached; any key leaves ----
        void Fail(int reason, string detail)
        {
            failed = true; running = false;
            Trace.Mark("fail", reason.ToString("00") + " " + (detail ?? ""));
            try
            {
                List<string> stages = Trace.ThisStart();
                var s = StremioOrigin.Start.StartCode.Summarise(stages);
                string code = StremioOrigin.Start.StartCode.Format(Build.Letter, s.Stage, reason);
                if (main == null) main = Window.Instance;
                main.BackgroundColor = Color.Black;
                Layer root = main.GetDefaultLayer();
                // the screen as every failed start shows it (FailLayout), saved as one picture and shown as it is; only if
                // that cannot be done, the toolkit's own text with the code to scan beside it
                try
                {
                    if (dataDir != null)
                    {
                        var lines = StremioOrigin.Start.FailLayout.Lines(code, null, "TV " + model + " / Tizen " + tizen, null, stages);
                        byte[] png = StremioOrigin.Start.FailLayout.Make(lines, Build.W, Build.H, StremioOrigin.Start.FailLayout.Report(code, lines)).Png();
                        string whole = Path.Combine(dataDir, "start-screen.png");
                        File.WriteAllBytes(whole, png);
                        root.Add(new ImageView(whole) { Size2D = new Size2D(Build.W, Build.H), Position2D = new Position2D(0, 0) });
                        return;
                    }
                }
                catch { }
                int perLine = 110;
                try
                {
                    var text = new List<string> { "TV " + model + " / Tizen " + tizen }; text.AddRange(stages);
                    bool[,] m = dataDir == null ? null : StremioOrigin.Start.Qr.Encode(System.Text.Encoding.UTF8.GetBytes(StremioOrigin.Start.Qr.ReportUrl(code, text, 900)));
                    if (m != null)
                    {
                        int scale = Math.Max(3, Math.Min(6, 540 / (m.GetLength(0) + 8))), side = (m.GetLength(0) + 8) * scale;
                        string file = Path.Combine(dataDir, "start-report.bmp");
                        File.WriteAllBytes(file, StremioOrigin.Start.Qr.Bmp(m, scale));
                        root.Add(new ImageView(file) { Size2D = new Size2D(side, side), Position2D = new Position2D(Build.W - 80 - side, Build.H - 70 - side) });
                        textWidth = Build.W - 80 - side - 40; perLine = 70;
                    }
                }
                catch { textWidth = Build.W; perLine = 110; }
                root.Add(Line(code, 64, 380));
                root.Add(Line(model + "  ·  Tizen " + tizen, 28, 500));
                int y = 580, shown = 0;
                foreach (string st in stages)
                {
                    for (int at = 0; at < st.Length && at < 440 && shown < 12; at += perLine, shown++) { root.Add(Line(st.Substring(at, Math.Min(perLine, st.Length - at)), 18, y)); y += 34; }
                    if (shown >= 12) break;
                }
            }
            catch { }
        }

        static TextLabel Line(string text, float points, int y)
        {
            return new TextLabel(text)
            {
                TextColor = Color.White, PointSize = points, MultiLine = false, HorizontalAlignment = HorizontalAlignment.Center,
                Size2D = new Size2D(textWidth, (int)(points * 2.2f)), Position2D = new Position2D(0, y),
            };
        }
    }
}
