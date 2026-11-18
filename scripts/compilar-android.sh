#!/bin/bash
# Compila la app de Android (dist/DeckLink-Android.apk) sin Gradle, con las herramientas
# de Android que trae Ubuntu/Debian:
#   sudo apt install android-sdk-platform-23 aapt apksigner zipalign dalvik-exchange openjdk-17-jdk-headless
set -e
cd "$(dirname "$0")/.."
SDK=${ANDROID_JAR:-/usr/lib/android-sdk/platforms/android-23/android.jar}
O=build/android
rm -rf "$O" && mkdir -p "$O/gen" "$O/clases" dist

# 0. versión (la misma que el resto de DeckLink, de src/protocolo.h)
VER=$(sed -n 's/^#define DL_VERSION *"\(.*\)".*/\1/p' src/protocolo.h)
PRU=$(sed -n 's/^#define DL_PRUEBAS *\([01]\).*/\1/p' src/protocolo.h)
MAY=${VER%%.*}; MEN=${VER#*.}; MEN=${MEN%%.*}
CODIGO=$((MAY * 1000 + MEN * 10))          # 2.9 -> 2090 (siempre mayor que los anteriores)
mkdir -p "$O/gen/io/github/viclex2/decklink"
cat > "$O/gen/io/github/viclex2/decklink/Version.java" <<FIN
package io.github.viclex2.decklink;
final class Version {
    static final String VERSION = "$VER";
    static final boolean PRUEBAS = $([ "$PRU" = 1 ] && echo true || echo false);
}
FIN

# 1. recursos (iconos) y R.java
aapt package -f -m -J "$O/gen" -M android/AndroidManifest.xml -S android/res -I "$SDK" -F "$O/base.apk" \
     --version-name "$VER" --version-code "$CODIGO"

# 2. Java -> bytecode (Java 8) -> dex
javac -source 8 -target 8 -bootclasspath "$SDK" -classpath "$SDK" -encoding UTF-8 -nowarn \
      -d "$O/clases" $(find android/src "$O/gen" -name '*.java')
dalvik-exchange --dex --min-sdk-version=21 --output="$O/classes.dex" "$O/clases"

# 3. empaquetar, alinear y firmar
cp "$O/base.apk" "$O/sin_firmar.apk"
(cd "$O" && aapt add sin_firmar.apk classes.dex >/dev/null)
zipalign -f 4 "$O/sin_firmar.apk" "$O/alineado.apk"
apksigner sign --ks android/decklink.keystore --ks-pass pass:decklink --key-pass pass:decklink \
         --min-sdk-version 21 --out dist/DeckLink-Android.apk "$O/alineado.apk"
apksigner verify dist/DeckLink-Android.apk
echo "Listo: dist/DeckLink-Android.apk"
