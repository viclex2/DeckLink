package io.github.viclex2.droidvm;

import android.content.Context;
import android.content.Intent;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Una VM en marcha: el proceso de QEMU, su registro y el canal de control (QMP). */
final class Runner {
    interface Listener { void changed(); }

    static final Map<String, Runner> RUNNING = new LinkedHashMap<String, Runner>();
    static final List<Listener> LISTENERS = new ArrayList<Listener>();

    final Vm vm;
    final String vncSock, qmpSock;
    final long started = System.currentTimeMillis();
    private Process proc;
    private final StringBuilder log = new StringBuilder();
    volatile boolean alive = true;
    volatile int exitCode = 0;
    volatile boolean paused = false;
    String cmdLine = "";

    private Runner(Vm vm, String vnc, String qmp) { this.vm = vm; vncSock = vnc; qmpSock = qmp; }

    static synchronized Runner get(String id) { return RUNNING.get(id); }

    static synchronized List<Runner> all() { return new ArrayList<Runner>(RUNNING.values()); }

    static void addListener(Listener l) { synchronized (LISTENERS) { LISTENERS.add(l); } }

    static void removeListener(Listener l) { synchronized (LISTENERS) { LISTENERS.remove(l); } }

    static void fire() {
        U.ui(new Runnable() { public void run() {
            List<Listener> ls;
            synchronized (LISTENERS) { ls = new ArrayList<Listener>(LISTENERS); }
            for (Listener l : ls) l.changed();
        } });
    }

    static Runner start(final Context ctx, Vm vm) throws IOException {
        final Context c = ctx.getApplicationContext();
        synchronized (Runner.class) {
            if (RUNNING.containsKey(vm.id)) return RUNNING.get(vm.id);
        }
        if (!Bins.present(c)) throw new IOException("Este APK no lleva QEMU (faltan los binarios para arm64-v8a)");
        Bins.ensureShare(c, null);
        File rd = vm.runDir(c);
        File vnc = new File(rd, "vnc"), qmp = new File(rd, "qmp");
        vnc.delete(); qmp.delete();
        List<String> cmd = QemuCmd.build(c, vm, vnc.getAbsolutePath(), qmp.getAbsolutePath());
        new File(vm.dir(c), Snapshots.SAVED_TAG).delete();
        final Runner r = new Runner(vm, vnc.getAbsolutePath(), qmp.getAbsolutePath());
        StringBuilder cl = new StringBuilder();
        for (String s : cmd) cl.append(s.contains(" ") ? "'" + s + "'" : s).append(' ');
        r.cmdLine = cl.toString();
        r.append("$ " + r.cmdLine + "\n\n");
        ProcessBuilder pb = new ProcessBuilder(cmd).redirectErrorStream(true).directory(vm.dir(c));
        Bins.env(c, pb.environment());
        r.proc = pb.start();
        synchronized (Runner.class) { RUNNING.put(vm.id, r); }
        Thread t = new Thread(new Runnable() { public void run() {
            try {
                BufferedReader br = new BufferedReader(new InputStreamReader(r.proc.getInputStream(), "UTF-8"));
                String line;
                while ((line = br.readLine()) != null) r.append(line + "\n");
            } catch (IOException ignored) { }
            try { r.exitCode = r.proc.waitFor(); } catch (InterruptedException ignored) { }
            r.alive = false;
            r.append("\n[QEMU ha terminado con código " + r.exitCode + "]\n");
            synchronized (Runner.class) { RUNNING.remove(r.vm.id); }
            VmService.update(c);
            fire();
        } }, "qemu-" + vm.id);
        t.setDaemon(true);
        t.start();
        try {
            c.startService(new Intent(c, VmService.class));
        } catch (Exception e) {
            r.append("[aviso] no se pudo iniciar el servicio: " + e + "\n");
        }
        fire();
        return r;
    }

    synchronized void append(String s) {
        log.append(s);
        if (log.length() > 300000) log.delete(0, log.length() - 200000);
    }

    synchronized String log() { return log.toString(); }

    String lastLines(int n) {
        String l = log();
        String[] ls = l.split("\n");
        StringBuilder sb = new StringBuilder();
        for (int i = Math.max(0, ls.length - n); i < ls.length; i++) sb.append(ls[i]).append('\n');
        return sb.toString();
    }

    void kill() {
        if (proc != null) proc.destroy();
    }

    // ---------- QMP ----------

    private static JSONObject readReply(BufferedReader r) throws Exception {
        String line;
        while ((line = r.readLine()) != null) {
            line = line.trim();
            if (line.length() == 0) continue;
            JSONObject o = new JSONObject(line);
            if (o.has("return") || o.has("error")) return o;
        }
        throw new IOException("QMP cerrado");
    }

    /** Ejecuta una orden QMP. Devuelve el objeto "return" o lanza una excepción con el error. */
    synchronized Object qmp(String exec, JSONObject args, int timeoutMs) throws Exception {
        LocalSocket s = new LocalSocket();
        try {
            s.connect(new LocalSocketAddress(qmpSock, LocalSocketAddress.Namespace.FILESYSTEM));
            s.setSoTimeout(timeoutMs);
            BufferedReader r = new BufferedReader(new InputStreamReader(s.getInputStream(), "UTF-8"));
            OutputStream w = s.getOutputStream();
            r.readLine();
            w.write("{\"execute\":\"qmp_capabilities\"}\n".getBytes("UTF-8"));
            w.flush();
            readReply(r);
            JSONObject c = new JSONObject();
            c.put("execute", exec);
            if (args != null) c.put("arguments", args);
            w.write((c.toString() + "\n").getBytes("UTF-8"));
            w.flush();
            JSONObject res = readReply(r);
            if (res.has("error")) throw new IOException(res.getJSONObject("error").optString("desc", res.toString()));
            return res.opt("return");
        } finally {
            try { s.close(); } catch (IOException ignored) { }
        }
    }

    /** Orden del monitor humano (HMP), p. ej. "savevm x". */
    String hmp(String line, int timeoutMs) throws Exception {
        JSONObject a = new JSONObject();
        a.put("command-line", line);
        Object r = qmp("human-monitor-command", a, timeoutMs);
        String out = r == null ? "" : r.toString().trim();
        append("(qemu) " + line + "\n" + (out.length() > 0 ? out + "\n" : ""));
        return out;
    }

    void simple(String exec) throws Exception { qmp(exec, null, 15000); }

    boolean queryPaused() {
        try {
            Object r = qmp("query-status", null, 5000);
            if (r instanceof JSONObject) paused = !((JSONObject) r).optBoolean("running", true);
        } catch (Exception ignored) { }
        return paused;
    }
}
