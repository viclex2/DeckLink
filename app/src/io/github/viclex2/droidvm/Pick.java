package io.github.viclex2.droidvm;

import android.app.Activity;
import android.app.ProgressDialog;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

/** Elegir ficheros/carpetas con el selector de Android y convertirlos en rutas que QEMU pueda abrir. */
final class Pick {
    static void file(Activity a, int req) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        a.startActivityForResult(i, req);
    }

    static void folder(Activity a, int req) {
        a.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), req);
    }

    static boolean hasStorage(Context c) {
        if (Build.VERSION.SDK_INT < 23) return true;
        return c.checkSelfPermission("android.permission.READ_EXTERNAL_STORAGE") == 0
                && c.checkSelfPermission("android.permission.WRITE_EXTERNAL_STORAGE") == 0;
    }

    static void askStorage(Activity a) {
        if (Build.VERSION.SDK_INT >= 23 && !hasStorage(a))
            a.requestPermissions(new String[]{"android.permission.READ_EXTERNAL_STORAGE",
                    "android.permission.WRITE_EXTERNAL_STORAGE"}, 77);
    }

    /** Ruta real para documentos del almacenamiento interno o tarjetas SD; null si no se puede saber. */
    static String path(Context c, Uri u, boolean tree) {
        try {
            String auth = u.getAuthority();
            if ("file".equals(u.getScheme())) return u.getPath();
            String id = tree ? DocumentsContract.getTreeDocumentId(u) : DocumentsContract.getDocumentId(u);
            if ("com.android.externalstorage.documents".equals(auth)) {
                int k = id.indexOf(':');
                String vol = id.substring(0, k), rel = id.substring(k + 1);
                String base = "primary".equalsIgnoreCase(vol) ? Environment.getExternalStorageDirectory().getAbsolutePath()
                        : "/storage/" + vol;
                return rel.length() == 0 ? base : base + "/" + rel;
            }
            if ("com.android.providers.downloads.documents".equals(auth)) {
                if (id.startsWith("raw:")) return id.substring(4);
                String name = name(c, u);
                if (name != null) {
                    File f = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), name);
                    if (f.exists()) return f.getAbsolutePath();
                }
            }
        } catch (Exception ignored) { }
        return null;
    }

    static String name(Context c, Uri u) {
        Cursor cur = null;
        try {
            cur = c.getContentResolver().query(u, new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE}, null, null, null);
            if (cur != null && cur.moveToFirst()) return cur.getString(0);
        } catch (Exception ignored) {
        } finally { if (cur != null) cur.close(); }
        String s = u.getLastPathSegment();
        return s == null ? "fichero" : s.replaceAll(".*[/:]", "");
    }

    static long size(Context c, Uri u) {
        Cursor cur = null;
        try {
            cur = c.getContentResolver().query(u, new String[]{OpenableColumns.SIZE}, null, null, null);
            if (cur != null && cur.moveToFirst() && !cur.isNull(0)) return cur.getLong(0);
        } catch (Exception ignored) {
        } finally { if (cur != null) cur.close(); }
        return -1;
    }

    /**
     * Devuelve una ruta legible por QEMU para el fichero elegido: la real si se puede leer directamente,
     * o una copia en la carpeta de la app (con barra de progreso).
     */
    static void resolve(final Activity a, final Uri u, final File copyDir, final U.Cb<String> done) {
        String p = path(a, u, false);
        if (p != null && new File(p).canRead()) { done.on(p); return; }
        final String name = name(a, u);
        final long total = size(a, u);
        final ProgressDialog pd = U.progress(a, "Copiando " + name + " a la app…");
        U.bg(new Runnable() { public void run() {
            try {
                copyDir.mkdirs();
                final File dest = new File(copyDir, name.replaceAll("[/\\\\:]", "_"));
                InputStream in = a.getContentResolver().openInputStream(u);
                U.copy(in, new FileOutputStream(dest), total, new U.Progress() {
                    public void on(long d, long t) {
                        U.setProgress(pd, "Copiando " + name + "…\n" + U.human(d) + (t > 0 ? " de " + U.human(t) : ""));
                    }
                });
                U.dismiss(pd);
                U.ui(new Runnable() { public void run() { done.on(dest.getAbsolutePath()); } });
            } catch (final Exception e) {
                U.dismiss(pd);
                U.alert(a, "No se pudo copiar", String.valueOf(e.getMessage()));
            }
        } });
    }
}
