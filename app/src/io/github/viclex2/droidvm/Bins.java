package io.github.viclex2.droidvm;

import android.content.Context;
import android.content.res.AssetManager;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/** Binarios de QEMU (van como lib*.so para que Android permita ejecutarlos) y sus datos. */
final class Bins {
    static String nativeDir(Context c) { return c.getApplicationInfo().nativeLibraryDir; }

    static File qemu(Context c, String arch) { return new File(nativeDir(c), "libqemu_" + arch + ".so"); }

    static File img(Context c) { return new File(nativeDir(c), "libqemu_img.so"); }

    static boolean present(Context c) { return qemu(c, "aarch64").exists() && img(c).exists(); }

    static File share(Context c) { return new File(c.getFilesDir(), "qemu-share"); }

    static synchronized boolean shareReady(Context c) {
        File stamp = new File(share(c), ".version");
        return stamp.isFile() && readSmall(stamp).equals(String.valueOf(BuildInfo.CODE));
    }

    /** Copia firmware (UEFI/BIOS) y mapas de teclado desde assets la primera vez o tras actualizar. */
    static synchronized void ensureShare(Context c, U.Cb<String> status) throws IOException {
        if (shareReady(c)) return;
        File d = share(c);
        U.rmrf(d);
        d.mkdirs();
        AssetManager am = c.getAssets();
        extract(am, "qemu", d, status);
        FileOutputStream o = new FileOutputStream(new File(d, ".version"));
        o.write(String.valueOf(BuildInfo.CODE).getBytes());
        o.close();
    }

    private static void extract(AssetManager am, String path, File dest, U.Cb<String> status) throws IOException {
        String[] l = am.list(path);
        if (l == null || l.length == 0) {
            if (status != null) status.on(dest.getName());
            InputStream in = am.open(path);
            U.copy(in, new FileOutputStream(dest), -1, null);
            return;
        }
        dest.mkdirs();
        for (String n : l) extract(am, path + "/" + n, new File(dest, n), status);
    }

    static String readSmall(File f) {
        try {
            BufferedReader r = new BufferedReader(new InputStreamReader(new java.io.FileInputStream(f), "UTF-8"));
            try { String s = r.readLine(); return s == null ? "" : s.trim(); } finally { r.close(); }
        } catch (IOException e) { return ""; }
    }

    static void env(Context c, Map<String, String> env) {
        env.put("HOME", c.getFilesDir().getAbsolutePath());
        env.put("TMPDIR", c.getCacheDir().getAbsolutePath());
        env.put("QEMU_AUDIO_DRV", "none");
    }

    static final class Result {
        int code; String out;
        Result(int c, String o) { code = c; out = o; }
    }

    /** Ejecuta qemu-img y devuelve código y salida. */
    static Result img(Context c, String... args) {
        List<String> cmd = new ArrayList<String>();
        cmd.add(img(c).getAbsolutePath());
        cmd.addAll(Arrays.asList(args));
        return run(c, cmd);
    }

    static Result run(Context c, List<String> cmd) {
        try {
            ProcessBuilder pb = new ProcessBuilder(cmd).redirectErrorStream(true);
            env(c, pb.environment());
            Process p = pb.start();
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), "UTF-8"));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = r.readLine()) != null) sb.append(line).append('\n');
            return new Result(p.waitFor(), sb.toString());
        } catch (Exception e) {
            return new Result(-1, String.valueOf(e));
        }
    }

    static boolean kvmUsable() {
        File k = new File("/dev/kvm");
        return k.exists() && k.canRead() && k.canWrite();
    }
}
