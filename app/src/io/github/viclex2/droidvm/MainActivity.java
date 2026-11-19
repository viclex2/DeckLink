package io.github.viclex2.droidvm;

import android.app.Activity;
import android.app.ProgressDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.view.Gravity;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/** Lista de máquinas virtuales (el «Administrador» de VirtualBox). */
public final class MainActivity extends Activity implements Runner.Listener {
    private LinearLayout list;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        setTitle("DroidVM");
        ScrollView sv = new ScrollView(this);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        int p = U.dp(this, 12);
        list.setPadding(p, p, p, p * 3);
        sv.addView(list);
        setContentView(sv);
        Runner.addListener(this);
        prepare();
        if (!getPreferences(0).getBoolean("storageAsked", false)) {
            getPreferences(0).edit().putBoolean("storageAsked", true).apply();
            Pick.askStorage(this);
        }
    }

    private void prepare() {
        if (!Bins.present(this) || Bins.shareReady(this)) return;
        final ProgressDialog pd = U.progress(this, "Preparando el firmware de QEMU (solo la primera vez)…");
        U.bg(new Runnable() { public void run() {
            try {
                Bins.ensureShare(MainActivity.this, new U.Cb<String>() { public void on(String f) { U.setProgress(pd, "Preparando " + f + "…"); } });
            } catch (Exception e) {
                U.alert(MainActivity.this, "Error", String.valueOf(e));
            }
            U.dismiss(pd);
        } });
    }

    @Override protected void onResume() { super.onResume(); refresh(); }

    @Override protected void onDestroy() { Runner.removeListener(this); super.onDestroy(); }

    public void changed() { refresh(); }

    private void refresh() {
        list.removeAllViews();
        List<Vm> vms = Vm.list(this);
        if (!Bins.present(this)) {
            TextView w = U.text(this, "⚠ Este APK no incluye QEMU para arm64-v8a. Las máquinas no podrán arrancar.", 14, 0xFFFFB74D);
            w.setPadding(0, 0, 0, U.dp(this, 12));
            list.addView(w);
        }
        if (vms.isEmpty()) {
            TextView t = U.text(this, "Aún no tienes máquinas virtuales.\n\nCrea una y elige la ISO de Windows 11 ARM o de cualquier Linux para instalarla.", 16, 0xFFCFD8DC);
            t.setGravity(Gravity.CENTER);
            t.setPadding(0, U.dp(this, 60), 0, U.dp(this, 24));
            list.addView(t);
            Button nb = U.button(this, "＋ Nueva máquina virtual", new View.OnClickListener() { public void onClick(View v) { newVm(); } });
            nb.setBackground(U.round(U.ACCENT, U.dp(this, 8)));
            nb.setTextColor(0xFF000000);
            list.addView(nb, new LinearLayout.LayoutParams(-1, U.dp(this, 52)));
            return;
        }
        for (Vm v : vms) list.addView(card(v));
    }

    private View card(final Vm v) {
        final Runner r = Runner.get(v.id);
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        int p = U.dp(this, 14);
        c.setPadding(p, p, p, U.dp(this, 6));
        c.setBackground(U.round(U.CARD, U.dp(this, 12)));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.bottomMargin = U.dp(this, 12);
        c.setLayoutParams(lp);

        String icon = v.isWin() ? "⊞ " : v.os.startsWith("linux") ? "🐧 " : "▣ ";
        c.addView(U.text(this, icon + v.name, 19, 0xFFFFFFFF));
        boolean saved = new File(v.dir(this), Snapshots.SAVED_TAG).isFile();
        String st = r == null ? (saved ? "Estado guardado" : "Apagada") : (r.paused ? "En pausa" : "En marcha");
        TextView s = U.text(this, "● " + st + "   " + v.summary(), 13, r != null ? 0xFF81C784 : saved ? 0xFFFFD54F : 0xFF90A4AE);
        s.setPadding(0, U.dp(this, 2), 0, 0);
        c.addView(s);
        String iso = v.iso.length() > 0 ? "💿 " + new File(v.iso).getName() : "💿 sin ISO";
        c.addView(U.text(this, iso, 12, 0xFF78909C));

        Button start = U.button(this, r == null ? "▶ Iniciar" : "🖥 Mostrar", new View.OnClickListener() {
            public void onClick(View x) { start(v); } });
        Button cfg = U.button(this, "⚙ Configurar", new View.OnClickListener() {
            public void onClick(View x) { startActivity(new Intent(MainActivity.this, EditActivity.class).putExtra("id", v.id)); } });
        Button more = U.button(this, "⋯ Más", new View.OnClickListener() {
            public void onClick(View x) { more(v); } });
        c.addView(U.row(this, start, cfg, more));
        return c;
    }

    private void start(final Vm v) {
        Runner r = Runner.get(v.id);
        if (r != null) { show(v); return; }
        if (v.iso.length() > 0 && !new File(v.iso).canRead()) {
            U.alert(this, "No se puede leer la ISO", v.iso + "\n\nDa permiso de almacenamiento a la app o vuelve a elegir la ISO en Configurar.");
            Pick.askStorage(this);
            return;
        }
        final ProgressDialog pd = U.progress(this, "Arrancando " + v.name + "…");
        U.bg(new Runnable() { public void run() {
            try {
                Runner.start(MainActivity.this, v);
                U.dismiss(pd);
                U.ui(new Runnable() { public void run() { show(v); } });
            } catch (Exception e) {
                U.dismiss(pd);
                U.alert(MainActivity.this, "No se pudo arrancar", String.valueOf(e.getMessage()));
            }
        } });
    }

    private void show(Vm v) {
        startActivity(new Intent(this, DisplayActivity.class).putExtra("id", v.id));
    }

    private void more(final Vm v) {
        final Runner r = Runner.get(v.id);
        final List<String> items = new ArrayList<String>();
        final List<Runnable> acts = new ArrayList<Runnable>();
        items.add("Instantáneas…"); acts.add(new Runnable() { public void run() { Snapshots.show(MainActivity.this, v, r); } });
        if (r != null) {
            items.add("Apagado ordenado (ACPI)"); acts.add(new Runnable() { public void run() {
                U.bg(new Runnable() { public void run() {
                    try { r.simple("system_powerdown"); U.toast(MainActivity.this, "Señal de apagado enviada"); }
                    catch (Exception e) { U.toast(MainActivity.this, "Error: " + e.getMessage()); } } }); } });
            items.add("Forzar apagado"); acts.add(new Runnable() { public void run() {
                U.confirm(MainActivity.this, "Forzar apagado", "Se perderá lo que no esté guardado dentro de la VM.", "Apagar",
                        new Runnable() { public void run() { r.kill(); } }); } });
            items.add("Registro de QEMU"); acts.add(new Runnable() { public void run() { DisplayActivity.showLog(MainActivity.this, r); } });
        } else {
            if (new File(v.dir(this), Snapshots.SAVED_TAG).isFile()) {
                items.add("Descartar el estado guardado"); acts.add(new Runnable() { public void run() {
                    new File(v.dir(MainActivity.this), Snapshots.SAVED_TAG).delete(); refresh(); } });
            }
            items.add("Clonar…"); acts.add(new Runnable() { public void run() { cloneVm(v); } });
            items.add("Ampliar disco…"); acts.add(new Runnable() { public void run() { resize(v); } });
            items.add("Información del disco"); acts.add(new Runnable() { public void run() {
                U.bg(new Runnable() { public void run() {
                    Bins.Result res = Bins.img(MainActivity.this, "info", v.disk);
                    U.alert(MainActivity.this, "Disco", res.out); } }); } });
            items.add("Eliminar"); acts.add(new Runnable() { public void run() { delete(v); } });
        }
        items.add("Ver comando de QEMU"); acts.add(new Runnable() { public void run() {
            try {
                List<String> cmd = QemuCmd.build(MainActivity.this, v, "<vnc>", "<qmp>");
                StringBuilder sb = new StringBuilder();
                for (String s : cmd) sb.append(s).append('\n');
                U.alert(MainActivity.this, "Comando", sb.toString());
            } catch (Exception e) { U.alert(MainActivity.this, "Error", String.valueOf(e.getMessage())); }
        } });
        U.choose(this, v.name, items.toArray(new String[0]), new U.Cb<Integer>() { public void on(Integer i) { acts.get(i).run(); } });
    }

    private void cloneVm(final Vm v) {
        U.choose(this, "Clonar " + v.name, new String[]{"Clon completo (copia todo el disco)", "Clon enlazado (rápido, depende del original)"},
                new U.Cb<Integer>() { public void on(final Integer kind) {
            final ProgressDialog pd = U.progress(MainActivity.this, "Clonando…");
            U.bg(new Runnable() { public void run() {
                Vm n = v.copy();
                n.id = Vm.newId();
                n.created = System.currentTimeMillis();
                n.name = v.name + (kind == 0 ? " (copia)" : " (enlazado)");
                try {
                    File d = n.dir(MainActivity.this);
                    File vars = QemuCmd.varsFile(MainActivity.this, v);
                    if (vars.isFile()) U.copyFile(vars, QemuCmd.varsFile(MainActivity.this, n), null);
                    if (v.disk.length() > 0) {
                        File dst = new File(d, "disco.qcow2");
                        if (kind == 0) {
                            final File src = new File(v.disk);
                            if ("qcow2".equals(QemuCmd.fmt(v.disk))) U.copyFile(src, dst, new U.Progress() {
                                public void on(long done, long t) { U.setProgress(pd, "Copiando disco… " + U.human(done) + " de " + U.human(t)); } });
                            else {
                                Bins.Result r = Bins.img(MainActivity.this, "convert", "-O", "qcow2", v.disk, dst.getAbsolutePath());
                                if (r.code != 0) throw new Exception(r.out);
                            }
                        } else {
                            Bins.Result r = Bins.img(MainActivity.this, "create", "-f", "qcow2", "-b", v.disk, "-F", QemuCmd.fmt(v.disk), dst.getAbsolutePath());
                            if (r.code != 0) throw new Exception(r.out);
                        }
                        n.disk = dst.getAbsolutePath();
                    }
                    n.save(MainActivity.this);
                    U.dismiss(pd);
                    U.ui(new Runnable() { public void run() { refresh(); } });
                } catch (Exception e) {
                    U.dismiss(pd);
                    U.rmrf(new File(Vm.root(MainActivity.this), n.id));
                    U.alert(MainActivity.this, "Error al clonar", String.valueOf(e.getMessage()));
                }
            } });
        } });
    }

    private void resize(final Vm v) {
        U.input(this, "Ampliar disco", "GB que añadir", "16", true, new U.Cb<String>() { public void on(final String s) {
            U.bg(new Runnable() { public void run() {
                Bins.Result r = Bins.img(MainActivity.this, "resize", v.disk, "+" + s + "G");
                if (r.code == 0) U.alert(MainActivity.this, "Disco ampliado", "Amplía también la partición dentro del sistema invitado (Administración de discos en Windows, growpart/GParted en Linux).");
                else U.alert(MainActivity.this, "Error", r.out);
            } });
        } });
    }

    private void delete(final Vm v) {
        U.confirm(this, "Eliminar " + v.name, "Se borrará la máquina y sus discos guardados en la app. Las ISO no se tocan.", "Eliminar",
                new Runnable() { public void run() {
                    U.rmrf(new File(Vm.root(MainActivity.this), v.id));
                    U.rmrf(new File(new File(getFilesDir(), "run"), v.id));
                    refresh();
                } });
    }

    private void newVm() { startActivity(new Intent(this, EditActivity.class)); }

    @Override public boolean onCreateOptionsMenu(Menu m) {
        m.add(0, 1, 0, "Nueva").setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
        m.add(0, 2, 0, "Permitir funcionar en segundo plano");
        m.add(0, 3, 0, "Permiso de almacenamiento");
        m.add(0, 4, 0, "Ayuda");
        return true;
    }

    @Override public boolean onOptionsItemSelected(MenuItem i) {
        switch (i.getItemId()) {
            case 1: newVm(); return true;
            case 2:
                try {
                    PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
                    if (Build.VERSION.SDK_INT >= 23 && !pm.isIgnoringBatteryOptimizations(getPackageName()))
                        startActivity(new Intent("android.settings.REQUEST_IGNORE_BATTERY_OPTIMIZATIONS", Uri.parse("package:" + getPackageName())));
                    else U.toast(this, "Ya está permitido");
                } catch (Exception e) { U.toast(this, String.valueOf(e)); }
                return true;
            case 3: Pick.askStorage(this); if (Pick.hasStorage(this)) U.toast(this, "Ya tiene permiso"); return true;
            case 4: help(); return true;
        }
        return super.onOptionsItemSelected(i);
    }

    private void help() {
        U.alert(this, "DroidVM " + BuildInfo.VERSION,
                "Máquinas virtuales con QEMU " + "dentro del móvil.\n\n"
                + "• Windows 11 ARM: descarga la ISO ARM64 oficial de Microsoft, crea una máquina «Windows 11 ARM64» y elígela. "
                + "Si el instalador pide pulsar una tecla para arrancar desde el CD, toca la pantalla o usa Teclas → Esc/Enter. "
                + "Los requisitos (TPM, Secure Boot) se saltan solos. Sin red durante la instalación puedes crear una cuenta local.\n\n"
                + "• Linux: cualquier ISO ARM64 (aarch64) va bastante mejor que una x86_64, que se emula entera.\n\n"
                + "• Sin KVM todo se emula (TCG): Windows tarda mucho en instalarse (horas) y va lento. Deja el móvil cargando y con la app en primer plano o con el permiso de segundo plano.\n\n"
                + "• Ratón: «Táctil» = tocas donde quieres hacer clic (mantener = clic derecho). «Touchpad» = mueves el puntero como en un portátil "
                + "(mantener y arrastrar = arrastrar). Dos dedos: desplazar; toque con dos dedos = clic derecho.\n\n"
                + "• Instantáneas, guardar estado, clonar, carpeta compartida, reenvío de puertos y cambiar de ISO en marcha, como en VirtualBox.\n\n"
                + "Los discos están en Android/data/" + getPackageName() + "/files/vms (se borran si desinstalas la app).");
    }
}
