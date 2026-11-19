#!/bin/bash
# Compila DroidVM (dist/DroidVM.apk) sin Gradle, con las herramientas de Android de Ubuntu/Debian:
#   sudo apt install android-sdk-platform-23 aapt apksigner zipalign dalvik-exchange openjdk-17-jdk-headless
# QEMU (estático, arm64) lo compila GitHub Actions (.github/workflows/qemu.yml) en la rama droidvm-qemu;
# pásale su carpeta en QEMU_DIR (por defecto ../qemu-bin).
set -e
cd "$(dirname "$0")"
VER=${VER:-1.0}
CODE=${CODE:-100}
SDK=${ANDROID_JAR:-/usr/lib/android-sdk/platforms/android-23/android.jar}
Q=${QEMU_DIR:-../qemu-bin}
O=build
rm -rf "$O" && mkdir -p "$O/gen/io/github/viclex2/droidvm" "$O/clases" "$O/apk/lib/arm64-v8a" "$O/assets/qemu" dist
cat > "$O/gen/io/github/viclex2/droidvm/BuildInfo.java" <<FIN
package io.github.viclex2.droidvm;
final class BuildInfo { static final String VERSION = "$VER"; static final int CODE = $CODE; }
FIN
cp app/assets/keysyms.txt "$O/assets/"
if [ -x "$Q/bin/qemu-system-aarch64" ]; then
    cp "$Q/bin/qemu-system-aarch64" "$O/apk/lib/arm64-v8a/libqemu_aarch64.so"
    cp "$Q/bin/qemu-system-x86_64" "$O/apk/lib/arm64-v8a/libqemu_x86_64.so"
    cp "$Q/bin/qemu-img" "$O/apk/lib/arm64-v8a/libqemu_img.so"
    cp -r "$Q/share/." "$O/assets/qemu/"
    gunzip -c app/firmware/edk2-arm-vars-1024.fd.gz > "$O/assets/qemu/edk2-arm-vars-1024.fd"
    rm -f "$O/assets/qemu/edk2-arm-vars.fd"
else
    echo "AVISO: sin binarios de QEMU en $Q"
fi
aapt package -f -m -J "$O/gen" -M app/AndroidManifest.xml -S app/res -A "$O/assets" -I "$SDK" -F "$O/base.apk" \
     --version-name "$VER" --version-code "$CODE"
javac -source 8 -target 8 -bootclasspath "$SDK" -classpath "$SDK" -encoding UTF-8 -Xlint:-options \
      -d "$O/clases" $(find app/src "$O/gen" -name '*.java')
dalvik-exchange --dex --min-sdk-version=26 --output="$O/classes.dex" "$O/clases"
cp "$O/base.apk" "$O/sin_firmar.apk"
(cd "$O" && aapt add sin_firmar.apk classes.dex >/dev/null)
(cd "$O/apk" && [ -n "$(ls lib/arm64-v8a)" ] && zip -q -r ../sin_firmar.apk lib) || true
[ -f droidvm.keystore ] || keytool -genkeypair -keystore droidvm.keystore -storepass droidvm -keypass droidvm \
     -alias droidvm -keyalg RSA -keysize 2048 -validity 20000 -dname "CN=DroidVM, O=viclex2" >/dev/null 2>&1
zipalign -f -p 4 "$O/sin_firmar.apk" "$O/alineado.apk"
apksigner sign --ks droidvm.keystore --ks-pass pass:droidvm --key-pass pass:droidvm --min-sdk-version 26 \
     --out dist/DroidVM.apk "$O/alineado.apk"
apksigner verify dist/DroidVM.apk
ls -la dist/DroidVM.apk
