package tv.stremioorigin.app;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.app.job.JobInfo;
import android.app.job.JobParameters;
import android.app.job.JobScheduler;
import android.app.job.JobService;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

/** Holds Android's background execution token while the native queue has work. */
public final class SbDownloads {
    private static final String CHANNEL = "video-downloads";
    private static final int ID = 7321;
    private static final int IDLE = -2;
    private static volatile int progress = IDLE;
    private static volatile boolean running;
    private static boolean scheduled;

    private SbDownloads() {}

    static boolean update(MainActivity activity, int value, boolean visible) {
        boolean changed = progress != value;
        progress = value;
        activity.runOnUiThread(() -> {
            if (progress != value) return;
            if (value == IDLE) {
                if (scheduled) {
                    scheduled = false;
                    if (Build.VERSION.SDK_INT >= 34) {
                        activity.getSystemService(JobScheduler.class).cancel(ID);
                    } else {
                        activity.stopService(new Intent(activity, LegacyService.class));
                    }
                }
                return;
            }
            if (visible && activity.hasWindowFocus() && !scheduled) {
                try {
                    if (Build.VERSION.SDK_INT >= 34) {
                        JobInfo info = new JobInfo.Builder(ID, new ComponentName(activity, TransferJob.class))
                                .setUserInitiated(true)
                                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                                .build();
                        scheduled = activity.getSystemService(JobScheduler.class).schedule(info)
                                == JobScheduler.RESULT_SUCCESS;
                    } else {
                        Intent intent = new Intent(activity, LegacyService.class);
                        if (Build.VERSION.SDK_INT >= 26) activity.startForegroundService(intent);
                        else activity.startService(intent);
                        scheduled = true;
                    }
                    if (scheduled) askNotificationPermission(activity);
                } catch (IllegalStateException | SecurityException e) {
                    android.util.Log.w("StremioOrigin", "Android deferred the download task");
                }
            }
            if (changed && running) activity.getSystemService(NotificationManager.class)
                    .notify(ID, notification(activity));
        });
        return running;
    }

    private static void askNotificationPermission(MainActivity activity) {
        if (Build.VERSION.SDK_INT < 33 ||
                activity.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED)
            return;
        android.content.SharedPreferences prefs = activity.getPreferences(Context.MODE_PRIVATE);
        if (!prefs.getBoolean("askedDownloadNotifications", false)) {
            prefs.edit().putBoolean("askedDownloadNotifications", true).apply();
            activity.requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, 732);
        }
    }

    private static Notification notification(Context context) {
        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel channel = new NotificationChannel(CHANNEL, "Video downloads", NotificationManager.IMPORTANCE_LOW);
            context.getSystemService(NotificationManager.class).createNotificationChannel(channel);
            builder = new Notification.Builder(context, CHANNEL);
        } else {
            builder = new Notification.Builder(context);
        }
        Intent show = new Intent(context, MainActivity.class);
        int value = progress;
        return builder.setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle("Origin")
                .setContentText("Downloading for offline viewing")
                .setContentIntent(PendingIntent.getActivity(context, ID, show,
                        PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT))
                .setOnlyAlertOnce(true)
                .setOngoing(true)
                .setProgress(100, Math.max(0, value), value < 0)
                .build();
    }

    public static final class TransferJob extends JobService {
        @Override public boolean onStartJob(JobParameters params) {
            if (progress == IDLE || MainActivity.self == null) {
                scheduled = false;
                return false;
            }
            setNotification(params, ID, notification(this), JOB_END_NOTIFICATION_POLICY_REMOVE);
            scheduled = running = true;
            return true;
        }

        @Override public boolean onStopJob(JobParameters params) {
            running = false;
            scheduled = false;
            return false;
        }
    }

    public static final class LegacyService extends Service {
        private PowerManager.WakeLock wake;

        @Override public int onStartCommand(Intent intent, int flags, int startId) {
            startForeground(ID, notification(this));
            if (progress == IDLE || MainActivity.self == null) {
                stopSelf();
                return START_NOT_STICKY;
            }
            if (wake == null) {
                wake = getSystemService(PowerManager.class).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Origin:Downloads");
                wake.acquire(6 * 60 * 60 * 1000L);
            }
            running = true;
            return START_NOT_STICKY;
        }

        @Override public IBinder onBind(Intent intent) { return null; }

        @Override public void onDestroy() {
            running = scheduled = false;
            if (wake != null && wake.isHeld()) wake.release();
            super.onDestroy();
        }
    }
}
