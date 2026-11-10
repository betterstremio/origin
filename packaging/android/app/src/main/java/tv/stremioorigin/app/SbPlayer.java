package tv.stremioorigin.app;

import android.animation.ValueAnimator;
import android.content.Context;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;

import androidx.annotation.NonNull;
import androidx.media3.common.C;
import androidx.media3.common.ColorInfo;
import androidx.media3.common.Format;
import androidx.media3.common.MediaItem;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.PlaybackException;
import androidx.media3.common.Player;
import androidx.media3.common.TrackGroup;
import androidx.media3.common.TrackSelectionOverride;
import androidx.media3.common.TrackSelectionParameters;
import androidx.media3.common.Tracks;
import androidx.media3.common.VideoSize;
import androidx.media3.common.text.CueGroup;
import androidx.media3.datasource.DefaultDataSource;
import androidx.media3.datasource.DefaultHttpDataSource;
import androidx.media3.exoplayer.DefaultRenderersFactory;
import androidx.media3.exoplayer.ExoPlayer;
import androidx.media3.exoplayer.source.DefaultMediaSourceFactory;
import androidx.media3.ui.SubtitleView;

import java.util.ArrayList;
import java.util.List;

/* The player behind src/platform/android/video_android.c. ExoPlayer lives on the main thread; the C side
   calls these static methods from SDL's thread, so every command is posted, and what the C side reads is a
   snapshot (poll) refreshed here every 50 ms. Rectangles arrive as fractions of the 16:9 box. */
@androidx.annotation.OptIn(markerClass = androidx.media3.common.util.UnstableApi.class)
public final class SbPlayer {
    static final String TAG = "StremioOrigin";
    static final Handler ui = new Handler(Looper.getMainLooper());
    static Context ctx;
    static FrameLayout clip;          // the window (or a crop's destination), clipping
    static SurfaceView surface;       // the picture, sized and placed inside the clip
    static SubtitleView subs;
    static ExoPlayer player;
    static ViewGroup box;

    // the snapshot (guarded by SbPlayer.class)
    static final double[] snap = new double[12];
    static int flags;                 // 1 tracks changed, 2 error changed, 4 HDR changed
    static String err = "", hdr = "", audioList = "", textList = "";
    // on the main thread
    static boolean everReady, firstFrame, seeking, hasError, audioOff;   // audioOff: the sound was switched off because it can't be played here
    static int vw, vh; static float fps;
    static float[] win = { 0, 0, 1, 1 }; static float[] crop;    // crop: src x,y,w,h (of the frame), dst x,y,w,h (of the box)
    static final List<int[]> audioMap = new ArrayList<>(), textMap = new ArrayList<>();   // our index -> { group, track }
    static ValueAnimator volAnim;
    static int textSel = -1;

    static void attach(Context c, ViewGroup b) {
        ctx = c; box = b;
        clip = new FrameLayout(c); clip.setClipChildren(true);
        surface = new SurfaceView(c);
        clip.addView(surface, new FrameLayout.LayoutParams(0, 0));
        subs = new SubtitleView(c); subs.setUserDefaultStyle(); subs.setUserDefaultTextSize();
        // the clip goes in a full-box frame so it can be placed anywhere inside
        FrameLayout holder = new FrameLayout(c) {
            @Override protected void onLayout(boolean ch, int l, int t, int r, int bt) { super.onLayout(ch, l, t, r, bt); place(); }
        };
        holder.addView(clip, new FrameLayout.LayoutParams(0, 0));
        b.addView(holder, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        b.addView(subs, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        clip.setVisibility(View.INVISIBLE);
        ui.post(tick);
    }
    static void release() { ui.removeCallbacksAndMessages(null); if (player != null) { player.release(); player = null; } }

    static ExoPlayer make() {
        DefaultHttpDataSource.Factory http = new DefaultHttpDataSource.Factory()
            .setAllowCrossProtocolRedirects(true).setConnectTimeoutMs(15000).setReadTimeoutMs(30000)
            .setUserAgent("StremioOrigin (Android; native)");
        DefaultRenderersFactory rf = new DefaultRenderersFactory(ctx).setEnableDecoderFallback(true)
            .setExtensionRendererMode(DefaultRenderersFactory.EXTENSION_RENDERER_MODE_PREFER);
        ExoPlayer p = new ExoPlayer.Builder(ctx, rf).setMediaSourceFactory(new DefaultMediaSourceFactory(new DefaultDataSource.Factory(ctx, http))).build();
        p.setVideoSurfaceView(surface);
        p.addListener(new Player.Listener() {
            @Override public void onPlaybackStateChanged(int s) { if (s == Player.STATE_READY) { everReady = true; seeking = false; } refresh(); }
            @Override public void onIsPlayingChanged(boolean playing) { keepOn(playing); refresh(); }
            @Override public void onRenderedFirstFrame() { firstFrame = true; clip.setVisibility(View.VISIBLE); refresh(); }
            @Override public void onVideoSizeChanged(@NonNull VideoSize v) { vw = v.width; vh = v.height; place(); refresh(); }
            @Override public void onTracksChanged(@NonNull Tracks t) { tracksChanged(t); }
            @Override public void onPlayerError(@NonNull PlaybackException e) {
                // the sound alone can't be played (a codec this device lacks): the picture goes on without it, as the TV's own
                // player does; the app sees "audio unsupported" and offers what it can (another track, another stream)
                String all = String.valueOf(e.getMessage()) + " " + (e.getCause() != null ? e.getCause().toString() : "");
                if (!audioOff && player != null && (all.contains("AudioRenderer") || e.errorCode == PlaybackException.ERROR_CODE_AUDIO_TRACK_INIT_FAILED || e.errorCode == PlaybackException.ERROR_CODE_AUDIO_TRACK_WRITE_FAILED)) {
                    Log.w(TAG, "player: the sound can't be played here (" + e.getErrorCodeName() + "): the picture goes on without it");
                    audioOff = true; long pos = player.getCurrentPosition();
                    player.setTrackSelectionParameters(player.getTrackSelectionParameters().buildUpon().setTrackTypeDisabled(C.TRACK_TYPE_AUDIO, true).build());
                    player.seekTo(pos); player.prepare(); player.play(); refresh(); return; }
                hasError = true; String m = e.getErrorCodeName() + (e.getMessage() != null ? ": " + e.getMessage() : "");
                Throwable c = e.getCause(); if (c != null && c.getMessage() != null) m += " (" + c.getMessage() + ")";
                Log.w(TAG, "player error " + m); synchronized (SbPlayer.class) { err = m; flags |= 2; } refresh(); }
            @Override public void onCues(@NonNull CueGroup g) { subs.setCues(g.cues); }
        });
        return p;
    }
    static void keepOn(boolean on) {
        MainActivity a = MainActivity.self; if (a == null) return;
        if (on) a.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); else a.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    // ---- commands from C ----
    public static void load(String url, double startSec) {
        ui.post(() -> {
            if (player == null) player = make();
            everReady = firstFrame = seeking = hasError = audioOff = false; vw = vh = 0; fps = 0; textSel = -1;
            clip.setVisibility(View.INVISIBLE); subs.setCues(new ArrayList<>());
            synchronized (SbPlayer.class) { err = ""; hdr = ""; audioList = ""; textList = ""; flags |= 7; }
            audioMap.clear(); textMap.clear();
            MediaItem.Builder mb = new MediaItem.Builder().setUri(Uri.parse(url));
            String lu = url.toLowerCase();
            if (lu.contains(".m3u8") || lu.contains("/hls/") || lu.contains("format=m3u8")) mb.setMimeType(MimeTypes.APPLICATION_M3U8);
            else if (lu.contains(".mpd")) mb.setMimeType(MimeTypes.APPLICATION_MPD);
            player.setTrackSelectionParameters(player.getTrackSelectionParameters().buildUpon().clearOverrides()
                .setTrackTypeDisabled(C.TRACK_TYPE_TEXT, true).setTrackTypeDisabled(C.TRACK_TYPE_AUDIO, false).build());
            player.setMediaItem(mb.build(), startSec > 0 ? (long) (startSec * 1000) : C.TIME_UNSET);
            player.setVolume(1f);
            player.prepare(); player.setPlayWhenReady(true);
            refresh();
        });
    }
    public static void unload() { ui.post(() -> { if (player != null) { player.stop(); player.clearMediaItems(); } clip.setVisibility(View.INVISIBLE); subs.setCues(new ArrayList<>()); keepOn(false); everReady = firstFrame = false; refresh(); }); }
    public static void play() { ui.post(() -> { if (player != null) player.play(); }); }
    public static void pause() { ui.post(() -> { if (player != null) player.pause(); }); }
    public static void seek(double sec) { ui.post(() -> { if (player != null) { seeking = true; player.seekTo((long) (sec * 1000)); refresh(); } }); }
    public static void setVolume(int pct, int easeMs) {
        ui.post(() -> {
            if (player == null) return; if (volAnim != null) volAnim.cancel();
            float to = Math.max(0, Math.min(100, pct)) / 100f;
            if (easeMs <= 0) { player.setVolume(to); return; }
            volAnim = ValueAnimator.ofFloat(player.getVolume(), to); volAnim.setDuration(easeMs);
            volAnim.addUpdateListener(a -> { if (player != null) player.setVolume((float) a.getAnimatedValue()); }); volAnim.start();
        });
    }
    public static void setWindow(float x, float y, float w, float h) { ui.post(() -> { win = new float[] { x, y, w, h }; crop = null; place(); }); }
    public static boolean setCrop(float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh) {
        ui.post(() -> { crop = new float[] { sx, sy, sw, sh, dx, dy, dw, dh }; place(); }); return true; }
    public static void selectAudio(int idx) { ui.post(() -> { audioOff = false; select(C.TRACK_TYPE_AUDIO, audioMap, idx); }); }   // another track chosen: tried again
    public static void selectText(int idx) { ui.post(() -> { textSel = idx; if (idx < 0) { if (player != null) player.setTrackSelectionParameters(player.getTrackSelectionParameters().buildUpon().setTrackTypeDisabled(C.TRACK_TYPE_TEXT, true).build()); subs.setCues(new ArrayList<>()); } else select(C.TRACK_TYPE_TEXT, textMap, idx); }); }
    public static String tracks(int kind) { synchronized (SbPlayer.class) { return kind == 0 ? audioList : textList; } }
    public static String error() { synchronized (SbPlayer.class) { return err; } }
    public static String hdr() { synchronized (SbPlayer.class) { return hdr; } }
    public static int poll(double[] out) { synchronized (SbPlayer.class) { System.arraycopy(snap, 0, out, 0, snap.length); int f = flags; flags = 0; return f; } }

    static void select(int type, List<int[]> map, int idx) {
        if (player == null || idx < 0 || idx >= map.size()) return;
        int[] gt = map.get(idx); Tracks t = player.getCurrentTracks(); int gi = 0;
        for (Tracks.Group g : t.getGroups()) { if (g.getType() != type) continue;
            if (gi++ == gt[0]) { player.setTrackSelectionParameters(player.getTrackSelectionParameters().buildUpon()
                    .setTrackTypeDisabled(type, false).setOverrideForType(new TrackSelectionOverride(g.getMediaTrackGroup(), gt[1])).build()); return; } }
    }

    // ---- the snapshot ----
    static final Runnable tick = new Runnable() { @Override public void run() { refresh(); ui.postDelayed(this, 50); } };
    static void refresh() {
        double st = 0, t = 0, dur = 0, buf = 0; boolean media = false; double audioBad = 0, audioCur = -1;
        if (player != null) {
            int s = player.getPlaybackState(); media = s != Player.STATE_IDLE;
            if (hasError) st = 6;
            else if (s == Player.STATE_IDLE) st = 0;
            else if (s == Player.STATE_BUFFERING) st = everReady ? 4 : 1;
            else if (s == Player.STATE_READY) st = player.getPlayWhenReady() ? 2 : 3;
            else if (s == Player.STATE_ENDED) st = 5;
            t = Math.max(0, player.getCurrentPosition() / 1000.0);
            long d = player.getDuration(); dur = d == C.TIME_UNSET || d <= 0 ? 0 : d / 1000.0;
            buf = player.getBufferedPosition() / 1000.0;
            int ai = 0; boolean anyAudio = false, audioOn = false, audioOk = true;
            for (Tracks.Group g : player.getCurrentTracks().getGroups()) {
                if (g.getType() != C.TRACK_TYPE_AUDIO) continue;
                for (int i = 0; i < g.length; i++) { anyAudio = true;
                    if (g.isTrackSelected(i)) { audioCur = ai; audioOn = true; audioOk = g.isTrackSupported(i); } ai++; } }
            if ((everReady && anyAudio && (!audioOn || !audioOk)) || audioOff) audioBad = 1;
        }
        synchronized (SbPlayer.class) {
            snap[0] = st; snap[1] = t; snap[2] = dur; snap[3] = buf; snap[4] = vw; snap[5] = vh; snap[6] = fps;
            snap[7] = firstFrame ? 1 : 0; snap[8] = media ? 1 : 0; snap[9] = seeking ? 1 : 0; snap[10] = audioBad; snap[11] = audioCur;
        }
    }
    static String codec(String mime) {
        if (mime == null) return "";
        switch (mime) {
            case MimeTypes.AUDIO_AC3: return "ac3"; case MimeTypes.AUDIO_E_AC3: case MimeTypes.AUDIO_E_AC3_JOC: return "eac3";
            case MimeTypes.AUDIO_DTS: case MimeTypes.AUDIO_DTS_HD: case MimeTypes.AUDIO_DTS_EXPRESS: case MimeTypes.AUDIO_DTS_X: return "dts";
            case MimeTypes.AUDIO_TRUEHD: return "truehd"; case MimeTypes.AUDIO_AAC: return "aac"; case MimeTypes.AUDIO_OPUS: return "opus";
            case MimeTypes.AUDIO_FLAC: return "flac"; case MimeTypes.AUDIO_MPEG: return "mp3"; case MimeTypes.AUDIO_VORBIS: return "vorbis";
            case MimeTypes.TEXT_SSA: return "ass"; case MimeTypes.APPLICATION_SUBRIP: return "srt"; case MimeTypes.TEXT_VTT: return "webvtt";
            case MimeTypes.APPLICATION_PGS: return "pgs"; case MimeTypes.APPLICATION_VOBSUB: return "vobsub";
            default: int k = mime.indexOf('/'); return k >= 0 ? mime.substring(k + 1) : mime;
        }
    }
    static String clean(String s) { return s == null ? "" : s.replace('\t', ' ').replace('\n', ' '); }
    static void tracksChanged(Tracks t) {
        StringBuilder a = new StringBuilder(), x = new StringBuilder(); audioMap.clear(); textMap.clear();
        int ga = 0, gx = 0; String h = "";
        for (Tracks.Group g : t.getGroups()) {
            TrackGroup mg = g.getMediaTrackGroup();
            if (g.getType() == C.TRACK_TYPE_VIDEO) {
                for (int i = 0; i < g.length; i++) if (g.isTrackSelected(i)) { Format f = g.getTrackFormat(i);
                    if (f.frameRate > 0) fps = f.frameRate;
                    ColorInfo ci = f.colorInfo;
                    if (MimeTypes.VIDEO_DOLBY_VISION.equals(f.sampleMimeType)) h = "DolbyVision";
                    else if (ci != null && ci.colorTransfer == C.COLOR_TRANSFER_ST2084) h = "HDR10";
                    else if (ci != null && ci.colorTransfer == C.COLOR_TRANSFER_HLG) h = "HLG";
                    else h = "none"; }
            } else if (g.getType() == C.TRACK_TYPE_AUDIO || g.getType() == C.TRACK_TYPE_TEXT) {
                boolean au = g.getType() == C.TRACK_TYPE_AUDIO; List<int[]> map = au ? audioMap : textMap; StringBuilder sb = au ? a : x;
                for (int i = 0; i < g.length; i++) { Format f = g.getTrackFormat(i);
                    int idx = map.size(); map.add(new int[] { au ? ga : gx, i });
                    String name = f.label != null ? f.label : f.language != null ? f.language : "";
                    sb.append(idx).append('\t').append(clean(f.language)).append('\t').append(clean(name)).append('\t').append(clean(codec(f.sampleMimeType)))
                      .append('\t').append((f.selectionFlags & C.SELECTION_FLAG_FORCED) != 0 ? 1 : 0).append('\t').append(Math.max(0, f.channelCount)).append('\n'); }
                if (au) ga++; else gx++;
            }
        }
        synchronized (SbPlayer.class) { audioList = a.toString(); textList = x.toString(); flags |= 1; if (!h.equals(hdr)) { hdr = h; flags |= 4; } }
        refresh();
    }

    /* the clip at the window (or the crop's destination); the picture inside it fitted to its shape, or scaled so
       the crop's source part fills the destination */
    static void place() {
        if (clip == null || box == null) return;
        int W = box.getWidth(), H = box.getHeight(); if (W <= 0 || H <= 0) return;
        // the box's own child area is the 16:9 rectangle: use the holder's size
        View holder = (View) clip.getParent(); if (holder != null && holder.getWidth() > 0) { W = holder.getWidth(); H = holder.getHeight(); }
        int cx, cy, cw, ch, sx, sy, sw, sh;
        if (crop != null && vw > 0 && vh > 0) {
            cx = Math.round(crop[4] * W); cy = Math.round(crop[5] * H); cw = Math.round(crop[6] * W); ch = Math.round(crop[7] * H);
            sw = Math.round(cw / crop[2]); sh = Math.round(ch / crop[3]); sx = -Math.round(crop[0] * sw); sy = -Math.round(crop[1] * sh);
        } else {
            cx = Math.round(win[0] * W); cy = Math.round(win[1] * H); cw = Math.round(win[2] * W); ch = Math.round(win[3] * H);
            sw = cw; sh = ch; sx = 0; sy = 0;
            if (vw > 0 && vh > 0 && cw > 0 && ch > 0) { double ar = (double) vw / vh;
                if ((double) cw / ch > ar) { sw = (int) Math.round(ch * ar); sx = (cw - sw) / 2; } else { sh = (int) Math.round(cw / ar); sy = (ch - sh) / 2; } }
        }
        FrameLayout.LayoutParams lc = (FrameLayout.LayoutParams) clip.getLayoutParams();
        if (lc.leftMargin != cx || lc.topMargin != cy || lc.width != cw || lc.height != ch) { lc.leftMargin = cx; lc.topMargin = cy; lc.width = Math.max(1, cw); lc.height = Math.max(1, ch); clip.setLayoutParams(lc); }
        FrameLayout.LayoutParams ls = (FrameLayout.LayoutParams) surface.getLayoutParams();
        if (ls.leftMargin != sx || ls.topMargin != sy || ls.width != sw || ls.height != sh) { ls.leftMargin = sx; ls.topMargin = sy; ls.width = Math.max(1, sw); ls.height = Math.max(1, sh); surface.setLayoutParams(ls); }
    }
}
