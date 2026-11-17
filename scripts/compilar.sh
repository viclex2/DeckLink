#!/bin/bash
# Compila los programas de Linux en ./build. Antes, ejecuta scripts/dependencias.sh una vez.
set -e
cd "$(dirname "$0")/.."
P=${P:-$PWD/deps/prefix}
mkdir -p build

# Steam Deck (CPU Zen 2). PipeWire y systemd ya vienen en SteamOS.
gcc -O3 -march=znver2 -Wall -Wno-format-truncation -I"$P/include" -o build/decklink-deck \
    src/decklink-deck.c src/terceros/lz4.c \
    $(pkg-config --cflags --libs libpipewire-0.3 libsystemd) "$P/lib/libturbojpeg.a" -lpthread

# Portátil con Linux: cualquier x86-64 de los últimos ~15 años, sin dependencias extra.
# compat.c se compila sin _GNU_SOURCE para funcionar con glibc antiguas (>= 2.34).
gcc -O2 -std=c11 -fno-builtin -c src/compat.c -o build/compat.o
gcc -O2 -march=x86-64-v2 -Wall -I"$P/include" -o build/decklink-portatil \
    src/decklink-portatil.c src/terceros/lz4.c build/compat.o \
    "$P/lib/libSDL2.a" "$P/lib/libturbojpeg.a" -lm -ldl -lpthread

echo "Compilado en build/: decklink-deck (Steam Deck) y decklink-portatil (Linux)"
