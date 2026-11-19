package io.github.viclex2.droidvm;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/** Construye la línea de órdenes de QEMU a partir de la configuración de la VM. */
final class QemuCmd {

    static String fmt(String path) {
        String p = path.toLowerCase(java.util.Locale.ROOT);
        if (p.endsWith(".qcow2") || p.endsWith(".qcow")) return "qcow2";
        if (p.endsWith(".vdi")) return "vdi";
        if (p.endsWith(".vmdk")) return "vmdk";
        if (p.endsWith(".vhdx")) return "vhdx";
        if (p.endsWith(".vhd") || p.endsWith(".vpc")) return "vpc";
        return "raw";
    }

    static File varsFile(Context c, Vm v) { return new File(v.dir(c), v.isArm() ? "efivars-arm.qcow2" : "efivars-x86.qcow2"); }

    /** Copia la plantilla de variables UEFI si la VM aún no tiene la suya. */
    static void ensureVars(Context c, Vm v) throws IOException {
        File f = varsFile(c, v);
        if (f.isFile()) return;
        // en ARM se usa una NVRAM ya configurada a 1024x768 (lo máximo de ramfb) en vez de 800x600
        File tpl = new File(Bins.share(c), v.isArm() ? "edk2-arm-vars-1024.fd" : "edk2-i386-vars.fd");
        if (!tpl.isFile() && v.isArm()) tpl = new File(Bins.share(c), "edk2-arm-vars.fd");
        if (!tpl.isFile()) throw new IOException("Falta el firmware " + tpl.getName());
        // en qcow2 para que las instantáneas en marcha (savevm) también guarden la NVRAM
        Bins.Result r = Bins.img(c, "convert", "-O", "qcow2", tpl.getAbsolutePath(), f.getAbsolutePath());
        if (r.code != 0) throw new IOException("qemu-img: " + r.out);
    }

    static File unattendDir(Context c, Vm v) throws IOException {
        File d = new File(v.dir(c), "unattend");
        d.mkdirs();
        FileOutputStream o = new FileOutputStream(new File(d, "autounattend.xml"));
        o.write(unattendXml().getBytes("UTF-8"));
        o.close();
        return d;
    }

    private static String reg(int order, String key, String name) {
        return "        <RunSynchronousCommand wcm:action=\"add\"><Order>" + order + "</Order><Path>reg add " + key
                + " /v " + name + " /t REG_DWORD /d 1 /f</Path></RunSynchronousCommand>\n";
    }

    private static String component(String name, String arch, String body) {
        return "    <component name=\"" + name + "\" processorArchitecture=\"" + arch + "\" publicKeyToken=\"31bf3856ad364e35\""
                + " language=\"neutral\" versionScope=\"nonSxS\" xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n"
                + body + "    </component>\n";
    }

    /** Solo salta las comprobaciones (TPM, Secure Boot, RAM, CPU) y la cuenta de Microsoft; el resto de la instalación es normal. */
    static String unattendXml() {
        String lab = "HKLM\\SYSTEM\\Setup\\LabConfig";
        String pe = "      <RunSynchronous>\n" + reg(1, lab, "BypassTPMCheck") + reg(2, lab, "BypassSecureBootCheck")
                + reg(3, lab, "BypassRAMCheck") + reg(4, lab, "BypassCPUCheck") + reg(5, lab, "BypassStorageCheck")
                + "      </RunSynchronous>\n";
        String sp = "      <RunSynchronous>\n" + reg(1, "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE", "BypassNRO")
                + "      </RunSynchronous>\n";
        StringBuilder s = new StringBuilder();
        s.append("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n");
        s.append("  <settings pass=\"windowsPE\">\n");
        s.append(component("Microsoft-Windows-Setup", "arm64", pe));
        s.append(component("Microsoft-Windows-Setup", "amd64", pe));
        s.append("  </settings>\n  <settings pass=\"specialize\">\n");
        s.append(component("Microsoft-Windows-Deployment", "arm64", sp));
        s.append(component("Microsoft-Windows-Deployment", "amd64", sp));
        s.append("  </settings>\n</unattend>\n");
        return s.toString();
    }

    static String drive(String file, String id, String extra) {
        StringBuilder s = new StringBuilder();
        if (file != null && file.length() > 0) s.append("file=").append(file.replace(",", ",,")).append(',');
        s.append("if=none,id=").append(id);
        if (extra != null && extra.length() > 0) s.append(',').append(extra);
        return s.toString();
    }

    static List<String> build(Context c, Vm v, String vncSock, String qmpSock) throws IOException {
        List<String> a = new ArrayList<String>();
        boolean arm = v.isArm();
        String share = Bins.share(c).getAbsolutePath();
        a.add(Bins.qemu(c, v.arch).getAbsolutePath());
        a.add("-L"); a.add(share);
        a.add("-name"); a.add(v.name);
        a.add("-nodefaults");

        boolean kvm = arm && !"tcg".equals(v.accel) && Bins.kvmUsable();
        if ("kvm".equals(v.accel) && !kvm) throw new IOException("KVM no está disponible en este dispositivo (/dev/kvm)");

        String machine = v.machine.length() > 0 ? v.machine : (arm ? "virt" : "q35");
        if (arm && machine.equals("virt") && !kvm) machine = "virt,gic-version=3";
        a.add("-M"); a.add(machine);
        if (kvm) {
            a.add("-accel"); a.add("kvm");
            a.add("-cpu"); a.add("host");
        } else {
            a.add("-accel"); a.add("tcg,thread=" + (v.tcgMulti ? "multi" : "single") + ",tb-size=256");
            String cpu = v.cpu.length() > 0 ? v.cpu : "max";
            if (arm && cpu.equals("max")) cpu = "max,pauth-impdef=on";
            a.add("-cpu"); a.add(cpu);
        }
        a.add("-smp"); a.add(String.valueOf(v.cpus));
        a.add("-m"); a.add(v.ram + "M");

        if (arm || v.uefi) {
            ensureVars(c, v);
            File code = new File(share, arm ? "edk2-aarch64-code.fd" : "edk2-x86_64-code.fd");
            a.add("-drive"); a.add("if=pflash,format=raw,unit=0,readonly=on,file=" + code.getAbsolutePath());
            a.add("-drive"); a.add("if=pflash,format=qcow2,unit=1,file.locking=off,file=" + varsFile(c, v).getAbsolutePath());
        }

        // pantalla
        int xres = 1280, yres = 800;
        try {
            String[] r = v.res.toLowerCase(java.util.Locale.ROOT).split("x");
            xres = Integer.parseInt(r[0].trim()); yres = Integer.parseInt(r[1].trim());
        } catch (Exception ignored) { }
        String disp = v.display;
        if (disp.equals("ramfb")) { a.add("-device"); a.add("ramfb"); }
        else if (disp.equals("virtio-gpu")) { a.add("-device"); a.add((arm ? "virtio-gpu-pci" : "virtio-vga") + ",xres=" + xres + ",yres=" + yres); }
        else if (disp.equals("std")) { a.add("-device"); a.add((arm ? "bochs-display" : "VGA") + ",xres=" + xres + ",yres=" + yres); }
        else if (disp.equals("ramfb+virtio")) {
            a.add("-device"); a.add("ramfb");
            a.add("-device"); a.add("virtio-gpu-pci,xres=" + xres + ",yres=" + yres);
        }

        // USB, teclado y ratón absoluto (tableta)
        a.add("-device"); a.add("qemu-xhci,id=xhci");
        if (v.usbKbd || arm) { a.add("-device"); a.add("usb-kbd"); }
        a.add("-device"); a.add("usb-tablet");

        boolean needScsi = "scsi".equals(v.diskIf) || "scsi".equals(v.cdIf);
        if (needScsi) { a.add("-device"); a.add("virtio-scsi-pci,id=scsi0"); }
        boolean sataBus = !arm && (machine.startsWith("q35"));
        boolean hasIso = v.iso.length() > 0;

        // disco principal
        int bootDisk = (v.bootCd && hasIso) ? 2 : 0, bootCd = (v.bootCd && hasIso) ? 0 : 2;
        if (v.disk.length() > 0) addDisk(a, v.disk, "hd0", v.diskIf, bootDisk, sataBus, 0);
        if (v.disk2.length() > 0) addDisk(a, v.disk2, "hd1", "virtio".equals(v.diskIf) || "nvme".equals(v.diskIf) ? v.diskIf : "virtio", 5, sataBus, 2);

        // CD/DVD (siempre hay una unidad, aunque esté vacía, para poder meter una ISO en marcha)
        a.add("-drive"); a.add(drive(hasIso ? v.iso : "", "cd0", "media=cdrom,readonly=on" + (hasIso ? ",format=raw,file.locking=off" : "")));
        String cd = v.cdIf;
        if (cd.equals("usb")) { a.add("-device"); a.add("usb-storage,drive=cd0,removable=on,bootindex=" + bootCd); }
        else if (cd.equals("scsi")) { a.add("-device"); a.add("scsi-cd,drive=cd0,bus=scsi0.0,bootindex=" + bootCd); }
        else { a.add("-device"); a.add("ide-cd,drive=cd0,bus=ide." + (sataBus ? "1" : "1") + ",bootindex=" + bootCd); }

        // Windows 11: saltar requisitos con un autounattend.xml en una memoria USB virtual
        if (v.isWin() && v.winBypass) {
            File ud = unattendDir(c, v);
            a.add("-drive"); a.add(drive("fat:" + ud.getAbsolutePath(), "unattend", "format=raw,readonly=on"));
            a.add("-device"); a.add("usb-storage,drive=unattend,removable=on");
        }

        // carpeta compartida (unidad FAT virtual)
        if (v.share.length() > 0 && new File(v.share).isDirectory()) {
            a.add("-drive"); a.add(drive("fat:" + (v.shareRw ? "rw:" : "") + v.share, "share0", "format=raw" + (v.shareRw ? "" : ",readonly=on")));
            a.add("-device"); a.add("usb-storage,drive=share0");
        }

        // red
        if ("user".equals(v.net)) {
            StringBuilder n = new StringBuilder("user,id=n0");
            for (String f : v.fwd.split("[,;\\s]+")) {
                f = f.trim();
                if (f.length() == 0) continue;
                // formatos: 2222:22  |  tcp:2222:22  |  udp:5000:5000
                String[] p = f.split(":");
                String proto = "tcp", hp, gp;
                if (p.length == 3) { proto = p[0]; hp = p[1]; gp = p[2]; }
                else if (p.length == 2) { hp = p[0]; gp = p[1]; }
                else continue;
                n.append(",hostfwd=").append(proto).append("::").append(hp).append("-:").append(gp);
            }
            a.add("-netdev"); a.add(n.toString());
            String nic = v.nic;
            String dev = nic.equals("virtio") ? "virtio-net-pci" : nic.equals("usb-net") ? "usb-net" : nic;
            a.add("-device"); a.add(dev + ",netdev=n0");
        }

        if (v.rtcLocal) { a.add("-rtc"); a.add("base=localtime"); }
        if (v.keymap.length() > 0 && !v.keymap.equals("ninguno")) { a.add("-k"); a.add(v.keymap); }
        a.add("-vnc"); a.add("unix:" + vncSock);
        a.add("-qmp"); a.add("unix:" + qmpSock + ",server=on,wait=off");
        a.add("-display"); a.add("none");
        File saved = new File(v.dir(c), "estado-guardado");
        if (saved.isFile()) {
            String tag = Bins.readSmall(saved);
            if (tag.length() > 0) { a.add("-loadvm"); a.add(tag); }
        }
        a.addAll(U.splitArgs(v.extra));
        return a;
    }

    private static void addDisk(List<String> a, String path, String id, String iface, int boot, boolean sata, int sataPort) {
        a.add("-drive"); a.add(drive(path, id, "format=" + fmt(path) + ",file.locking=off,cache=writeback,discard=unmap,detect-zeroes=unmap"));
        String b = ",bootindex=" + boot;
        if (iface.equals("nvme")) { a.add("-device"); a.add("nvme,drive=" + id + ",serial=DROIDVM" + id + b); }
        else if (iface.equals("usb")) { a.add("-device"); a.add("usb-storage,drive=" + id + b); }
        else if (iface.equals("scsi")) { a.add("-device"); a.add("scsi-hd,drive=" + id + ",bus=scsi0.0" + b); }
        else if (iface.equals("sata") || iface.equals("ide")) { a.add("-device"); a.add("ide-hd,drive=" + id + ",bus=ide." + sataPort + b); }
        else { a.add("-device"); a.add("virtio-blk-pci,drive=" + id + b); }
    }
}
