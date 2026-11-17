#!/bin/bash
# Compila los dos programas en ./build. Antes, ejecuta scripts/dependencias.sh una vez.
set -e
cd "$(dirname "$0")/.."
P=${P:-$PWD/deps/prefix}
LZ4=${LZ4:-$(pkg-config --variable=libdir liblz4 2>/dev/null || echo /usr/lib/x86_64-linux-gnu)/liblz4.a}
mkdir -p build

# Steam Deck (CPU Zen 2). PipeWire y systemd ya vienen en SteamOS.
gcc -O3 -march=znver2 -Wall -Wno-format-truncation -I"$P/include" -o build/decklink-deck src/decklink-deck.c \
    $(pkg-config --cflags --libs libpipewire-0.3 libsystemd) "$P/lib/libturbojpeg.a" "$LZ4" -lpthread

# Portátil: cualquier x86-64 de los últimos ~15 años, sin dependencias extra.
# compat.c se compila sin _GNU_SOURCE para funcionar con glibc antiguas (>= 2.34).
gcc -O2 -std=c11 -fno-builtin -c src/compat.c -o build/compat.o
gcc -O2 -march=x86-64-v2 -Wall -I"$P/include" -o build/decklink-portatil src/decklink-portatil.c build/compat.o \
    "$P/lib/libSDL2.a" "$P/lib/libturbojpeg.a" "$LZ4" -lm -ldl -lpthread

echo "Compilado en build/: decklink-deck (Steam Deck) y decklink-portatil (portátil)"
