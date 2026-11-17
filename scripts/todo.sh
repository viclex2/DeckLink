#!/bin/bash
# Compila y empaqueta todo: Steam Deck, Linux, Windows y Android -> ./dist
set -e
cd "$(dirname "$0")/.."
[ -f deps/prefix/lib/libSDL2.a ] || scripts/dependencias.sh
[ -f deps/win64/lib/libSDL2.a ] || scripts/dependencias-windows.sh
scripts/compilar.sh
scripts/empaquetar.sh
scripts/compilar-windows.sh
scripts/compilar-android.sh
ls -la dist/*.zip dist/*.exe dist/*.apk
