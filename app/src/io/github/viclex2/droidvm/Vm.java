package io.github.viclex2.droidvm;

import android.content.Context;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/** Configuración de una máquina virtual (se guarda en vms/&lt;id&gt;/config.json). */
final class Vm {
    static final String[] OS_IDS = {"win11-arm", "linux-arm", "linux-x86", "win-x86", "otro-arm", "otro-x86"};
    static final String[] OS_NAMES = {"Windows 11 ARM64", "Linux ARM64 (aarch64)", "Linux x86_64 (PC, emulado)",
            "Windows x86_64 (PC, emulado)", "Otro ARM64", "Otro x86_64"};

    String id, name = "Nueva máquina", os = "linux-arm", arch = "aarch64", machine = "virt", cpu = "max";
    int ram = 2048, cpus = 2;
    String accel = "auto";
    boolean tcgMulti = true, uefi = true;
    String disk = "", diskIf = "virtio", disk2 = "";
    String iso = "", cdIf = "scsi";
    boolean bootCd = true;
    String display = "virtio-gpu", res = "1280x800";
    String net = "user", nic = "virtio", fwd = "";
    String share = "";
    boolean shareRw = false;
    String keymap = "es";
    boolean winBypass = false, rtcLocal = false, usbKbd = true, sound = false;
    String extra = "";
    long created;

    boolean isWin() { return os.startsWith("win"); }
    boolean isArm() { return "aarch64".equals(arch); }

    static Vm preset(String os, int cores) {
        Vm v = new Vm();
        v.os = os;
        int c = Math.max(1, Math.min(4, cores));
        v.cpus = c;
        if (os.equals("win11-arm")) {
            v.name = "Windows 11 ARM"; v.arch = "aarch64"; v.machine = "virt"; v.ram = 4096;
            v.diskIf = "nvme"; v.cdIf = "usb"; v.display = "ramfb"; v.nic = "usb-net";
            v.winBypass = true; v.rtcLocal = true;
        } else if (os.equals("linux-arm") || os.equals("otro-arm")) {
            v.name = os.equals("linux-arm") ? "Linux ARM64" : "Máquina ARM64"; v.arch = "aarch64"; v.machine = "virt";
            v.ram = 2048; v.diskIf = "virtio"; v.cdIf = "scsi"; v.display = "virtio-gpu"; v.nic = "virtio";
        } else if (os.equals("win-x86")) {
            v.name = "Windows x64"; v.arch = "x86_64"; v.machine = "q35"; v.ram = 4096;
            v.diskIf = "sata"; v.cdIf = "sata"; v.display = "std"; v.nic = "e1000"; v.uefi = true;
            v.winBypass = true; v.rtcLocal = true;
        } else {
            v.name = os.equals("linux-x86") ? "Linux x86_64" : "Máquina x86_64"; v.arch = "x86_64"; v.machine = "q35";
            v.ram = 2048; v.diskIf = "virtio"; v.cdIf = "sata"; v.display = "std"; v.nic = "virtio"; v.uefi = false;
        }
        v.usbKbd = v.isArm();
        return v;
    }

    JSONObject toJson() throws JSONException {
        JSONObject o = new JSONObject();
        o.put("id", id); o.put("name", name); o.put("os", os); o.put("arch", arch); o.put("machine", machine);
        o.put("cpu", cpu); o.put("ram", ram); o.put("cpus", cpus); o.put("accel", accel); o.put("tcgMulti", tcgMulti);
        o.put("uefi", uefi); o.put("disk", disk); o.put("diskIf", diskIf); o.put("disk2", disk2); o.put("iso", iso);
        o.put("cdIf", cdIf); o.put("bootCd", bootCd); o.put("display", display); o.put("res", res); o.put("net", net);
        o.put("nic", nic); o.put("fwd", fwd); o.put("share", share); o.put("shareRw", shareRw); o.put("keymap", keymap);
        o.put("winBypass", winBypass); o.put("rtcLocal", rtcLocal); o.put("usbKbd", usbKbd); o.put("sound", sound);
        o.put("extra", extra); o.put("created", created);
        return o;
    }

    static Vm fromJson(JSONObject o) {
        Vm v = new Vm();
        v.id = o.optString("id"); v.name = o.optString("name", v.name); v.os = o.optString("os", v.os);
        v.arch = o.optString("arch", v.arch); v.machine = o.optString("machine", v.machine); v.cpu = o.optString("cpu", v.cpu);
        v.ram = o.optInt("ram", v.ram); v.cpus = o.optInt("cpus", v.cpus); v.accel = o.optString("accel", v.accel);
        v.tcgMulti = o.optBoolean("tcgMulti", true); v.uefi = o.optBoolean("uefi", true); v.disk = o.optString("disk");
        v.diskIf = o.optString("diskIf", v.diskIf); v.disk2 = o.optString("disk2"); v.iso = o.optString("iso");
        v.cdIf = o.optString("cdIf", v.cdIf); v.bootCd = o.optBoolean("bootCd", true); v.display = o.optString("display", v.display);
        v.res = o.optString("res", v.res); v.net = o.optString("net", v.net); v.nic = o.optString("nic", v.nic);
        v.fwd = o.optString("fwd"); v.share = o.optString("share"); v.shareRw = o.optBoolean("shareRw", false);
        v.keymap = o.optString("keymap", "es"); v.winBypass = o.optBoolean("winBypass", false);
        v.rtcLocal = o.optBoolean("rtcLocal", false); v.usbKbd = o.optBoolean("usbKbd", v.isArm());
        v.sound = o.optBoolean("sound", false); v.extra = o.optString("extra"); v.created = o.optLong("created");
        return v;
    }

    Vm copy() { return fromJson(safeJson()); }

    JSONObject safeJson() {
        try { return toJson(); } catch (JSONException e) { throw new RuntimeException(e); }
    }

    // ---- almacenamiento ----

    static File root(Context c) {
        File ext = c.getExternalFilesDir(null);
        File r = new File(ext != null ? ext : c.getFilesDir(), "vms");
        r.mkdirs();
        return r;
    }

    File dir(Context c) { File d = new File(root(c), id); d.mkdirs(); return d; }

    /** Carpeta interna corta para los sockets (la ruta de un socket unix no puede pasar de 107 bytes). */
    File runDir(Context c) { File d = new File(new File(c.getFilesDir(), "run"), id); d.mkdirs(); return d; }

    void save(Context c) throws IOException {
        File f = new File(dir(c), "config.json");
        File t = new File(dir(c), "config.json.tmp");
        FileOutputStream o = new FileOutputStream(t);
        try { o.write(safeJson().toString(2).getBytes("UTF-8")); } catch (JSONException e) { throw new IOException(e); }
        finally { o.close(); }
        if (!t.renameTo(f)) throw new IOException("No se pudo guardar " + f);
    }

    static List<Vm> list(Context c) {
        List<Vm> r = new ArrayList<Vm>();
        File[] ds = root(c).listFiles();
        if (ds != null) for (File d : ds) {
            File f = new File(d, "config.json");
            if (!f.isFile()) continue;
            try {
                byte[] b = new byte[(int) f.length()];
                FileInputStream in = new FileInputStream(f);
                try { int o = 0; while (o < b.length) { int n = in.read(b, o, b.length - o); if (n < 0) break; o += n; } }
                finally { in.close(); }
                Vm v = fromJson(new JSONObject(new String(b, "UTF-8")));
                v.id = d.getName();
                r.add(v);
            } catch (Exception ignored) { }
        }
        Collections.sort(r, new Comparator<Vm>() {
            public int compare(Vm a, Vm b) { return a.created < b.created ? -1 : a.created == b.created ? 0 : 1; }
        });
        return r;
    }

    static Vm load(Context c, String id) {
        for (Vm v : list(c)) if (v.id.equals(id)) return v;
        return null;
    }

    static String newId() {
        return Long.toString(System.currentTimeMillis(), 36);
    }

    String summary() {
        String a = isArm() ? "ARM64" : "x86_64";
        return a + " · " + (ram >= 1024 && ram % 1024 == 0 ? (ram / 1024) + " GB" : ram + " MB") + " RAM · " + cpus + " CPU";
    }
}
