package io.github.viclex2.droidvm;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

import java.util.List;

/** Servicio en primer plano: mantiene vivas las VMs (y la CPU despierta) aunque la app no esté delante. */
public final class VmService extends Service {
    static final String CHANNEL = "vms";
    private static VmService instance;
    private PowerManager.WakeLock wake;

    static void update(Context c) {
        final VmService s = instance;
        if (s != null) U.ui(new Runnable() { public void run() { s.refresh(); } });
    }

    @Override public void onCreate() {
        super.onCreate();
        instance = this;
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "droidvm:vm");
        wake.acquire();
    }

    @Override public int onStartCommand(Intent i, int flags, int id) {
        if (i != null && "stopall".equals(i.getAction())) {
            for (Runner r : Runner.all()) {
                try { r.simple("system_powerdown"); } catch (Exception e) { r.kill(); }
            }
        }
        refresh();
        return START_NOT_STICKY;
    }

    void refresh() {
        List<Runner> rs = Runner.all();
        if (rs.isEmpty()) {
            stopForeground(true);
            stopSelf();
            return;
        }
        StringBuilder names = new StringBuilder();
        for (Runner r : rs) { if (names.length() > 0) names.append(", "); names.append(r.vm.name); }
        startForeground(1, build(rs.size() == 1 ? "1 máquina en marcha" : rs.size() + " máquinas en marcha", names.toString()));
    }

    @SuppressWarnings("deprecation")
    private Notification build(String title, String text) {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= 26) {
            try {
                Class<?> ch = Class.forName("android.app.NotificationChannel");
                Object chan = ch.getConstructor(String.class, CharSequence.class, int.class)
                        .newInstance(CHANNEL, "Máquinas en marcha", 2 /* IMPORTANCE_LOW */);
                NotificationManager.class.getMethod("createNotificationChannel", ch).invoke(nm, chan);
                b = Notification.Builder.class.getConstructor(Context.class, String.class).newInstance(this, CHANNEL);
            } catch (Exception e) {
                b = new Notification.Builder(this);
            }
        } else {
            b = new Notification.Builder(this);
        }
        int fl = Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0;
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), fl);
        PendingIntent stop = PendingIntent.getService(this, 1, new Intent(this, VmService.class).setAction("stopall"), fl);
        b.setContentTitle(title).setContentText(text).setSmallIcon(R.drawable.ic_stat)
                .setContentIntent(open).setOngoing(true)
                .addAction(0, "Apagar todas", stop);
        return b.build();
    }

    @Override public void onDestroy() {
        instance = null;
        if (wake != null && wake.isHeld()) wake.release();
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent i) { return null; }
}
