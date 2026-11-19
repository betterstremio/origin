package tv.stremioorigin.app;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

/* The first run of the new app id (src/core/migrate.c calls these over JNI): the old app's settings and sign-ins come
   through its ExportProvider (2.1.12; content://tv.stremioorigin.app.export), then the system is asked to remove the old
   package. Each file is written to <name>.tmp and renamed; state.json goes last, so it only exists when the rest landed. */
public final class SbMigrate {
    static final String TAG = "StremioOrigin";
    static final String OLD = "tv.stremioorigin.app";
    static final String BASE = "content://" + OLD + ".export/";

    /* -1: the provider is not there (an old app older than 2.1.12, or none); else how many files landed */
    public static int importOld(String dst) {
        Activity a = MainActivity.self;
        if (a == null) return -1;
        ContentResolver cr = a.getContentResolver();
        List<String> names = new ArrayList<>();
        try (Cursor c = cr.query(Uri.parse(BASE + "files"), null, null, null, null)) {
            if (c == null) return -1;
            while (c.moveToNext()) names.add(c.getString(0));
        } catch (Exception e) { Log.i(TAG, "migrate: no provider (" + e + ")"); return -1; }
        File d = new File(dst); new File(d, "acct").mkdirs();
        int n = 0;
        for (int pass = 0; pass < 2; pass++)   // pass 0: everything but state.json, pass 1: state.json
            for (String name : names) {
                if (name.contains("..") || name.startsWith("/") || name.contains("\\")) continue;
                if (("state.json".equals(name)) != (pass == 1)) continue;
                File out = new File(d, name), tmp = new File(d, name + ".tmp");
                try (InputStream in = cr.openInputStream(Uri.parse(BASE + "file/" + name)); OutputStream o = new FileOutputStream(tmp)) {
                    if (in == null) continue;
                    byte[] b = new byte[65536]; int r;
                    while ((r = in.read(b)) > 0) o.write(b, 0, r);
                    o.flush();
                } catch (Exception e) { Log.w(TAG, "migrate: " + name, e); tmp.delete(); continue; }
                if (out.exists()) out.delete();
                if (tmp.renameTo(out)) n++; else tmp.delete();
            }
        Log.i(TAG, "migrate: " + n + " of " + names.size() + " files imported");
        return n;
    }

    /* the system shows its own one-tap confirmation */
    public static void removeOld() {
        Activity a = MainActivity.self;
        if (a == null) return;
        try {
            a.getPackageManager().getPackageInfo(OLD, 0);
            a.startActivity(new Intent(Intent.ACTION_DELETE, Uri.parse("package:" + OLD)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        } catch (Exception e) { Log.i(TAG, "migrate: old app not removable here (" + e + ")"); }
    }
}
