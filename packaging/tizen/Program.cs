// Adapted from the Tizen 4/5 host of Nuvio Legacy (github.com/iqui27/nuvio-native-legacy, tizen-tpk/NuvioTpk40/Program40.cs),
// GPL-3.0, copyright the Nuvio Legacy authors. Modified for Stremio Origin from 2026-11-08 (the app's own loader, keys,
// player and window handling); see README.md, Credits. Licensed under the GPL-3.0, as the rest of this app.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using Tizen.System;
using Tizen.TV.NUI.GLApplication;
using GLKey = Tizen.TV.NUI.GLApplication.Key;

namespace StremioOrigin
{
    static class SbLib
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int IniciarDel(string arte, string dados, int w, int h);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int QuadroDel();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr ErroDel();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void TeclaDel(string nome, int apertou);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int TerminouDel();

        public static IniciarDel Iniciar;
        public static QuadroDel Quadro;
        public static ErroDel Erro;
        public static TeclaDel Tecla;
        public static TerminouDel Terminou;
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void VidaDel(int frente);
        public static VidaDel Vida; // (optional: the app sent to the back or brought in front)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int TextoPedidoDel();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void TextoDel(IntPtr utf8);
        public static TextoPedidoDel TextoPedido; public static TextoDel Texto; // (optional: the TV's own keyboard, see TvKeyboard)
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void WindowDel(IntPtr native);
        public static WindowDel Window; // (optional: the host's window, for the player's picture)

        public static bool Pronto => Iniciar != null && Quadro != null && Erro != null && Tecla != null;
    }

    class Program : TVGLApplication
    {
        [DllImport("libGLESv2.so")] static extern void glClearColor(float r, float g, float b, float a);
        [DllImport("libGLESv2.so")] static extern void glClear(uint mask);
        const uint GL_COLOR_BUFFER_BIT = 0x4000;
        [DllImport("libdl.so.2")] static extern IntPtr dlopen(string path, int flags);
        [DllImport("libdl.so.2")] static extern IntPtr dlsym(IntPtr h, string sym);
        [DllImport("libdl.so.2")] static extern IntPtr dlerror();
        [DllImport("libecore_wayland.so.1")] static extern IntPtr ecore_wl_window_find(uint id);
        [DllImport("libecore_wayland.so.1")] static extern byte ecore_wl_window_keygrab_set(IntPtr win, string key, int mod, int notMod, int priority, int modo);
        [DllImport("libecore_wl2.so.1")] static extern IntPtr ecore_wl2_window_find(uint id);
        [DllImport("libecore_wl2.so.1")] static extern byte ecore_wl2_window_keygrab_set(IntPtr win, string key, int mod, int notMod, int priority, int modo);

        [DllImport("libc.so.6", SetLastError = true, EntryPoint = "syscall")]
        static extern int syscall_memfd(int number, IntPtr name, int flags);
        [DllImport("libc.so.6", SetLastError = true, EntryPoint = "syscall")]
        static extern int syscall_cache(int number, IntPtr start, IntPtr end, int flags);
        [DllImport("libc.so.6", SetLastError = true)] static extern IntPtr write(int fd, IntPtr buf, IntPtr count);
        [DllImport("libc.so.6", SetLastError = true)] static extern IntPtr mmap(IntPtr addr, IntPtr len, int prot, int flags, int fd, IntPtr off);
        [DllImport("libc.so.6", SetLastError = true)] static extern int mprotect(IntPtr addr, IntPtr len, int prot);
        [DllImport("libc.so.6", SetLastError = true)] static extern int munmap(IntPtr addr, IntPtr len);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void InitFn();

        const int RTLD_NOW = 2, RTLD_GLOBAL = 0x100;
        static readonly IntPtr RTLD_DEFAULT = IntPtr.Zero;
        const int PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4;
        const int MAP_PRIVATE = 2, MAP_ANONYMOUS = 0x20;
        const int SYS_memfd_create = 385; // ARMv7 EABI
        const int ARM_NR_cacheflush = 0x0f0002;
        const int PAGE = 4096;
        const int W = 1920, H = 1080;

        // ELF32 constants
        const uint PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7;
        const int STB_WEAK = 2;
        const int R_ARM_NONE = 0, R_ARM_ABS32 = 2, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23;

        static int memfdFd = -1;
        static IntPtr elfMap = IntPtr.Zero;
        static long elfSpan = 0;
        static string logArq = "";

        bool emErro;   // a failed start: its screen is up (FailText), Back leaves
        bool emAviso; Action arranque;   // the start before showed no picture: its code is up (FailText), a key starts this one
        uint janelaId = uint.MaxValue;   // this app's window, by the number the system knows it under (the TV's keyboard is shown for it)
        bool tecladoLigado;
        bool rodando, fim, primeiroQuadro, teclasReservadas, clearSaid, janelaDada;

#if SB_NETLOG
        static void NetLogStart() => DevNet.NetLogStart(); // (development: the build machine's private tools)
        static void NetLog(string line) => DevNet.NetLog(line);
#endif
        static void Log(string msg)
        {
#if SB_NETLOG
            NetLog(DateTime.UtcNow.ToString("HH:mm:ss.fff") + " [tpk-host] " + msg);
#endif
            Console.WriteLine("[tpk-host] " + msg);
            if (!string.IsNullOrEmpty(logArq))
            {
                try { File.AppendAllText(logArq, DateTime.UtcNow.ToString("s") + " " + msg + "\n"); } catch { }
            }
        }

        protected override void OnCreate()
        {
            base.OnCreate();
#if SB_NETLOG
            NetLogStart();
#endif
            var dir = Tizen.Applications.Application.Current.DirectoryInfo;
            string dados = dir.Data;
            logArq = Path.Combine(dados, "tpk-launch.log");
            StartTrace.Open(dados);
            Information.TryGetValue<string>("http://tizen.org/feature/platform.version", out string tv0); Information.TryGetValue<string>("http://tizen.org/system/model_name", out string md0);
            StartTrace.Mark("start", "A " + (md0 ?? "?") + " tizen " + (tv0 ?? "?"));
            Log("OnCreate starting on " + RuntimeInformation.FrameworkDescription);

            // the start before ran 10 s and showed no picture (a black screen): its code first, then this start on a key
            List<string> anterior = null;
            try { anterior = StremioOrigin.Start.StartCode.BlankBefore(StartTrace.All()); } catch { }
            if (anterior != null)
            {
                Log("the start before showed no picture: its code first");
                StartTrace.Mark("note", "the start before showed no picture: its code shown");
                try
                {
                    var su = StremioOrigin.Start.StartCode.Summarise(anterior);
                    string codigo = StremioOrigin.Start.StartCode.Format(su.Host, su.Stage, StremioOrigin.Start.StartCode.NoPicture);
                    var linhas = StremioOrigin.Start.FailLayout.Lines(codigo, "The last start showed no picture", $"TV {md0 ?? "?"} / Tizen {tv0 ?? "?"} / {RuntimeInformation.FrameworkDescription}", null, anterior, "Press OK to start");
                    FailText.Show(linhas, W, H, StremioOrigin.Start.FailLayout.Report(codigo, linhas));
                    emAviso = true; arranque = () => Arranca(dir, dados);
                    return;
                }
                catch (Exception e) { Log("the start before: " + e.Message); }
            }
            Arranca(dir, dados);
        }

        void Arranca(Tizen.Applications.DirectoryInfo dir, string dados)
        {

            string raiz = Path.GetFullPath(Path.Combine(dir.Resource, ".."));
            string bundled = Path.Combine(raiz, "lib", "libstremioorigin.so");
            if (!File.Exists(bundled)) bundled = Path.Combine(dir.Resource, "libstremioorigin.so");

            Log("Native library target: " + bundled + " (exists=" + File.Exists(bundled) + ")");

            string falhaMemfd, falhaElf;
            if (!CarregaNativo(bundled, out falhaMemfd, out falhaElf))
            {
                Log("Native load failed. Memfd: " + falhaMemfd + ", ELF: " + falhaElf);
                Erro(FalhaCarga, "Stremio Origin could not load native core.",
                     "memfd (syscall 385): " + falhaMemfd,
                     "ELF loader: " + falhaElf,
                     "mounts: " + Montagem(raiz));
                return;
            }

            try
            {
                string art = Path.Combine(dir.Resource, "art");
                Log("Initializing Stremio Origin: art=" + art + ", data=" + dados);
                if (SbLib.Iniciar(art, dados, W, H) != 0)
                {
                    string err = Marshal.PtrToStringAnsi(SbLib.Erro()) ?? "sb_host_init returned non-zero";
                    Log("Initialization failed: " + err);
                    Erro(FalhaInicio, "Stremio Origin initialization failed.", err);
                    return;
                }
                Log("Initialization successful.");
            }
            catch (Exception ex)
            {
                Log("Exception during Iniciar: " + ex.GetType().Name + ": " + ex.Message);
                Erro(FalhaOutra, "Exception during native startup", ex.GetType().Name + ": " + ex.Message);
                return;
            }

            rodando = true;
            // a start with no picture 10 s on is noted, for the next start to show its code (a frame later clears it)
            vigia = new System.Threading.Timer(_ =>
            {
                try
                {
                    bool frame = false; foreach (string l in StartTrace.ThisStart()) if (l.StartsWith("frame")) frame = true;
                    if (!frame && !emErro) { StartTrace.Mark("blank", "no picture 10 s after the start"); Log("no picture 10 s after the start: noted"); }
                }
                catch { }
            }, null, 10000, System.Threading.Timeout.Infinite);
        }
        System.Threading.Timer vigia;

        protected override bool OnUpdate()
        {
            if (fim) return false;
            if (emAviso) { try { FailText.Draw(H); } catch (Exception e) { if (!clearSaid) { clearSaid = true; Log("notice: " + e.Message); } } return true; }
            if (!rodando && emErro)
            {
                // a failed start: its code and what failed, drawn here in this window (a separate toolkit window for the
                // error stayed invisible on a Tizen 5.5 TV, under this one or not shown at all: a black screen and no code)
                try { FailText.Draw(H); } catch (Exception e) { if (!clearSaid) { clearSaid = true; Log("fail screen: " + e.Message); } }
                return true;
            }
            if (!rodando) { try { glClearColor(0f, 0f, 0f, 1f); glClear(GL_COLOR_BUFFER_BIT); } catch (Exception e) { if (!clearSaid) { clearSaid = true; Log("clear: " + e.Message); } } return true; } // (nothing drawn yet: black, never an undrawn buffer)
            if (!teclasReservadas) { teclasReservadas = true; ReservaTeclasMidia(); }
            int r = SbLib.Quadro();
            if (r < 0) { fim = true; Log("app ended: closing"); Exit(); return false; } // (the app quit: the process ends with it, it was left spinning on the core and holding the package)
            if (r > 0 && !primeiroQuadro) { primeiroQuadro = true; Log("First frame rendered successfully."); }
            // the TV's own keyboard while the app asks for typed text (Search's keyboard button)
            if (SbLib.TextoPedido != null && janelaId != uint.MaxValue)
            {
                if (!tecladoLigado) { tecladoLigado = true; TvKeyboard.Log = Log; TvKeyboard.Typed = p => { try { SbLib.Texto(p); } catch { } }; TvKeyboard.Key = k => { try { SbLib.Tecla(k, 1); SbLib.Tecla(k, 0); } catch { } }; }
                try { TvKeyboard.Tick(SbLib.TextoPedido() != 0, janelaId); } catch (Exception e) { if (!clearSaid) { clearSaid = true; Log("TV keyboard: " + e.Message); } }
            }
            return true;
        }

        // the TV's Home or another app over this one: the app is told, as on webOS (playback pauses and keeps its place)
        protected override void OnPause() { base.OnPause(); Log("to the back"); try { if (rodando) SbLib.Vida?.Invoke(0); } catch { } }
        protected override void OnResume() { base.OnResume(); Log("back in front"); try { if (rodando) SbLib.Vida?.Invoke(1); } catch { } }

        protected override void OnKeyEvent(GLKey k)
        {
            if (emAviso)
            {
                if (k.State != GLKey.StateType.Up) return;
                if (k.KeyPressedName == "XF86Back" || k.KeyPressedName == "Escape") { Exit(); return; }
                emAviso = false; var a = arranque; arranque = null; Log("the notice left: this start goes on");
                try { a?.Invoke(); } catch (Exception e) { Erro(FalhaOutra, "Exception during native startup", e.GetType().Name + ": " + e.Message); }
                return;
            }
            if (emErro)
            {
                if (k.State == GLKey.StateType.Down && (k.KeyPressedName == "XF86Back" || k.KeyPressedName == "Escape")) Exit();
                return;
            }
            if (rodando) SbLib.Tecla(k.KeyPressedName, k.State == GLKey.StateType.Down ? 1 : 0);
        }

        static readonly string[] TeclasMidia = {
            "XF86AudioPlay", "XF86AudioPause", "XF86AudioPlayPause", "XF86PlayBack",
            "XF86AudioStop", "XF86AudioRewind", "XF86AudioForward",
            "XF86AudioNext", "XF86AudioPrev", "XF86NextChapter", "XF86PreviousChapter",
            "XF86RaiseChannel", "XF86LowerChannel", "XF86ChannelGuide", "XF86ChannelList",
            "XF86Red", "XF86Green", "XF86Yellow", "XF86Blue" // (the colour keys: the card menu and the like, as on webOS)
        };

        void ReservaTeclasMidia()
        {
            foreach (bool wl2 in new[] { false, true })
            {
                const int TOPMOST = 2;
                try
                {
                    var janelas = new List<IntPtr>();
                    for (uint id = 0; id < 64; id++)
                    {
                        IntPtr w = wl2 ? ecore_wl2_window_find(id) : ecore_wl_window_find(id);
                        if (w != IntPtr.Zero && !janelas.Contains(w)) { janelas.Add(w); if (janelaId == uint.MaxValue) janelaId = id; }
                    }
                    if (janelas.Count > 0 && !janelaDada) { janelaDada = true; try { SbLib.Window?.Invoke(janelas[0]); } catch { } } // (the player's picture goes under this window)
                    foreach (string k in TeclasMidia)
                    {
                        foreach (IntPtr w in janelas)
                        {
                            try
                            {
                                if (wl2) ecore_wl2_window_keygrab_set(w, k, 0, 0, 0, TOPMOST);
                                else ecore_wl_window_keygrab_set(w, k, 0, 0, 0, TOPMOST);
                            }
                            catch { }
                        }
                    }
                }
                catch { }
            }
        }

        static bool CarregaNativo(string so, out string falhaMemfd, out string falhaElf)
        {
            falhaMemfd = falhaElf = null;
            byte[] elf;
            try { elf = File.ReadAllBytes(so); }
            catch (Exception e)
            {
                falhaMemfd = falhaElf = "Could not read " + so + ": " + e.Message;
                return false;
            }

            // the core is a 32-bit ARM library: a TV whose apps run otherwise is named in the error instead of a loader's failure
            var arq = RuntimeInformation.ProcessArchitecture;
            if (arq != Architecture.Arm)
            {
                falhaMemfd = falhaElf = "this TV runs apps as " + arq + "; the core is built for 32-bit ARM";
                return false;
            }

            // Route 0: the system's loader, straight from the package (a TV that allows it needs nothing more)
            try
            {
                dlerror();
                IntPtr h0 = dlopen(so, RTLD_NOW | RTLD_GLOBAL);
                if (h0 != IntPtr.Zero && LigaSimbolos(s => dlsym(h0, s), out string q0))
                {
                    Log("Loaded via Route 0 (dlopen)."); StartTrace.Mark("library", "system");
                    return true;
                }
                Log("Route 0 (dlopen) not taken: " + (h0 == IntPtr.Zero ? Err() : "symbols missing"));
            }
            catch (Exception e) { Log("Route 0 (dlopen) exception: " + e.Message); }

            // Route 1: memfd + dlopen(/proc/self/fd/N)
            try
            {
                if (Metodo1_Memfd(elf, out falhaMemfd))
                {
                    Log("Loaded via Route 1 (memfd)."); StartTrace.Mark("library", "memfd");
                    return true;
                }
            }
            catch (Exception e) { falhaMemfd = "Exception " + e.GetType().Name + ": " + e.Message; }

            // Route 2: In-memory ELF loader
            try
            {
                if (Metodo2_CarregadorElf(elf, out falhaElf))
                {
                    Log("Loaded via Route 2 (ELF loader)."); StartTrace.Mark("library", "own");
                    return true;
                }
            }
            catch (Exception e) { falhaElf = "Exception " + e.GetType().Name + ": " + e.Message; }

            return false;
        }

        static bool Metodo1_Memfd(byte[] elf, out string falha)
        {
            falha = null;
            IntPtr nome = Marshal.StringToHGlobalAnsi("stremioorigin");
            int fd;
            try { fd = syscall_memfd(SYS_memfd_create, nome, 0); }
            finally { Marshal.FreeHGlobal(nome); }
            if (fd < 0) { falha = "syscall(385) failed (errno=" + Marshal.GetLastWin32Error() + ")"; return false; }

            GCHandle pin = GCHandle.Alloc(elf, GCHandleType.Pinned);
            try
            {
                IntPtr baseP = pin.AddrOfPinnedObject();
                int off = 0;
                while (off < elf.Length)
                {
                    int n = (int)write(fd, (IntPtr)(baseP.ToInt64() + off), (IntPtr)(elf.Length - off));
                    if (n <= 0) { falha = "write failed at " + off + "/" + elf.Length + " (errno=" + Marshal.GetLastWin32Error() + ")"; return false; }
                    off += n;
                }
            }
            finally { pin.Free(); }

            string path = "/proc/self/fd/" + fd;
            dlerror();
            IntPtr h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
            if (h == IntPtr.Zero) { falha = "dlopen(" + path + ") -> " + Err(); return false; }
            memfdFd = fd;

            if (!LigaSimbolos(s => dlsym(h, s), out string qual)) { falha = "Symbol missing: " + qual; return false; }
            return true;
        }

        static bool Metodo2_CarregadorElf(byte[] e, out string falha)
        {
            falha = null;
            if (e.Length < 52 || e[0] != 0x7F || e[1] != (byte)'E' || e[2] != (byte)'L' || e[3] != (byte)'F') { falha = "not an ELF file"; return false; }
            if (e[4] != 1) { falha = "not ELFCLASS32"; return false; }
            if (e[5] != 1) { falha = "not little-endian"; return false; }
            ushort machine = U16(e, 18);
            if (machine != 40) { falha = "e_machine=" + machine + ", expected 40 (ARM)"; return false; }

            uint phoff = U32(e, 28);
            ushort phentsize = U16(e, 42);
            ushort phnum = U16(e, 44);

            long minVa = long.MaxValue, maxVa = long.MinValue;
            uint dynVa = 0, dynOff = 0, dynSz = 0;
            for (int i = 0; i < phnum; i++)
            {
                int p = (int)(phoff + (uint)i * phentsize);
                uint ptype = U32(e, p);
                uint poff = U32(e, p + 4);
                uint pvaddr = U32(e, p + 8);
                uint pmemsz = U32(e, p + 20);
                if (ptype == PT_TLS)
                {
                    falha = "library has PT_TLS segment (" + pmemsz + " bytes); loader cannot set up compiler TLS";
                    return false;
                }
                if (ptype == PT_LOAD) { if (pvaddr < minVa) minVa = pvaddr; if (pvaddr + pmemsz > maxVa) maxVa = pvaddr + pmemsz; }
                else if (ptype == PT_DYNAMIC) { dynVa = pvaddr; dynOff = poff; dynSz = pfilesz(e, p); }
            }
            if (minVa == long.MaxValue) { falha = "no PT_LOAD segment"; return false; }
            if (dynVa == 0 || dynSz == 0) { falha = "no PT_DYNAMIC segment"; return false; }

            uint fSymtab = 0, fStrtab = 0, fHash = 0, fRel = 0, fRelsz = 0, fRelent = 8, fJmprel = 0, fPltrelsz = 0;
            var needed = new List<uint>();
            for (uint d = dynOff; d + 8 <= dynOff + dynSz; d += 8)
            {
                int tag = (int)U32(e, (int)d);
                uint val = U32(e, (int)d + 4);
                if (tag == 0) break;
                switch (tag)
                {
                    case 1: needed.Add(val); break;
                    case 4: fHash = val; break;
                    case 5: fStrtab = val; break;
                    case 6: fSymtab = val; break;
                    case 17: fRel = val; break;
                    case 18: fRelsz = val; break;
                    case 19: fRelent = val; break;
                    case 23: fJmprel = val; break;
                    case 2: fPltrelsz = val; break;
                }
            }
            if (fSymtab == 0 || fStrtab == 0) { falha = "no DT_SYMTAB/DT_STRTAB"; return false; }
            if (fHash == 0) { falha = "no DT_HASH; library must be linked with --hash-style=both"; return false; }

            if (!VaEmArquivo(e, phoff, phentsize, phnum, fSymtab, out int oSymtab) ||
                !VaEmArquivo(e, phoff, phentsize, phnum, fStrtab, out int oStrtab) ||
                !VaEmArquivo(e, phoff, phentsize, phnum, fHash, out int oHash))
            { falha = "dynamic tables outside PT_LOAD"; return false; }

            int nchain = (int)U32(e, oHash + 4);
            if (nchain <= 0 || nchain > 1000000) { falha = "invalid DT_HASH nchain: " + nchain; return false; }

            var tipos = new SortedDictionary<int, int>();
            if (!ContaRelocs(e, phoff, phentsize, phnum, fRel, fRelsz, fRelent < 8 ? 8 : fRelent, tipos, out falha)) return false;
            if (!ContaRelocs(e, phoff, phentsize, phnum, fJmprel, fPltrelsz, 8, tipos, out falha)) return false;

            foreach (var kv in tipos)
            {
                int t = kv.Key;
                if (t == R_ARM_NONE || t == R_ARM_ABS32 || t == R_ARM_GLOB_DAT || t == R_ARM_JUMP_SLOT || t == R_ARM_RELATIVE) continue;
                falha = "unsupported relocation type " + t;
                return false;
            }

            var pre = new List<string> { "libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
                                         "libEGL.so.1", "libGLESv2.so.2", "libEGL.so", "libGLESv2.so",
                                         "libSDL2-2.0.so.0", "libSDL2.so", "libz.so.1", "libz.so" };
            foreach (var idx in needed) pre.Add(LeCStr(e, oStrtab + (int)idx));
            foreach (var lib in pre) if (!string.IsNullOrEmpty(lib)) dlopen(lib, RTLD_NOW | RTLD_GLOBAL);

            for (int i = 1; i < nchain; i++)
            {
                int sym = oSymtab + i * 16;
                if (sym + 16 > e.Length) break;
                uint stName = U32(e, sym);
                byte stInfo = e[sym + 12];
                ushort stShndx = U16(e, sym + 14);
                if (stShndx != 0) continue;
                if ((stInfo >> 4) == STB_WEAK) continue;
                string nome = LeCStr(e, oStrtab + (int)stName);
                if (string.IsNullOrEmpty(nome)) continue;
                if (dlsym(RTLD_DEFAULT, nome) == IntPtr.Zero)
                {
                    falha = "undefined symbol: " + nome;
                    return false;
                }
            }

            long baseVa = minVa & ~(long)(PAGE - 1);
            long span = ((maxVa - baseVa) + PAGE - 1) & ~(long)(PAGE - 1);

            IntPtr map = mmap(IntPtr.Zero, (IntPtr)span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, IntPtr.Zero);
            if (map == (IntPtr)(-1) || map == IntPtr.Zero) { falha = "mmap failed (errno=" + Marshal.GetLastWin32Error() + ")"; return false; }
            long delta = map.ToInt64() - baseVa;

            for (int i = 0; i < phnum; i++)
            {
                int p = (int)(phoff + (uint)i * phentsize);
                if (U32(e, p) != PT_LOAD) continue;
                uint poff = U32(e, p + 4);
                uint pvaddr = U32(e, p + 8);
                uint filesz = U32(e, p + 16);
                Marshal.Copy(e, (int)poff, (IntPtr)(delta + pvaddr), (int)filesz);
            }

            long dynAddr = delta + dynVa;
            long symtab = 0, strtab = 0, hash = 0;
            long rel = 0, relsz = 0, relent = 8;
            long jmprel = 0, pltrelsz = 0;
            long initArray = 0, initArraySz = 0, initFn = 0;
            for (long d = dynAddr; ; d += 8)
            {
                int tag = Marshal.ReadInt32((IntPtr)d);
                uint val = (uint)Marshal.ReadInt32((IntPtr)(d + 4));
                if (tag == 0) break;
                switch (tag)
                {
                    case 4: hash = delta + val; break;
                    case 5: strtab = delta + val; break;
                    case 6: symtab = delta + val; break;
                    case 17: rel = delta + val; break;
                    case 18: relsz = val; break;
                    case 19: relent = val; break;
                    case 23: jmprel = delta + val; break;
                    case 2: pltrelsz = val; break;
                    case 12: initFn = delta + val; break;
                    case 25: initArray = delta + val; break;
                    case 27: initArraySz = val; break;
                }
            }
            if (symtab == 0 || strtab == 0 || hash == 0) { munmap(map, (IntPtr)span); falha = "dynamic table changed"; return false; }

            if (!RelocaBloco(rel, relsz, relent, symtab, strtab, delta, out falha) ||
                !RelocaBloco(jmprel, pltrelsz, 8, symtab, strtab, delta, out falha))
            { munmap(map, (IntPtr)span); return false; }

            for (int i = 0; i < phnum; i++)
            {
                int p = (int)(phoff + (uint)i * phentsize);
                if (U32(e, p) != PT_LOAD) continue;
                uint pvaddr = U32(e, p + 8);
                uint pmemsz = U32(e, p + 20);
                uint pflags = U32(e, p + 24);
                long segStart = (delta + pvaddr) & ~(long)(PAGE - 1);
                long segEnd = ((delta + pvaddr + pmemsz) + PAGE - 1) & ~(long)(PAGE - 1);
                int prot = ((pflags & 4) != 0 ? PROT_READ : 0) | ((pflags & 2) != 0 ? PROT_WRITE : 0) | ((pflags & 1) != 0 ? PROT_EXEC : 0);
                int rc = mprotect((IntPtr)segStart, (IntPtr)(segEnd - segStart), prot);
                if (rc != 0 && (pflags & 1) != 0)
                { munmap(map, (IntPtr)span); falha = "mprotect +EXEC denied (errno=" + Marshal.GetLastWin32Error() + ")"; return false; }
            }
            try { syscall_cache(ARM_NR_cacheflush, map, (IntPtr)(map.ToInt64() + span), 0); } catch { }

            uint mapIni = unchecked((uint)map.ToInt32());
            uint mapFim = unchecked(mapIni + (uint)span);
            Func<uint, bool> noMapa = x => x >= mapIni && x < mapFim;
            Action<uint, string> roda = (fp, nome) =>
            {
                if (fp == 0 || fp == 0xFFFFFFFFu) return;
                if (!noMapa(fp)) return;
                try { Marshal.GetDelegateForFunctionPointer<InitFn>(new IntPtr(unchecked((int)fp)))(); }
                catch (Exception ex) { Log("dt-init " + nome + " threw " + ex.Message); }
            };
            if (initFn != 0) roda(unchecked((uint)initFn), "DT_INIT");
            for (long a = 0; a < initArraySz; a += 4)
                roda(unchecked((uint)Marshal.ReadInt32(new IntPtr(unchecked((int)(uint)(initArray + a))))), "init_array");

            elfMap = map; elfSpan = span;
            if (!LigaSimbolos(s => (IntPtr)ResolveLocal(s, symtab, strtab, hash, delta), out string qual))
            {
                falha = "symbol missing from dynsym: " + qual;
                return false;
            }
            return true;
        }

        static uint pfilesz(byte[] e, int p) => U32(e, p + 16);

        static bool VaEmArquivo(byte[] e, uint phoff, ushort phentsize, ushort phnum, uint va, out int off)
        {
            off = 0;
            for (int i = 0; i < phnum; i++)
            {
                int p = (int)(phoff + (uint)i * phentsize);
                if (U32(e, p) != PT_LOAD) continue;
                uint poff = U32(e, p + 4), pvaddr = U32(e, p + 8), pfilesz = U32(e, p + 16);
                if (va >= pvaddr && va < pvaddr + pfilesz) { off = (int)(poff + (va - pvaddr)); return off >= 0 && off < e.Length; }
            }
            return false;
        }

        static bool ContaRelocs(byte[] e, uint phoff, ushort phentsize, ushort phnum, uint tabVa, uint tamBytes, uint ent,
                                SortedDictionary<int, int> tipos, out string falha)
        {
            falha = null;
            if (tabVa == 0 || tamBytes == 0) return true;
            if (!VaEmArquivo(e, phoff, phentsize, phnum, tabVa, out int off)) { falha = "relocation table outside PT_LOAD"; return false; }
            if (off + (long)tamBytes > e.Length) { falha = "relocation table past end of file"; return false; }
            for (uint o = 0; o + 8 <= tamBytes; o += ent)
            {
                uint rInfo = U32(e, off + (int)o + 4);
                int type = (int)(rInfo & 0xff);
                tipos[type] = tipos.ContainsKey(type) ? tipos[type] + 1 : 1;
            }
            return true;
        }

        static bool LigaSimbolos(Func<string, IntPtr> resolve, out string qualFaltou)
        {
            qualFaltou = null;
            IntPtr pIni = resolve("sb_host_init");
            IntPtr pQd = resolve("sb_host_frame");
            IntPtr pErr = resolve("sb_host_error");
            IntPtr pTec = resolve("sb_host_key");
            IntPtr pTer = resolve("sb_host_finished");
            IntPtr pVida = resolve("sb_host_life");
            if (pVida != IntPtr.Zero) SbLib.Vida = Marshal.GetDelegateForFunctionPointer<SbLib.VidaDel>(pVida);
            IntPtr pTp = resolve("sb_host_text_wanted"), pTx = resolve("sb_host_text");
            if (pTp != IntPtr.Zero && pTx != IntPtr.Zero) { SbLib.TextoPedido = Marshal.GetDelegateForFunctionPointer<SbLib.TextoPedidoDel>(pTp); SbLib.Texto = Marshal.GetDelegateForFunctionPointer<SbLib.TextoDel>(pTx); }
            IntPtr pWin = resolve("sb_host_window");
            if (pWin != IntPtr.Zero) SbLib.Window = Marshal.GetDelegateForFunctionPointer<SbLib.WindowDel>(pWin);
            if (pIni == IntPtr.Zero) { qualFaltou = "sb_host_init"; return false; }
            if (pQd == IntPtr.Zero) { qualFaltou = "sb_host_frame"; return false; }
            if (pErr == IntPtr.Zero) { qualFaltou = "sb_host_error"; return false; }
            if (pTec == IntPtr.Zero) { qualFaltou = "sb_host_key"; return false; }
            SbLib.Iniciar = Marshal.GetDelegateForFunctionPointer<SbLib.IniciarDel>(pIni);
            SbLib.Quadro = Marshal.GetDelegateForFunctionPointer<SbLib.QuadroDel>(pQd);
            SbLib.Erro = Marshal.GetDelegateForFunctionPointer<SbLib.ErroDel>(pErr);
            SbLib.Tecla = Marshal.GetDelegateForFunctionPointer<SbLib.TeclaDel>(pTec);
            if (pTer != IntPtr.Zero) SbLib.Terminou = Marshal.GetDelegateForFunctionPointer<SbLib.TerminouDel>(pTer);
            return SbLib.Pronto;
        }

        static bool RelocaBloco(long tabela, long tamBytes, long ent, long symtab, long strtab, long delta, out string falha)
        {
            falha = null;
            if (tabela == 0 || tamBytes == 0) return true;
            if (ent < 8) ent = 8;
            for (long o = 0; o < tamBytes; o += ent)
            {
                long r = tabela + o;
                uint rOffset = (uint)Marshal.ReadInt32((IntPtr)r);
                uint rInfo = (uint)Marshal.ReadInt32((IntPtr)(r + 4));
                int type = (int)(rInfo & 0xff);
                int symidx = (int)(rInfo >> 8);
                IntPtr where = (IntPtr)(delta + rOffset);
                switch (type)
                {
                    case R_ARM_NONE:
                        break;
                    case R_ARM_RELATIVE:
                        Marshal.WriteInt32(where, (int)((uint)Marshal.ReadInt32(where) + (uint)delta));
                        break;
                    case R_ARM_ABS32:
                    case R_ARM_GLOB_DAT:
                    {
                        if (!ResolveSimbolo(symidx, symtab, strtab, delta, out long s, out falha)) return false;
                        uint a = (uint)Marshal.ReadInt32(where);
                        Marshal.WriteInt32(where, (int)((uint)s + a));
                        break;
                    }
                    case R_ARM_JUMP_SLOT:
                    {
                        if (!ResolveSimbolo(symidx, symtab, strtab, delta, out long s, out falha)) return false;
                        Marshal.WriteInt32(where, (int)(uint)s);
                        break;
                    }
                    default:
                        falha = "unsupported relocation type " + type + " at offset 0x" + rOffset.ToString("x");
                        return false;
                }
            }
            return true;
        }

        static bool ResolveSimbolo(int symidx, long symtab, long strtab, long delta, out long addr, out string falha)
        {
            falha = null;
            long sym = symtab + (long)symidx * 16;
            uint stName = (uint)Marshal.ReadInt32((IntPtr)sym);
            uint stValue = (uint)Marshal.ReadInt32((IntPtr)(sym + 4));
            byte stInfo = Marshal.ReadByte((IntPtr)(sym + 12));
            ushort stShndx = (ushort)Marshal.ReadInt16((IntPtr)(sym + 14));
            if (stShndx != 0) { addr = delta + stValue; return true; }
            string nome = LeCStr(strtab + stName);
            if (string.IsNullOrEmpty(nome)) { addr = 0; return true; }
            IntPtr p = dlsym(RTLD_DEFAULT, nome);
            addr = p.ToInt64();
            if (addr == 0 && (stInfo >> 4) != STB_WEAK) { falha = "undefined symbol: " + nome; return false; }
            return true;
        }

        static long ResolveLocal(string nome, long symtab, long strtab, long hash, long delta)
        {
            int count = Marshal.ReadInt32((IntPtr)(hash + 4));
            if (count <= 0 || count > 1000000) return 0;
            for (int i = 1; i < count; i++)
            {
                long sym = symtab + (long)i * 16;
                uint stName = (uint)Marshal.ReadInt32((IntPtr)sym);
                uint stValue = (uint)Marshal.ReadInt32((IntPtr)(sym + 4));
                ushort stShndx = (ushort)Marshal.ReadInt16((IntPtr)(sym + 14));
                if (stShndx == 0 || stValue == 0) continue;
                if (LeCStr(strtab + stName) == nome) return delta + stValue;
            }
            return 0;
        }

        static string LeCStr(long addr)
        {
            var sb = new System.Text.StringBuilder();
            for (int i = 0; i < 256; i++)
            {
                byte b = Marshal.ReadByte((IntPtr)(addr + i));
                if (b == 0) break;
                sb.Append((char)b);
            }
            return sb.ToString();
        }

        static string LeCStr(byte[] e, int off)
        {
            var sb = new System.Text.StringBuilder();
            for (int i = 0; i < 256 && off + i < e.Length; i++)
            {
                byte b = e[off + i];
                if (b == 0) break;
                sb.Append((char)b);
            }
            return sb.ToString();
        }

        static uint U32(byte[] b, int o) => (uint)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24));
        static ushort U16(byte[] b, int o) => (ushort)(b[o] | (b[o + 1] << 8));

        static string Err()
        {
            IntPtr e = dlerror();
            string s = e == IntPtr.Zero ? null : Marshal.PtrToStringAnsi(e);
            return string.IsNullOrEmpty(s) ? "error without message" : s;
        }

        static string Montagem(string raiz)
        {
            try
            {
                string melhor = "", linha = "?";
                foreach (var l in File.ReadAllLines("/proc/self/mounts"))
                {
                    var p = l.Split(' ');
                    if (p.Length > 3 && raiz.StartsWith(p[1]) && p[1].Length > melhor.Length) { melhor = p[1]; linha = p[1] + " " + p[3]; }
                }
                return linha;
            }
            catch (Exception e) { return "? (" + e.Message + ")"; }
        }

        // this host's fail reasons, as in the codes of the other hosts and the picker (01-09 the picker's, 10-19 the newer hosts')
        const int FalhaCarga = 20, FalhaInicio = 21, FalhaOutra = 29;

        void Erro(int razao, string titulo, params string[] detalhes)
        {
            rodando = false;
            StartTrace.Mark("fail", razao.ToString("00") + " " + titulo);
            string codigo = "A?-" + razao.ToString("00");
            try { var su = StremioOrigin.Start.StartCode.Summarise(StartTrace.ThisStart()); codigo = StremioOrigin.Start.StartCode.Format('A', su.Stage, razao); } catch { }
            Log("Start failed, code " + codigo + ": " + titulo);
            Information.TryGetValue<string>("http://tizen.org/feature/platform.version", out string versao);
            Information.TryGetValue<string>("http://tizen.org/system/model_name", out string modelo);
            // drawn in this host's own window (FailText), laid out as every failed start's screen is (FailLayout): the code
            // first (host, last stage reached, reason), what failed, the TV, the stages; bottom right the code to scan
            var linhas = StremioOrigin.Start.FailLayout.Lines(codigo, titulo, $"TV {modelo ?? "?"} / Tizen {versao ?? "?"} / {RuntimeInformation.FrameworkDescription}", detalhes, StartTrace.ThisStart());
            try { FailText.Show(linhas, W, H, StremioOrigin.Start.FailLayout.Report(codigo, linhas)); } catch (Exception e) { Log("fail screen: " + e.Message); }
            emErro = true;
        }

        static void Main(string[] args)
        {
            new Program().Run(args);
        }
    }
}
