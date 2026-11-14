package io.github.freefrank.lostodyssey;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

/**
 * Steprant 2.1.1: keeps the game process at foreground-service priority while
 * the game runs, so Android and HyperOS do not reclaim a 3-3.5 GB game the
 * moment it goes to the background (killed in the background on 12/11 and
 * 14/11). It does no work of its own: it only holds the ongoing notification
 * that Android requires for that priority. RuntimeActivity starts it while in
 * the foreground and stops it when the game ends; tapping the notification
 * returns to the game.
 */
public final class GameKeepAliveService extends Service {
    private static final String TAG = "LostOdysseyKeepAlive";
    private static final String CHANNEL = "game_running";
    private static final int NOTIFICATION_ID = 0x10D7;

    static void start(Context context) {
        try {
            Intent intent = new Intent(context, GameKeepAliveService.class);
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent);
            else context.startService(intent);
        } catch (RuntimeException refused) {
            // Background start restrictions or a vendor policy: play on without it.
            Log.w(TAG, "keep-alive service not started", refused);
        }
    }

    static void stop(Context context) {
        try { context.stopService(new Intent(context, GameKeepAliveService.class)); }
        catch (RuntimeException ignored) { }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        try {
            Notification notification = build();
            if (Build.VERSION.SDK_INT >= 34)
                startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            else
                startForeground(NOTIFICATION_ID, notification);
        } catch (RuntimeException refused) {
            Log.w(TAG, "foreground state refused", refused);
            stopSelf();
        }
        // Not restarted by itself after the process dies: the game restarts it.
        return START_NOT_STICKY;
    }

    private Notification build() {
        NotificationManager manager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= 26 && manager != null && manager.getNotificationChannel(CHANNEL) == null) {
            NotificationChannel channel = new NotificationChannel(CHANNEL,
                getString(R.string.keep_alive_channel), NotificationManager.IMPORTANCE_LOW);
            channel.setDescription(getString(R.string.keep_alive_channel_description));
            channel.setShowBadge(false);
            channel.setSound(null, null);
            channel.enableVibration(false);
            manager.createNotificationChannel(channel);
        }
        Intent open = new Intent(this, RuntimeActivity.class)
            .setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        PendingIntent tap = PendingIntent.getActivity(this, 0, open,
            PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
            ? new Notification.Builder(this, CHANNEL) : new Notification.Builder(this);
        builder.setSmallIcon(getApplicationInfo().icon)
            .setContentTitle(getString(R.string.keep_alive_title))
            .setContentText(getString(R.string.keep_alive_text))
            .setContentIntent(tap)
            .setOngoing(true)
            .setShowWhen(false)
            .setCategory(Notification.CATEGORY_SERVICE);
        if (Build.VERSION.SDK_INT >= 31)
            builder.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        return builder.build();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
