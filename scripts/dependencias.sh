#!/bin/bash
# Compila SDL2 y libjpeg-turbo como librerías estáticas en ./deps/prefix.
# Así el programa del portátil funciona en cualquier Linux sin instalar nada.
#
# Necesita (Debian/Ubuntu): build-essential cmake nasm pkg-config curl
#   libpipewire-0.3-dev libsystemd-dev liblz4-dev libpulse-dev libasound2-dev
#   libwayland-dev wayland-protocols libxkbcommon-dev libdecor-0-dev libegl-dev
#   libgles-dev libgl-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev
#   libxi-dev libxfixes-dev libxss-dev libxinerama-dev
set -e
cd "$(dirname "$0")/.."
RAIZ=$PWD/deps
P=$RAIZ/prefix
SDL_V=2.30.9
JT_V=3.0.4
mkdir -p "$RAIZ/src"
cd "$RAIZ"

[ -f sdl.tgz ] || curl -sSL -o sdl.tgz "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_V/SDL2-$SDL_V.tar.gz"
[ -f jt.tgz ]  || curl -sSL -o jt.tgz "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$JT_V/libjpeg-turbo-$JT_V.tar.gz"
tar xzf sdl.tgz -C src
tar xzf jt.tgz -C src

cmake -S "src/libjpeg-turbo-$JT_V" -B b-jt -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF \
      -DENABLE_STATIC=ON -DWITH_TURBOJPEG=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$P"
cmake --build b-jt -j"$(nproc)" && cmake --install b-jt

# SDL2: solo vídeo y sonido; X11, Wayland, PipeWire, PulseAudio y ALSA se cargan al arrancar
cmake -S "src/SDL2-$SDL_V" -B b-sdl -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON \
 -DSDL_AUDIO=ON -DSDL_PIPEWIRE=ON -DSDL_PIPEWIRE_SHARED=ON -DSDL_PULSEAUDIO=ON -DSDL_PULSEAUDIO_SHARED=ON \
 -DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_OSS=OFF \
 -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF -DSDL_SENSOR=OFF -DSDL_POWER=OFF -DSDL_LOCALE=OFF -DSDL_MISC=OFF \
 -DSDL_X11=ON -DSDL_X11_SHARED=ON -DSDL_WAYLAND=ON -DSDL_WAYLAND_SHARED=ON \
 -DSDL_WAYLAND_LIBDECOR=ON -DSDL_WAYLAND_LIBDECOR_SHARED=ON -DSDL_KMSDRM=OFF -DSDL_VULKAN=OFF \
 -DSDL_OPENGL=ON -DSDL_OPENGLES=ON -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_TEST=OFF -DSDL2_DISABLE_SDL2MAIN=ON \
 -DCMAKE_INSTALL_PREFIX="$P"
cmake --build b-sdl -j"$(nproc)" && cmake --install b-sdl

echo "Dependencias listas en $P"
