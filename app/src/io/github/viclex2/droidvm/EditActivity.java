package io.github.viclex2.droidvm;

import android.app.Activity;
import android.app.ProgressDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.AdapterView;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.File;

/** Asistente de creación y ajustes de una VM (como el «Configuración» de VirtualBox). */
public final class EditActivity extends Activity {
    static final String[] ARCH = {"aarch64", "x86_64"};
    static final String[] ARCH_L = {"ARM64 (aarch64) — Windows 11 ARM, Linux ARM", "x86_64 (PC) — emulado, muy lento"};
    static final String[] DISKIF = {"nvme", "virtio", "scsi", "usb", "sata"};
    static final String[] DISKIF_L = {"NVMe (Windows ARM)", "VirtIO (Linux, el más rápido)", "SCSI VirtIO", "USB", "SATA/IDE (solo x86)"};
    static final String[] CDIF = {"usb", "scsi", "sata"};
    static final String[] CDIF_L = {"USB (Windows ARM)", "SCSI VirtIO (Linux)", "SATA/IDE (solo x86)"};
    static final String[] DISP = {"ramfb", "virtio-gpu", "std", "ramfb+virtio"};
    static final String[] DISP_L = {"ramfb (Windows ARM, UEFI)", "virtio-gpu (Linux)", "VGA estándar (x86)", "ramfb + virtio-gpu"};
    static final String[] RES = {"800x600", "1024x768", "1280x720", "1280x800", "1366x768", "1600x900", "1920x1080", "2400x1080"};
    static final String[] NET = {"user", "none"};
    static final String[] NET_L = {"NAT (Internet a través del móvil)", "Sin red"};
    static final String[] NIC = {"virtio", "e1000", "e1000e", "usb-net", "rtl8139"};
    static final String[] NIC_L = {"VirtIO (Linux)", "Intel e1000", "Intel e1000e", "USB RNDIS (Windows ARM)", "Realtek 8139"};
    static final String[] KEYMAP = {"es", "en-us", "en-gb", "fr", "de", "it", "pt", "pt-br", "ninguno"};
    static final String[] ACCEL = {"auto", "tcg", "kvm"};
    static final String[] ACCEL_L = {"Automática (KVM si existe, si no TCG)", "TCG (emulación, funciona siempre)", "KVM (solo si el móvil lo permite)"};

    private Vm v;
    private boolean isNew;
    private Spinner osSp, archSp, cpusSp, diskIfSp, cdIfSp, dispSp, resSp, netSp, nicSp, keySp, accelSp;
    private EditText name, ram, diskSize, fwd, machine, cpu, extra;
    private CheckBox bootCd, multi, uefi, bypass, rtc, shareRw, usbKbd;
    private TextView isoL, diskL, disk2L, shareL;
    private String existingDisk = "";
    private Uri pendingUri;
    private int pendingReq;
    private boolean filling;
    private int cores;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        cores = Runtime.getRuntime().availableProcessors();
        String id = getIntent().getStringExtra("id");
        if (id != null) v = Vm.load(this, id);
        isNew = v == null;
        if (isNew) v = Vm.preset("win11-arm", cores);
        setTitle(isNew ? "Nueva máquina virtual" : "Configuración: " + v.name);
        build();
        fill();
    }

    private void build() {
        ScrollView sv = new ScrollView(this);
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        int p = U.dp(this, 16);
        l.setPadding(p, p / 2, p, p * 2);
        sv.addView(l);

        if (isNew) {
            l.addView(U.section(this, "Sistema operativo"));
            osSp = U.spinner(this, Vm.OS_NAMES, 0);
            osSp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                public void onItemSelected(AdapterView<?> a, View vw, int pos, long i) {
                    if (filling) return;
                    String keepName = name.getText().toString();
                    boolean defName = false;
                    for (String os : Vm.OS_IDS) if (keepName.equals(Vm.preset(os, cores).name)) defName = true;
                    v = Vm.preset(Vm.OS_IDS[pos], cores);
                    if (!defName && keepName.length() > 0) v.name = keepName;
                    fill();
                }
                public void onNothingSelected(AdapterView<?> a) { }
            });
            l.addView(osSp);
        }

        l.addView(U.section(this, "General"));
        name = U.field(this, "", false);
        l.addView(U.labeled(this, "Nombre", name));
        archSp = U.spinner(this, ARCH_L, 0);
        l.addView(U.labeled(this, "Arquitectura", archSp));

        l.addView(U.section(this, "Sistema"));
        ram = U.field(this, "", true);
        long mem = 0;
        try {
            android.app.ActivityManager.MemoryInfo mi = new android.app.ActivityManager.MemoryInfo();
            ((android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE)).getMemoryInfo(mi);
            mem = mi.totalMem;
        } catch (Exception ignored) { }
        l.addView(U.labeled(this, "Memoria RAM en MB (el móvil tiene " + U.human(mem) + ")", ram));
        String[] cp = new String[Math.max(8, cores)];
        for (int i = 0; i < cp.length; i++) cp[i] = String.valueOf(i + 1);
        cpusSp = U.spinner(this, cp, 0);
        l.addView(U.labeled(this, "Procesadores (el móvil tiene " + cores + " núcleos)", cpusSp));
        accelSp = U.spinner(this, ACCEL_L, 0);
        l.addView(U.labeled(this, "Aceleración", accelSp));
        multi = U.check(this, "TCG multihilo (usa varios núcleos del móvil)", true);
        l.addView(multi);
        uefi = U.check(this, "Firmware UEFI (en ARM64 siempre)", true);
        l.addView(uefi);
        rtc = U.check(this, "Reloj en hora local (Windows)", false);
        l.addView(rtc);

        l.addView(U.section(this, "Almacenamiento"));
        if (isNew) {
            diskSize = U.field(this, "64", true);
            l.addView(U.labeled(this, "Tamaño del disco nuevo (GB, crece según se usa)", diskSize));
            diskL = U.text(this, "", 13, 0xFFB0BEC5);
            l.addView(diskL);
            l.addView(U.row(this,
                    U.button(this, "Usar disco existente…", new View.OnClickListener() { public void onClick(View x) { Pick.file(EditActivity.this, 2); } }),
                    U.button(this, "Disco nuevo", new View.OnClickListener() { public void onClick(View x) { existingDisk = ""; showDisk(); } })));
        } else {
            diskL = U.text(this, "", 13, 0xFFB0BEC5);
            l.addView(U.labeled(this, "Disco principal", diskL));
        }
        diskIfSp = U.spinner(this, DISKIF_L, 0);
        l.addView(U.labeled(this, "Controlador del disco", diskIfSp));
        disk2L = U.text(this, "", 13, 0xFFB0BEC5);
        l.addView(U.labeled(this, "Segundo disco (opcional)", disk2L));
        l.addView(U.row(this,
                U.button(this, "Elegir…", new View.OnClickListener() { public void onClick(View x) { Pick.file(EditActivity.this, 4); } }),
                U.button(this, "Crear…", new View.OnClickListener() { public void onClick(View x) { createDisk2(); } }),
                U.button(this, "Quitar", new View.OnClickListener() { public void onClick(View x) { v.disk2 = ""; showDisk(); } })));

        l.addView(U.section(this, "Óptico (ISO)"));
        isoL = U.text(this, "", 13, 0xFFB0BEC5);
        l.addView(isoL);
        l.addView(U.row(this,
                U.button(this, "Elegir ISO…", new View.OnClickListener() { public void onClick(View x) { Pick.file(EditActivity.this, 1); } }),
                U.button(this, "Expulsar", new View.OnClickListener() { public void onClick(View x) { v.iso = ""; showDisk(); } })));
        cdIfSp = U.spinner(this, CDIF_L, 0);
        l.addView(U.labeled(this, "Controlador del CD", cdIfSp));
        bootCd = U.check(this, "Arrancar primero desde el CD (para instalar)", true);
        l.addView(bootCd);

        l.addView(U.section(this, "Pantalla"));
        dispSp = U.spinner(this, DISP_L, 0);
        l.addView(U.labeled(this, "Tarjeta gráfica", dispSp));
        resSp = U.spinner(this, RES, 3);
        l.addView(U.labeled(this, "Resolución (virtio-gpu y VGA)", resSp));
        l.addView(U.text(this, "Con ramfb la resolución es 1024x768 (se puede cambiar en la UEFI: Esc al arrancar → Device Manager → OVMF Platform Configuration).", 12, 0xFF78909C));

        l.addView(U.section(this, "Red"));
        netSp = U.spinner(this, NET_L, 0);
        l.addView(U.labeled(this, "Conectado a", netSp));
        nicSp = U.spinner(this, NIC_L, 0);
        l.addView(U.labeled(this, "Tarjeta de red", nicSp));
        fwd = U.field(this, "", false);
        fwd.setHint("p. ej. 2222:22, 3389:3389");
        l.addView(U.labeled(this, "Reenvío de puertos (móvil:VM)", fwd));

        l.addView(U.section(this, "Carpeta compartida"));
        shareL = U.text(this, "", 13, 0xFFB0BEC5);
        l.addView(shareL);
        l.addView(U.row(this,
                U.button(this, "Elegir carpeta…", new View.OnClickListener() { public void onClick(View x) { Pick.folder(EditActivity.this, 3); } }),
                U.button(this, "Quitar", new View.OnClickListener() { public void onClick(View x) { v.share = ""; showDisk(); } })));
        shareRw = U.check(this, "Permitir escribir (experimental)", false);
        l.addView(shareRw);
        l.addView(U.text(this, "Aparece en la VM como una memoria USB (FAT, máx. ~500 MB).", 12, 0xFF78909C));

        l.addView(U.section(this, "Entrada"));
        keySp = U.spinner(this, KEYMAP, 0);
        l.addView(U.labeled(this, "Distribución de teclado de la VM", keySp));
        usbKbd = U.check(this, "Teclado USB", true);
        l.addView(usbKbd);

        l.addView(U.section(this, "Windows"));
        bypass = U.check(this, "Saltar requisitos de Windows 11 (TPM, Secure Boot, RAM, CPU) y la cuenta de Microsoft", false);
        l.addView(bypass);

        l.addView(U.section(this, "Avanzado"));
        machine = U.field(this, "", false);
        l.addView(U.labeled(this, "Máquina (-M)", machine));
        cpu = U.field(this, "", false);
        l.addView(U.labeled(this, "Modelo de CPU (-cpu)", cpu));
        extra = U.field(this, "", false);
        extra.setSingleLine(false);
        l.addView(U.labeled(this, "Argumentos extra de QEMU", extra));

        Button save = U.button(this, isNew ? "Crear máquina virtual" : "Guardar", new View.OnClickListener() {
            public void onClick(View x) { save(); } });
        save.setBackground(U.round(U.ACCENT, U.dp(this, 8)));
        save.setTextColor(0xFF000000);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, U.dp(this, 52));
        lp.topMargin = U.dp(this, 24);
        l.addView(save, lp);
        setContentView(sv);
    }

    private void fill() {
        filling = true;
        if (osSp != null) osSp.setSelection(U.indexOf(Vm.OS_IDS, v.os));
        name.setText(v.name);
        archSp.setSelection(U.indexOf(ARCH, v.arch));
        ram.setText(String.valueOf(v.ram));
        cpusSp.setSelection(Math.max(0, Math.min(cpusSp.getCount() - 1, v.cpus - 1)));
        accelSp.setSelection(U.indexOf(ACCEL, v.accel));
        multi.setChecked(v.tcgMulti);
        uefi.setChecked(v.uefi);
        rtc.setChecked(v.rtcLocal);
        if (diskSize != null) diskSize.setText(v.isWin() ? "64" : "32");
        diskIfSp.setSelection(U.indexOf(DISKIF, v.diskIf));
        cdIfSp.setSelection(U.indexOf(CDIF, v.cdIf));
        bootCd.setChecked(v.bootCd);
        dispSp.setSelection(U.indexOf(DISP, v.display));
        resSp.setSelection(U.indexOf(RES, v.res));
        netSp.setSelection(U.indexOf(NET, v.net));
        nicSp.setSelection(U.indexOf(NIC, v.nic));
        fwd.setText(v.fwd);
        shareRw.setChecked(v.shareRw);
        keySp.setSelection(U.indexOf(KEYMAP, v.keymap));
        usbKbd.setChecked(v.usbKbd);
        bypass.setChecked(v.winBypass);
        machine.setText(v.machine);
        cpu.setText(v.cpu);
        extra.setText(v.extra);
        showDisk();
        U.MAIN.post(new Runnable() { public void run() { filling = false; } });
    }

    private void showDisk() {
        isoL.setText(v.iso.length() > 0 ? v.iso : "Vacío");
        if (isNew) diskL.setText(existingDisk.length() > 0 ? "Disco existente: " + existingDisk : "Se creará un disco nuevo (qcow2)");
        else diskL.setText(v.disk.length() > 0 ? v.disk : "Sin disco");
        if (diskSize != null) diskSize.setEnabled(existingDisk.length() == 0);
        disk2L.setText(v.disk2.length() > 0 ? v.disk2 : "Ninguno");
        shareL.setText(v.share.length() > 0 ? v.share : "Ninguna");
    }

    private void read() {
        v.name = name.getText().toString().trim();
        if (v.name.length() == 0) v.name = "Máquina";
        v.arch = ARCH[archSp.getSelectedItemPosition()];
        try { v.ram = Integer.parseInt(ram.getText().toString().trim()); } catch (Exception e) { v.ram = 2048; }
        v.ram = Math.max(128, v.ram);
        v.cpus = cpusSp.getSelectedItemPosition() + 1;
        v.accel = ACCEL[accelSp.getSelectedItemPosition()];
        v.tcgMulti = multi.isChecked();
        v.uefi = uefi.isChecked();
        v.rtcLocal = rtc.isChecked();
        v.diskIf = DISKIF[diskIfSp.getSelectedItemPosition()];
        v.cdIf = CDIF[cdIfSp.getSelectedItemPosition()];
        v.bootCd = bootCd.isChecked();
        v.display = DISP[dispSp.getSelectedItemPosition()];
        v.res = RES[resSp.getSelectedItemPosition()];
        v.net = NET[netSp.getSelectedItemPosition()];
        v.nic = NIC[nicSp.getSelectedItemPosition()];
        v.fwd = fwd.getText().toString().trim();
        v.shareRw = shareRw.isChecked();
        v.keymap = KEYMAP[keySp.getSelectedItemPosition()];
        v.usbKbd = usbKbd.isChecked();
        v.winBypass = bypass.isChecked();
        v.machine = machine.getText().toString().trim();
        v.cpu = cpu.getText().toString().trim();
        v.extra = extra.getText().toString().trim();
        if (v.isArm()) {
            if (v.diskIf.equals("sata")) v.diskIf = "virtio";
            if (v.cdIf.equals("sata")) v.cdIf = "usb";
            if (v.machine.length() == 0 || v.machine.startsWith("q35") || v.machine.startsWith("pc")) v.machine = "virt";
        } else {
            if (v.machine.length() == 0 || v.machine.startsWith("virt")) v.machine = "q35";
        }
        if (v.cpu.length() == 0) v.cpu = "max";
    }

    private void createDisk2() {
        U.input(this, "Crear segundo disco", "Tamaño en GB", "16", true, new U.Cb<String>() { public void on(final String s) {
            if (isNew) { U.toast(EditActivity.this, "Crea primero la máquina y luego añade el segundo disco"); return; }
            final ProgressDialog pd = U.progress(EditActivity.this, "Creando disco…");
            U.bg(new Runnable() { public void run() {
                final File f = new File(v.dir(EditActivity.this), "disco2-" + System.currentTimeMillis() % 100000 + ".qcow2");
                final Bins.Result r = Bins.img(EditActivity.this, "create", "-f", "qcow2", f.getAbsolutePath(), s + "G");
                U.dismiss(pd);
                U.ui(new Runnable() { public void run() {
                    if (r.code == 0) { v.disk2 = f.getAbsolutePath(); showDisk(); }
                    else U.alert(EditActivity.this, "Error", r.out);
                } });
            } });
        } });
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        Uri u = data.getData();
        if (req == 3) {
            String p = Pick.path(this, u, true);
            if (p == null) { U.alert(this, "Carpeta no válida", "Elige una carpeta del almacenamiento interno o de la tarjeta SD."); return; }
            if (!Pick.hasStorage(this)) Pick.askStorage(this);
            v.share = p;
            showDisk();
            return;
        }
        String p = Pick.path(this, u, false);
        if (p != null && !new File(p).canRead() && !Pick.hasStorage(this)) {
            pendingUri = u; pendingReq = req;
            Pick.askStorage(this);
            return;
        }
        resolve(req, u);
    }

    @Override public void onRequestPermissionsResult(int code, String[] perms, int[] grants) {
        if (pendingUri != null) { Uri u = pendingUri; pendingUri = null; resolve(pendingReq, u); }
    }

    private void resolve(final int req, Uri u) {
        File base = getExternalFilesDir(null);
        File dir = new File(base != null ? base : getFilesDir(), req == 1 ? "isos" : "discos");
        Pick.resolve(this, u, dir, new U.Cb<String>() { public void on(String path) {
            if (req == 1) { v.iso = path; bootCd.setChecked(true); }
            else if (req == 2) existingDisk = path;
            else if (req == 4) v.disk2 = path;
            showDisk();
        } });
    }

    private void save() {
        read();
        if (v.isWin() && v.ram < 2048) U.toast(this, "Aviso: Windows necesita al menos 2048 MB");
        if (!isNew) {
            if (Runner.get(v.id) != null) U.toast(this, "Los cambios se aplicarán la próxima vez que arranques la máquina");
            try { v.save(this); finish(); } catch (Exception e) { U.alert(this, "Error", String.valueOf(e)); }
            return;
        }
        int gb = 32;
        try { gb = Integer.parseInt(diskSize.getText().toString().trim()); } catch (Exception ignored) { }
        final int size = Math.max(1, gb);
        v.id = Vm.newId();
        v.created = System.currentTimeMillis();
        final ProgressDialog pd = U.progress(this, "Creando la máquina…");
        U.bg(new Runnable() { public void run() {
            try {
                File d = v.dir(EditActivity.this);
                if (existingDisk.length() > 0) v.disk = existingDisk;
                else {
                    File f = new File(d, "disco.qcow2");
                    Bins.Result r = Bins.img(EditActivity.this, "create", "-f", "qcow2", "-o", "lazy_refcounts=on",
                            f.getAbsolutePath(), size + "G");
                    if (r.code != 0) throw new Exception("qemu-img: " + r.out);
                    v.disk = f.getAbsolutePath();
                }
                v.save(EditActivity.this);
                U.dismiss(pd);
                U.ui(new Runnable() { public void run() { setResult(RESULT_OK); finish(); } });
            } catch (final Exception e) {
                U.dismiss(pd);
                U.rmrf(new File(Vm.root(EditActivity.this), v.id));
                U.alert(EditActivity.this, "No se pudo crear", String.valueOf(e.getMessage()));
            }
        } });
    }
}
