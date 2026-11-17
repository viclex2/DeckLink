#!/bin/bash
# Compila DeckLink.exe y su instalador. Antes, ejecuta scripts/dependencias-windows.sh una vez.
# Necesita: mingw-w64 nsis
set -e
cd "$(dirname "$0")/.."
W=${W:-$PWD/deps/win64}
mkdir -p build dist
(cd paquete/windows && x86_64-w64-mingw32-windres decklink.rc -O coff -o ../../build/decklink-res.o)
x86_64-w64-mingw32-gcc -O2 -Wall -I"$W/include" -o build/DeckLink.exe \
    src/decklink-portatil.c src/terceros/lz4.c build/decklink-res.o \
    "$W/lib/libSDL2.a" "$W/lib/libturbojpeg.a" -static -mwindows \
    -lws2_32 -lwinusb -lsetupapi -lwinmm -limm32 -lversion -lole32 -loleaut32 -luuid -lgdi32 -luser32 -lshell32 -ladvapi32
(cd paquete/windows && makensis -V2 instalador.nsi)
echo "Listo: dist/DeckLink-Windows-Instalador.exe"
