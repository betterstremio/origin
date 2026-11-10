package tv.stremioorigin.app;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.os.Build;
import android.os.Bundle;
import android.system.Os;
import android.util.Log;
import android.view.Display;
import android.view.KeyEvent;
import android.view.InputDevice;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

import org.libsdl.app.SDLActivity;

/* SDL's activity, plus what the C side reads before it starts (environment variables, see
   src/platform/android/plat_android.c): the assets copied out of the APK, the data and library folders, the
   device and what its screen and decoders can do. The screen is a 16:9 box (Box) holding, bottom to top, the
   video (SbPlayer's SurfaceView in a clipping frame), its subtitles, and SDL's translucent surface. */
public class MainActivity extends SDLActivity {
    static final String TAG = "StremioOrigin";
    static MainActivity self;
    static Box box;
    /* every variable set for the C side, also handed over by envList(): native code running under a translation layer
       (an ARM build on an x86 emulator) has its own copy of the environment, which Os.setenv does not reach */
    static final java.util.LinkedHashMap<String, String> env = new java.util.LinkedHashMap<>();
    static void setenv(String k, String v) throws Exception { Os.setenv(k, v, true); env.put(k, v); }
    public static String envList() { StringBuilder b = new StringBuilder(); for (java.util.Map.Entry<String, String> e : env.entrySet()) b.append(e.getKey()).append('=').append(e.getValue().replace('\n', ' ')).append('\n'); return b.toString(); }

    @Override protected String[] getLibraries() { return new String[] { "SDL2", "curl", "main" }; }

    @Override protected void onCreate(Bundle saved) {
        self = this;
        prepareEnvironment(getIntent());
        super.onCreate(saved);
        if (mBrokenLibraries || mLayout == null) return;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        getWindow().getDecorView().setBackgroundColor(Color.BLACK);
        // SDL's surface over the video: translucent, and on the media-overlay layer above the player's SurfaceView
        mSurface.getHolder().setFormat(PixelFormat.TRANSLUCENT);
        mSurface.setZOrderMediaOverlay(true);
        ViewGroup parent = (ViewGroup) mLayout.getParent(); if (parent != null) parent.removeView(mLayout);
        box = new Box(this);
        SbPlayer.attach(this, box);            // the video frame and the subtitle view, under SDL's layout
        box.addView(mLayout, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(box);
    }

    @Override protected void onResume() { super.onResume(); SbUpdater.onResume(this); }
    @Override protected void onDestroy() { SbPlayer.release(); super.onDestroy(); }
    @Override protected void onNewIntent(Intent i) { super.onNewIntent(i); }

    /* the remote's keys SDL has no name for, as keys it does (plat_map_key reads them back); every key as a
       keyboard's, so a remote that calls itself a gamepad still sends keys and not joystick buttons */
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int k = e.getKeyCode(), m = k;
        switch (k) {
            case KeyEvent.KEYCODE_PROG_RED: m = KeyEvent.KEYCODE_F1; break;
            case KeyEvent.KEYCODE_PROG_GREEN: m = KeyEvent.KEYCODE_F2; break;
            case KeyEvent.KEYCODE_PROG_YELLOW: m = KeyEvent.KEYCODE_F3; break;
            case KeyEvent.KEYCODE_PROG_BLUE: m = KeyEvent.KEYCODE_F4; break;
            case KeyEvent.KEYCODE_CHANNEL_UP: m = KeyEvent.KEYCODE_PAGE_UP; break;
            case KeyEvent.KEYCODE_CHANNEL_DOWN: m = KeyEvent.KEYCODE_PAGE_DOWN; break;
            case KeyEvent.KEYCODE_MEDIA_PLAY: m = KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE; break;
            case KeyEvent.KEYCODE_MEDIA_PAUSE: m = KeyEvent.KEYCODE_BREAK; break;
            case KeyEvent.KEYCODE_BUTTON_A: m = KeyEvent.KEYCODE_DPAD_CENTER; break;
            case KeyEvent.KEYCODE_BUTTON_B: m = KeyEvent.KEYCODE_BACK; break;
            default: break;
        }
        int src = e.getSource();
        boolean pad = (src & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD || (src & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
        if (m != k || pad)
            e = new KeyEvent(e.getDownTime(), e.getEventTime(), e.getAction(), m, e.getRepeatCount(), e.getMetaState(), e.getDeviceId(), e.getScanCode(), e.getFlags(), InputDevice.SOURCE_KEYBOARD);
        return super.dispatchKeyEvent(e);
    }

    // ---- before SDL starts ----
    void prepareEnvironment(Intent intent) {
        try {
            File app = new File(getFilesDir(), "app"), data = new File(getFilesDir(), "data");
            data.mkdirs();
            copyAssetsIfNew(app);
            setenv("SB_ASSETS", new File(app, "assets").getAbsolutePath());
            setenv("SB_DATA", data.getAbsolutePath());
            setenv("SB_LIBDIR", getApplicationInfo().nativeLibraryDir);
            setenv("SB_DEVICE", deviceName());
            Locale l = getResources().getConfiguration().getLocales().get(0);
            setenv("SB_LOCALE", l.getLanguage() + (l.getCountry().isEmpty() ? "" : "-" + l.getCountry()));
            setenv("SB_VSYNC", "1");          // Android's buffer queue paces frames to the display
            setenv("TMPDIR", getCacheDir().getAbsolutePath());
            setenv("SB_TVCAPS", Caps.describe(this));
            setenv("SB_TV", isTv() ? "1" : "0");
            // the red and green keys on some remote or keyboard (Sony, Philips, TCL remotes have them; Google TV and Fire TV ones don't):
            // the guide names them only then
            boolean[] ck = android.view.KeyCharacterMap.deviceHasKeys(new int[] { KeyEvent.KEYCODE_PROG_RED, KeyEvent.KEYCODE_PROG_GREEN });
            setenv("SB_COLORKEYS", ck[0] && ck[1] ? "1" : "0");
            if (BuildConfig.DEV_HOOKS && intent != null && intent.getExtras() != null) {   // dev builds: SB_* from the launch (the tests)
                Bundle b = intent.getExtras();
                for (String k : b.keySet()) if (k.startsWith("SB_") || k.equals("LANG")) { Object v = b.get(k); if (v != null) { setenv(k, v.toString()); Log.i(TAG, "dev env " + k + "=" + v); } }
            }
        } catch (Exception ex) { Log.e(TAG, "environment", ex); }
    }
    boolean isTv() { return getPackageManager().hasSystemFeature("android.software.leanback"); }
    static String deviceName() {
        String m = Build.MANUFACTURER + " " + Build.MODEL;
        return (self != null && self.isTv() ? "Android TV " : "Android ") + Build.VERSION.RELEASE + " " + m.trim();
    }
    /* the assets ship inside the APK under app/; the C side reads plain files, so they are copied out once per install */
    void copyAssetsIfNew(File dst) throws Exception {
        PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
        String stamp = pi.versionCode + ":" + pi.lastUpdateTime;
        File st = new File(dst, ".stamp");
        if (st.exists() && stamp.equals(new String(java.nio.file.Files.readAllBytes(st.toPath())))) return;
        long t0 = System.currentTimeMillis();
        deleteTree(dst); dst.mkdirs();
        int n = copyTree(getAssets(), "app", dst);
        try (OutputStream o = new FileOutputStream(st)) { o.write(stamp.getBytes()); }
        Log.i(TAG, "assets: " + n + " files copied in " + (System.currentTimeMillis() - t0) + " ms");
    }
    static int copyTree(AssetManager am, String path, File dst) throws Exception {
        String[] kids = am.list(path); int n = 0;
        if (kids == null || kids.length == 0) {
            dst.getParentFile().mkdirs();
            try (InputStream in = am.open(path); OutputStream out = new FileOutputStream(dst)) {
                byte[] buf = new byte[65536]; int r; while ((r = in.read(buf)) > 0) out.write(buf, 0, r); }
            return 1;
        }
        dst.mkdirs();
        for (String k : kids) n += copyTree(am, path + "/" + k, new File(dst, k));
        return n;
    }
    static void deleteTree(File f) { File[] k = f.listFiles(); if (k != null) for (File c : k) deleteTree(c); f.delete(); }

    /* a 16:9 box in the middle of the screen: every child is laid out in it (a phone's wider screen gets black bars) */
    static class Box extends FrameLayout {
        int bx, by, bw, bh;
        Box(Context c) { super(c); setBackgroundColor(Color.BLACK); setClipChildren(true); }
        @Override protected void onMeasure(int ws, int hs) {
            int w = MeasureSpec.getSize(ws), h = MeasureSpec.getSize(hs);
            bw = w; bh = w * 9 / 16; if (bh > h) { bh = h; bw = h * 16 / 9; }
            bx = (w - bw) / 2; by = (h - bh) / 2;
            setMeasuredDimension(w, h);
            for (int i = 0; i < getChildCount(); i++) getChildAt(i).measure(MeasureSpec.makeMeasureSpec(bw, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(bh, MeasureSpec.EXACTLY));
        }
        @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
            for (int i = 0; i < getChildCount(); i++) getChildAt(i).layout(bx, by, bx + bw, by + bh);
        }
    }

    /* what the screen and the decoders take, as the C side's SB_TVCAPS ("maxRes=2160,hdr=1,dv=0,av1=1,dts=0,truehd=0") */
    static class Caps {
        static String describe(MainActivity a) {
            int maxRes = 1080, hdr = 0, dv = 0, av1 = 0, dts = 0, truehd = 0, ac3 = 0, eac3 = 0;
            try {
                Display d = a.getWindowManager().getDefaultDisplay();
                for (Display.Mode m : d.getSupportedModes()) if (Math.min(m.getPhysicalWidth(), m.getPhysicalHeight()) >= 2160) maxRes = 2160;
                Display.HdrCapabilities hc = d.getHdrCapabilities();
                if (hc != null) for (int t : hc.getSupportedHdrTypes()) {
                    if (t == Display.HdrCapabilities.HDR_TYPE_HDR10) hdr = 1;
                    if (t == Display.HdrCapabilities.HDR_TYPE_DOLBY_VISION) dv = 1; }
                boolean dec4k = false;   // 4K counts only when a decoder takes it too (some boxes put out 4K but decode 1080p only)
                for (MediaCodecInfo ci : new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
                    if (ci.isEncoder()) continue;
                    for (String t : ci.getSupportedTypes()) if (t.equalsIgnoreCase("video/hevc") || t.equalsIgnoreCase("video/avc")) {
                        try { MediaCodecInfo.VideoCapabilities vc = ci.getCapabilitiesForType(t).getVideoCapabilities(); if (vc != null && vc.isSizeSupported(3840, 2160)) dec4k = true; } catch (Exception ignored) {} }
                    boolean hw = Build.VERSION.SDK_INT >= 29 ? ci.isHardwareAccelerated() : !(ci.getName().startsWith("OMX.google") || ci.getName().startsWith("c2.android"));
                    for (String t : ci.getSupportedTypes()) {
                        if (t.equalsIgnoreCase("video/av01") && hw) av1 = 1;
                        if (t.equalsIgnoreCase("audio/vnd.dts") || t.equalsIgnoreCase("audio/vnd.dts.hd")) dts = 1;
                        if (t.equalsIgnoreCase("audio/true-hd")) truehd = 1;
                        if (t.equalsIgnoreCase("audio/ac3")) ac3 = 1;
                        if (t.equalsIgnoreCase("audio/eac3") || t.equalsIgnoreCase("audio/eac3-joc")) eac3 = 1;
                        if (t.equalsIgnoreCase("video/dolby-vision") && dv == 0 && hdr == 1) dv = 1; } }
                if (maxRes == 2160 && !dec4k) { maxRes = 1080; Log.i(TAG, "the screen takes 4K, no decoder does: 1080p"); }
                androidx.media3.exoplayer.audio.AudioCapabilities ac = androidx.media3.exoplayer.audio.AudioCapabilities.getCapabilities(a);
                if (ac.supportsEncoding(androidx.media3.common.C.ENCODING_DTS)) dts = 1;
                if (ac.supportsEncoding(androidx.media3.common.C.ENCODING_DOLBY_TRUEHD)) truehd = 1;
                if (ac.supportsEncoding(androidx.media3.common.C.ENCODING_AC3)) ac3 = 1;           // passed to a receiver or the TV over HDMI
                if (ac.supportsEncoding(androidx.media3.common.C.ENCODING_E_AC3) || ac.supportsEncoding(androidx.media3.common.C.ENCODING_E_AC3_JOC)) eac3 = 1;
            } catch (Throwable t) { Log.w(TAG, "caps", t); ac3 = eac3 = 1; }   // (unknown: as a TV)
            String s = "maxRes=" + maxRes + ",hdr=" + hdr + ",dv=" + dv + ",av1=" + av1 + ",dts=" + dts + ",truehd=" + truehd + ",ac3=" + ac3 + ",eac3=" + eac3;
            Log.i(TAG, "device caps " + s);
            return s;
        }
    }
}
