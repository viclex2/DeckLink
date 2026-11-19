# DroidVM

Máquinas virtuales en Android al estilo VirtualBox, con QEMU 10 (compilado estático para ARM64) por dentro.
Pensado para instalar **Windows 11 ARM64** o cualquier **Linux** desde su ISO.

## Funciones
- Asistente con plantillas: Windows 11 ARM64, Linux ARM64, Linux x86_64, Windows x86_64, otros.
- Discos qcow2 que crecen según se usan; también abre discos de VirtualBox (VDI), VMware (VMDK), Hyper-V (VHD/VHDX) y raw.
- UEFI (EDK2) con NVRAM propia por máquina (en ARM viene a 1024x768).
- Windows 11: salta TPM, Secure Boot, RAM, CPU y la cuenta de Microsoft con un `autounattend.xml` en una memoria USB virtual.
- Pantalla integrada (VNC por socket unix) con ratón táctil o modo touchpad, teclado en pantalla, barra de teclas
  (Ctrl/Alt/Shift/Win fijables, flechas, F1–F12), Ctrl+Alt+Supr, combinaciones y «escribir texto».
- Pausa/reanudar, reiniciar, apagado ACPI, forzar apagado, **guardar estado** (se restaura al arrancar).
- Instantáneas (en marcha con `savevm` o en frío con `qemu-img`), clon completo y enlazado, ampliar disco.
- Cambiar/expulsar la ISO en marcha, segundo disco, carpeta compartida (unidad FAT), red NAT con reenvío de puertos.
- Servicio en primer plano: las VMs siguen funcionando con la pantalla apagada o la app en segundo plano.

## Límites
Sin KVM (los móviles normales no lo dejan usar a las apps) todo se emula con TCG: Linux ARM64 va usable,
Windows 11 ARM tarda horas en instalarse y va lento; x86_64 es mucho más lento todavía.
Con disco NVMe o red USB (lo que Windows ARM reconoce sin drivers) no se puede guardar el estado en marcha;
las instantáneas en frío sí funcionan.

## Compilar
- QEMU: `.github/workflows/qemu.yml` (runner ARM64 + Alpine, `ci/build-qemu.sh`) deja los binarios en la rama `droidvm-qemu`.
- APK: `QEMU_DIR=<carpeta de droidvm-qemu> ./build.sh` (sin Gradle: aapt, javac, dx, apksigner de Ubuntu).
