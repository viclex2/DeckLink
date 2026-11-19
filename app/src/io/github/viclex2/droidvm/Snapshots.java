package io.github.viclex2.droidvm;

import android.app.Activity;
import android.app.ProgressDialog;

import java.io.File;
import java.io.FileOutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/** Instantáneas (snapshots) del disco qcow2, con la VM en marcha (savevm) o apagada (qemu-img snapshot). */
final class Snapshots {
    static final String SAVED_TAG = "estado-guardado";

    static List<String> parse(String out) {
        List<String> r = new ArrayList<String>();
        for (String l : out.split("\n")) {
            String[] p = l.trim().split("\\s+");
            if (p.length >= 3 && (p[0].matches("\\d+") || p[0].equals("--")) && !r.contains(p[1])) r.add(p[1]);
        }
        return r;
    }

    static void show(final Activity a, final Vm vm, final Runner run) {
        if (!"qcow2".equals(QemuCmd.fmt(vm.disk))) {
            U.alert(a, "Instantáneas", "Las instantáneas necesitan un disco qcow2 (los discos que crea la app lo son).");
            return;
        }
        final ProgressDialog pd = U.progress(a, "Leyendo instantáneas…");
        U.bg(new Runnable() { public void run() {
            String out;
            try {
                out = run != null && run.alive ? run.hmp("info snapshots", 20000) : Bins.img(a, "snapshot", "-l", vm.disk).out;
            } catch (Exception e) { out = ""; }
            final List<String> tags = parse(out);
            U.dismiss(pd);
            U.ui(new Runnable() { public void run() { menu(a, vm, run, tags); } });
        } });
    }

    private static void menu(final Activity a, final Vm vm, final Runner run, final List<String> tags) {
        final boolean live = run != null && run.alive;
        String[] items = new String[tags.size() + 1];
        items[0] = "＋ Tomar instantánea" + (live ? " (estado completo en marcha)" : " (del disco)");
        for (int i = 0; i < tags.size(); i++) items[i + 1] = "◷ " + tags.get(i);
        U.choose(a, "Instantáneas de " + vm.name, items, new U.Cb<Integer>() { public void on(Integer w) {
            if (w == 0) {
                String def = new SimpleDateFormat("yyyy-MM-dd_HH.mm", Locale.US).format(new Date());
                U.input(a, "Nueva instantánea", "Nombre (sin espacios)", def, false, new U.Cb<String>() { public void on(String n) {
                    final String tag = n.replaceAll("\\s+", "_");
                    if (tag.length() == 0) return;
                    op(a, live ? "Guardando el estado (puede tardar con mucha RAM)…" : "Creando instantánea…", run, vm,
                            live ? "savevm " + tag : null, new String[]{"snapshot", "-c", tag, vm.disk});
                } });
            } else {
                final String tag = tags.get(w - 1);
                U.choose(a, tag, new String[]{"Restaurar", "Borrar"}, new U.Cb<Integer>() { public void on(Integer x) {
                    if (x == 0) U.confirm(a, "Restaurar " + tag, "La máquina volverá exactamente a ese momento. Se pierde lo posterior que no esté en otra instantánea.",
                            "Restaurar", new Runnable() { public void run() {
                                op(a, "Restaurando…", run, vm, live ? "loadvm " + tag : null, new String[]{"snapshot", "-a", tag, vm.disk});
                            } });
                    else U.confirm(a, "Borrar " + tag, "¿Borrar esta instantánea?", "Borrar", new Runnable() { public void run() {
                        op(a, "Borrando…", run, vm, live ? "delvm " + tag : null, new String[]{"snapshot", "-d", tag, vm.disk});
                    } });
                } });
            }
        } });
    }

    private static void op(final Activity a, String msg, final Runner run, final Vm vm, final String hmp, final String[] img) {
        final ProgressDialog pd = U.progress(a, msg);
        U.bg(new Runnable() { public void run() {
            String err = null;
            try {
                if (hmp != null) {
                    String out = run.hmp(hmp, 30 * 60 * 1000);
                    if (out.toLowerCase(Locale.ROOT).contains("error")) err = out;
                } else {
                    Bins.Result r = Bins.img(a, img);
                    if (r.code != 0) err = r.out;
                    // también la NVRAM UEFI (qcow2), para que las instantáneas queden coherentes
                    File vars = QemuCmd.varsFile(a, vm);
                    if (err == null && vars.isFile()) {
                        String[] v2 = img.clone();
                        v2[v2.length - 1] = vars.getAbsolutePath();
                        Bins.img(a, v2);
                    }
                }
            } catch (Exception e) { err = String.valueOf(e.getMessage()); }
            U.dismiss(pd);
            if (err != null && err.contains("non-migratable"))
                err = "Con esta configuración no se puede guardar el estado con la máquina en marcha (los discos NVMe y la red USB no lo permiten).\n\n"
                        + "Apaga la máquina y toma la instantánea en frío, o cambia el disco y la red a VirtIO.\n\n" + err;
            if (err != null) U.alert(a, "Error", err);
            else U.toast(a, "Hecho");
        } });
    }

    /** «Guardar estado y apagar» (como VirtualBox): savevm y quit; al volver a arrancar se restaura solo. */
    static void saveAndQuit(final Activity a, final Runner run) {
        if (!"qcow2".equals(QemuCmd.fmt(run.vm.disk))) {
            U.alert(a, "No disponible", "Guardar el estado necesita un disco qcow2.");
            return;
        }
        final ProgressDialog pd = U.progress(a, "Guardando el estado de la máquina…");
        U.bg(new Runnable() { public void run() {
            try {
                String out = run.hmp("savevm " + SAVED_TAG, 30 * 60 * 1000);
                if (out.contains("non-migratable")) throw new Exception("Con disco NVMe o red USB no se puede guardar el estado en marcha. Usa VirtIO o apaga normalmente.\n\n" + out);
                if (out.toLowerCase(Locale.ROOT).contains("error")) throw new Exception(out);
                FileOutputStream o = new FileOutputStream(new File(run.vm.dir(a), SAVED_TAG));
                o.write(SAVED_TAG.getBytes());
                o.close();
                try { run.simple("quit"); } catch (Exception ignored) { }
                U.dismiss(pd);
                U.toast(a, "Estado guardado. Al arrancarla seguirá donde estaba.");
            } catch (Exception e) {
                U.dismiss(pd);
                U.alert(a, "No se pudo guardar el estado", String.valueOf(e.getMessage()));
            }
        } });
    }
}
