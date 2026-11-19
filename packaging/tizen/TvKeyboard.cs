// The TV's own on-screen keyboard for host A. The app asks for typed text (Search's keyboard button); the host shows
// the TV's keyboard through the system's input method calls and hands what is typed back to the app. Everything here
// runs on the main thread (the host's frame call), and every step is guarded: a TV without these calls keeps the app's
// own keyboard and nothing else changes.
using System;
using System.Runtime.InteropServices;

namespace StremioOrigin
{
    static class TvKeyboard
    {
        const string Lib = "libecore_imf.so.1";
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void EventCb(IntPtr data, IntPtr ctx, IntPtr info);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void PanelCb(IntPtr data, IntPtr ctx, int value);
        [DllImport(Lib)] static extern int ecore_imf_init();
        [DllImport(Lib)] static extern IntPtr ecore_imf_context_default_id_get();
        [DllImport(Lib)] static extern IntPtr ecore_imf_context_add(IntPtr id);
        [DllImport(Lib)] static extern void ecore_imf_context_client_window_set(IntPtr ctx, IntPtr window);
        [DllImport(Lib)] static extern void ecore_imf_context_event_callback_add(IntPtr ctx, int type, EventCb cb, IntPtr data);
        [DllImport(Lib)] static extern void ecore_imf_context_input_panel_event_callback_add(IntPtr ctx, int type, PanelCb cb, IntPtr data);
        [DllImport(Lib)] static extern void ecore_imf_context_focus_in(IntPtr ctx);
        [DllImport(Lib)] static extern void ecore_imf_context_focus_out(IntPtr ctx);
        [DllImport(Lib)] static extern void ecore_imf_context_reset(IntPtr ctx);
        [DllImport(Lib)] static extern void ecore_imf_context_input_panel_show(IntPtr ctx);
        [DllImport(Lib)] static extern void ecore_imf_context_input_panel_hide(IntPtr ctx);
        const int CommitEvent = 3, DeleteEvent = 4;   // the text typed; letters to take away (struct: ctx, offset, count)
        const int PanelState = 0, PanelShown = 0, PanelHidden = 1;

        static IntPtr ctx; static bool tried, up, wanted;
        static EventCb onCommit, onDelete; static PanelCb onPanel;   // kept: the system holds these for as long as the context lives
        public static Action<string> Log = s => { };
        public static Action<IntPtr> Typed = p => { };               // the typed text, as the system gave it (UTF-8)
        public static Action<string> Key = k => { };                 // a key for the app: one press and release

        // every frame: is typed text asked for? the keyboard comes up when it starts to be, goes when it stops
        public static void Tick(bool want, uint windowId)
        {
            if (want == wanted) return;
            wanted = want;
            try { if (want) Show(windowId); else Hide(); }
            catch (Exception e) { Log("TV keyboard: " + e.GetType().Name + ": " + e.Message); }
        }

        static void Show(uint windowId)
        {
            if (ctx == IntPtr.Zero)
            {
                if (tried) return; tried = true;   // (could not be made once: not asked again this run)
                ecore_imf_init();
                IntPtr id = ecore_imf_context_default_id_get();
                Log("TV keyboard: input method " + (id == IntPtr.Zero ? "(none)" : Marshal.PtrToStringAnsi(id)));
                if (id == IntPtr.Zero) return;
                ctx = ecore_imf_context_add(id);
                if (ctx == IntPtr.Zero) { Log("TV keyboard: no context"); return; }
                ecore_imf_context_client_window_set(ctx, new IntPtr(windowId));
                onCommit = (d, c, info) => { try { if (info != IntPtr.Zero) Typed(info); } catch { } };
                onDelete = (d, c, info) => { try { int n = info == IntPtr.Zero ? 1 : Marshal.ReadInt32(info, IntPtr.Size + 4); for (int i = 0; i < (n < 1 ? 1 : n > 32 ? 32 : n); i++) Key("XF86Back"); } catch { } };
                onPanel = (d, c, v) => { try { Log("TV keyboard: panel " + (v == PanelShown ? "shown" : v == PanelHidden ? "hidden" : "state " + v)); if (v == PanelHidden && up) { up = false; if (wanted) Key("Return"); } else if (v == PanelShown) up = true; } catch { } };
                ecore_imf_context_event_callback_add(ctx, CommitEvent, onCommit, IntPtr.Zero);
                ecore_imf_context_event_callback_add(ctx, DeleteEvent, onDelete, IntPtr.Zero);
                ecore_imf_context_input_panel_event_callback_add(ctx, PanelState, onPanel, IntPtr.Zero);
                Log("TV keyboard: ready (window " + windowId + ")");
            }
            ecore_imf_context_reset(ctx);
            ecore_imf_context_focus_in(ctx);
            ecore_imf_context_input_panel_show(ctx);
            Log("TV keyboard: asked to show");
        }

        static void Hide()
        {
            if (ctx == IntPtr.Zero) return;
            up = false;
            ecore_imf_context_input_panel_hide(ctx);
            ecore_imf_context_focus_out(ctx);
            Log("TV keyboard: put away");
        }
    }
}
