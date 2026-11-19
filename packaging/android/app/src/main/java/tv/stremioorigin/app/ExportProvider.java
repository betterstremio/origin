package tv.stremioorigin.app;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/* The bridge to the app that replaces this one under a new package name (src/core/update.c, 2.1.12): it serves this
   app's settings and sign-ins, read only, so the new app can import them on its first run. Only apps signed with our
   key hold the permission the manifest asks for. The list is rebuilt from the data folder on every call and an open
   must name an entry of it, so a path never reaches the file system unchecked.
     content://tv.stremioorigin.app.export/files        -> cursor (name, size)
     content://tv.stremioorigin.app.export/file/<name>  -> the file, read only
   Listed: the regular files at the top of the data folder (state.json, collections.json, the tmdb/trakt keys) and the
   .json files in acct/ (sign-ins and profiles, named "acct/<file>"). Left out: anything over 20 MB, logs, temporary
   files, the .bin caches (recsids.bin, acct/*.page.bin), the relaunch scripts, and every folder (cache, engine). */
public final class ExportProvider extends ContentProvider {
    static final long MAX = 20L << 20;

    @Override public boolean onCreate() { return true; }

    private File dataDir() { return new File(getContext().getFilesDir(), "data"); }

    private static boolean wanted(String name, boolean acct) {
        String n = name.toLowerCase();
        if (n.endsWith(".tmp") || n.contains(".tmp.") || n.endsWith(".log") || n.endsWith(".bin")) return false;
        if (acct) return n.endsWith(".json");
        return !n.startsWith("relaunch.") && !n.startsWith("applog");
    }
    private static void add(List<File> out, List<String> names, File dir, String prefix, boolean acct) {
        File[] fs = dir.listFiles();
        if (fs == null) return;
        Arrays.sort(fs);
        for (File f : fs)
            if (f.isFile() && f.length() <= MAX && wanted(f.getName(), acct)) { out.add(f); names.add(prefix + f.getName()); }
    }
    private void list(List<File> files, List<String> names) {
        File d = dataDir();
        add(files, names, d, "", false);
        add(files, names, new File(d, "acct"), "acct/", true);
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection, String[] args, String order) {
        if (!"/files".equals(uri.getPath())) return null;
        List<File> files = new ArrayList<>(); List<String> names = new ArrayList<>();
        list(files, names);
        MatrixCursor c = new MatrixCursor(new String[] { "name", "size" });
        for (int i = 0; i < files.size(); i++) c.addRow(new Object[] { names.get(i), files.get(i).length() });
        return c;
    }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new SecurityException("read only");
        List<String> seg = uri.getPathSegments();
        if (seg.size() < 2 || !"file".equals(seg.get(0))) throw new FileNotFoundException();
        StringBuilder sb = new StringBuilder();
        for (int i = 1; i < seg.size(); i++) sb.append(i > 1 ? "/" : "").append(seg.get(i));
        String name = sb.toString();
        if (name.contains("..") || name.contains("\\") || name.startsWith("/")) throw new FileNotFoundException();
        List<File> files = new ArrayList<>(); List<String> names = new ArrayList<>();
        list(files, names);
        int i = names.indexOf(name);
        if (i < 0) throw new FileNotFoundException(name);
        return ParcelFileDescriptor.open(files.get(i), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override public String getType(Uri uri) { return null; }
    @Override public Uri insert(Uri uri, ContentValues v) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri, ContentValues v, String s, String[] a) { return 0; }
    @Override public int delete(Uri uri, String s, String[] a) { return 0; }
}
