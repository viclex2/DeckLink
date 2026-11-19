#!/bin/sh
# Compila QEMU estático (musl) para ARM64 dentro de Alpine. Lo usa .github/workflows/qemu.yml
set -eux
QV=${QEMU_VERSION:-10.0.3}
SLIRP=4.8.0
apk add --no-cache build-base linux-headers python3 py3-pip meson samurai bash perl flex bison \
    pkgconf curl xz bzip2 git glib-dev glib-static pixman-dev pixman-static zlib-dev zlib-static \
    pcre2-dev gettext-dev libffi-dev file
for p in pcre2-static gettext-static libffi-static libintl-static; do apk add --no-cache $p || echo "no hay $p"; done

W=/tmp/b; mkdir -p $W; cd $W
# libslirp estática (red NAT de usuario)
curl -fsSL -o slirp.tgz https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v$SLIRP/libslirp-v$SLIRP.tar.gz
tar xzf slirp.tgz
(cd libslirp-v$SLIRP && meson setup b --prefix=/usr --default-library=static --buildtype=release && ninja -C b install)

curl -fsSL -o q.tar.xz https://download.qemu.org/qemu-$QV.tar.xz || { QV=10.0.0; curl -fsSL -o q.tar.xz https://download.qemu.org/qemu-$QV.tar.xz; }
tar xJf q.tar.xz
cd qemu-$QV
mkdir b && cd b
../configure --prefix=/opt/q --target-list=aarch64-softmmu,x86_64-softmmu --static \
    --without-default-features --enable-tcg --enable-kvm --enable-slirp --enable-vnc --enable-pixman \
    --enable-tools --enable-fdt=internal --enable-vvfat --enable-vdi --enable-vhdx --enable-vpc \
    --enable-qcow1 --enable-dmg --enable-parallels --enable-bochs --enable-cloop --enable-qed \
    --enable-install-blobs --disable-docs --disable-werror \
    --extra-ldflags="-Wl,-z,max-page-size=16384"
ninja -j$(nproc) qemu-system-aarch64 qemu-system-x86_64 qemu-img
ninja install || true
O=/w/salida; rm -rf $O; mkdir -p $O/bin $O/share/keymaps
for b in qemu-system-aarch64 qemu-system-x86_64 qemu-img; do cp $b $O/bin/; strip $O/bin/$b; done
file $O/bin/*
S=/opt/q/share/qemu
cp -r $S/keymaps/. $O/share/keymaps/ || true
for f in $S/bios-256k.bin $S/bios.bin $S/vgabios*.bin $S/efi-*.rom $S/kvmvapic.bin $S/linuxboot*.bin \
         $S/multiboot*.bin $S/pvh.bin $S/edk2-aarch64-code.fd $S/edk2-arm-vars.fd \
         $S/edk2-x86_64-code.fd $S/edk2-i386-vars.fd; do [ -f "$f" ] && cp "$f" $O/share/; done
ls -la $O/bin $O/share
# pruebas rápidas
$O/bin/qemu-system-aarch64 --version
$O/bin/qemu-system-aarch64 -M virt -device help | grep -E '"(nvme|ramfb|virtio-gpu-pci|usb-storage|usb-net|qemu-xhci|usb-tablet|virtio-net-pci|virtio-scsi-pci|scsi-cd)"'
$O/bin/qemu-system-x86_64 -device help | grep -E '"(e1000|ide-cd|VGA|qemu-xhci)"'
$O/bin/qemu-img --help | tail -5
cd /tmp && $O/bin/qemu-img create -f qcow2 t.qcow2 1G
mkdir -p /tmp/fat && echo hola > /tmp/fat/a.txt
printf 'info block\nquit\n' | timeout 60 $O/bin/qemu-system-aarch64 -L $O/share -M virt -cpu max -m 512 -accel tcg,thread=multi \
  -drive if=pflash,format=raw,readonly=on,file=$O/share/edk2-aarch64-code.fd \
  -drive file=t.qcow2,if=none,id=hd0 -device nvme,drive=hd0,serial=x -device ramfb -device qemu-xhci \
  -device usb-kbd -device usb-tablet -drive file=fat:/tmp/fat,format=raw,if=none,id=sf,readonly=on \
  -device usb-storage,drive=sf -netdev user,id=n0 -device usb-net,netdev=n0 -vnc 127.0.0.1:5 -k es -monitor stdio -display none
echo OK
