package tv.stremioorigin.app;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;

/* The in-app update (src/core/update.c on Android): the release's APK is downloaded to the cache, its SHA-256
   checked against the one GitHub lists, and handed to the system's installer through a PackageInstaller
   session. The first time, Android asks the user to allow installs from this app: the install goes on by
   itself when they come back (onResume). The C side polls the state as "<0|1|->|<message>". */
public final class SbUpdater {
    static final String TAG = "StremioOrigin";
    static final String ACTION = "tv.stremioorigin.app.INSTALL_STATUS";
    static volatile String state = "0|";
    static volatile boolean busy;
    static File ready;                 // a checked APK waiting for the permission
    static boolean waitingPermission;
    static BroadcastReceiver receiver;

    public static String poll() { return state; }

    public static boolean start(String url, String sha, String version) {
        if (busy) return true;
        Activity a = MainActivity.self; if (a == null) return false;
        busy = true; state = "0|Downloading…";
        new Thread(() -> {
            try {
                File f = new File(a.getCacheDir(), "update-" + version + ".apk");
                download(url, f);
                String got = sha256(f);
                if (!got.equalsIgnoreCase(sha)) { f.delete(); fail("the download is damaged (checksum " + got.substring(0, 12) + "… instead of " + sha.substring(0, Math.min(12, sha.length())) + "…)"); return; }
                Log.i(TAG, "update: " + f.length() + " bytes, checksum ok");
                a.runOnUiThread(() -> install(a, f));
            } catch (Exception e) { Log.w(TAG, "update download", e); fail(e.getMessage() != null ? e.getMessage() : e.toString()); }
        }, "sb-update").start();
        return true;
    }
    static void fail(String why) { state = "-|" + why; busy = false; }

    static void download(String url, File out) throws Exception {
        for (int hop = 0; hop < 8; hop++) {
            HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
            c.setInstanceFollowRedirects(false); c.setConnectTimeout(15000); c.setReadTimeout(30000);
            c.setRequestProperty("User-Agent", "StremioOrigin (Android; native)");
            int code = c.getResponseCode();
            if (code >= 300 && code < 400) { String loc = c.getHeaderField("Location"); c.disconnect(); if (loc == null) throw new Exception("redirect without a place"); url = new URL(new URL(url), loc).toString(); continue; }
            if (code != 200) { c.disconnect(); throw new Exception("HTTP " + code); }
            long total = c.getContentLengthLong(), got = 0; int lastPct = -1;
            try (InputStream in = c.getInputStream(); OutputStream o = new FileOutputStream(out)) {
                byte[] buf = new byte[65536]; int r;
                while ((r = in.read(buf)) > 0) { o.write(buf, 0, r); got += r;
                    if (total > 0) { int pct = (int) (got * 100 / total); if (pct != lastPct) { lastPct = pct; state = "0|Downloading… " + pct + "%"; } } }
            }
            c.disconnect(); return;
        }
        throw new Exception("too many redirects");
    }
    static String sha256(File f) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        try (InputStream in = new FileInputStream(f)) { byte[] b = new byte[65536]; int r; while ((r = in.read(b)) > 0) md.update(b, 0, r); }
        StringBuilder s = new StringBuilder(); for (byte x : md.digest()) s.append(String.format("%02x", x)); return s.toString();
    }

    static void install(Activity a, File f) {
        if (Build.VERSION.SDK_INT >= 26 && !a.getPackageManager().canRequestPackageInstalls()) {
            ready = f; waitingPermission = true; state = "1|Allow installs from Origin, then come back";
            try { a.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + a.getPackageName()))); }
            catch (Exception e) { fail("this device does not let apps install updates: install the APK by hand"); }
            return;
        }
        try {
            PackageInstaller pi = a.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams p = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            p.setAppPackageName(a.getPackageName()); p.setSize(f.length());
            if (Build.VERSION.SDK_INT >= 31) p.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
            int id = pi.createSession(p);
            try (PackageInstaller.Session s = pi.openSession(id)) {
                try (InputStream in = new FileInputStream(f); OutputStream o = s.openWrite("base.apk", 0, f.length())) {
                    byte[] b = new byte[65536]; int r; while ((r = in.read(b)) > 0) o.write(b, 0, r); s.fsync(o); }
                register(a);
                Intent i = new Intent(ACTION).setPackage(a.getPackageName());
                PendingIntent pend = PendingIntent.getBroadcast(a, id, i, PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0));
                s.commit(pend.getIntentSender());
            }
            state = "1|Installing…";
        } catch (Exception e) { Log.w(TAG, "update install", e); fail(e.getMessage() != null ? e.getMessage() : e.toString()); }
    }
    static void register(Context c) {
        if (receiver != null) return;
        receiver = new BroadcastReceiver() { @Override public void onReceive(Context cx, Intent in) {
            int st = in.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
            String msg = in.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            Log.i(TAG, "update: installer status " + st + " " + msg);
            if (st == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                Intent confirm = in.getParcelableExtra(Intent.EXTRA_INTENT);
                if (confirm != null) { confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK); cx.startActivity(confirm); }
                state = "1|Confirm the update on screen";
            } else if (st == PackageInstaller.STATUS_SUCCESS) { state = "1|Installed: open the app again"; busy = false; }
            else if (st == PackageInstaller.STATUS_FAILURE_ABORTED) fail("cancelled");
            else fail(msg != null ? msg : "the installer said no (" + st + ")");
        } };
        IntentFilter flt = new IntentFilter(ACTION);
        if (Build.VERSION.SDK_INT >= 33) c.registerReceiver(receiver, flt, Context.RECEIVER_NOT_EXPORTED); else c.registerReceiver(receiver, flt);
    }
    static void onResume(Activity a) {
        if (!waitingPermission || ready == null) return;
        if (Build.VERSION.SDK_INT >= 26 && !a.getPackageManager().canRequestPackageInstalls()) { fail("installs from this app are not allowed"); waitingPermission = false; return; }
        waitingPermission = false; File f = ready; ready = null; install(a, f);
    }
}
