#!/bin/bash
# Compila SDL2 y libjpeg-turbo para Windows (64 bits) desde Linux, con MinGW-w64,
# como librerías estáticas en ./deps/win64. Necesita: mingw-w64 cmake nasm curl.
set -e
cd "$(dirname "$0")/.."
RAIZ=$PWD/deps
P=$RAIZ/win64
SDL_V=2.30.9
JT_V=3.0.4
mkdir -p "$RAIZ/src"
cd "$RAIZ"

cat > toolchain-mingw64.cmake <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

[ -f sdl.tgz ] || curl -sSL -o sdl.tgz "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_V/SDL2-$SDL_V.tar.gz"
[ -f jt.tgz ]  || curl -sSL -o jt.tgz "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$JT_V/libjpeg-turbo-$JT_V.tar.gz"
tar xzf sdl.tgz -C src
tar xzf jt.tgz -C src

cmake -S "src/libjpeg-turbo-$JT_V" -B b-jt-win -DCMAKE_TOOLCHAIN_FILE="$RAIZ/toolchain-mingw64.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=ON -DCMAKE_INSTALL_PREFIX="$P"
cmake --build b-jt-win -j"$(nproc)" && cmake --install b-jt-win

cmake -S "src/SDL2-$SDL_V" -B b-sdl-win -DCMAKE_TOOLCHAIN_FILE="$RAIZ/toolchain-mingw64.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
      -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF -DSDL_SENSOR=OFF -DSDL_POWER=OFF \
      -DSDL_VULKAN=OFF -DSDL2_DISABLE_SDL2MAIN=ON -DCMAKE_INSTALL_PREFIX="$P"
cmake --build b-sdl-win -j"$(nproc)" && cmake --install b-sdl-win

echo "Dependencias de Windows listas en $P"
