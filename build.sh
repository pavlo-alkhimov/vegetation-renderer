#!/usr/bin/env bash
# Unity builds with clang, no CMake (docs/01).
#
#   ./build.sh [debug|release]      -> build/vr, build/cook_terrain, build/shaders/*.spv
#
# Needs: clang++, Vulkan headers + loader, SDL3 (pkg-config sdl3), slangc (Vulkan SDK >= 1.3.296 or Slang release).
# Override tools with CXX=..., SLANGC=...; extra SDL3 location with PKG_CONFIG_PATH=<prefix>/lib/pkgconfig.
set -euo pipefail
cd "$(dirname "$0")"

mode="${1:-debug}"
CXX="${CXX:-clang++}"
SLANGC="${SLANGC:-slangc}"
common="-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-function"
case "$mode" in
    debug)   opt="-g -O0 -DVR_DEBUG" ;;
    release) opt="-g -O2" ;;
    *)       echo "usage: ./build.sh [debug|release]" >&2; exit 1 ;;
esac

pkg-config --exists sdl3 || { echo "SDL3 not found by pkg-config (install libsdl3-dev / SDL3-devel, or set PKG_CONFIG_PATH)" >&2; exit 1; }
command -v "$SLANGC" >/dev/null || { echo "slangc not found (Vulkan SDK or github.com/shader-slang/slang releases; or set SLANGC)" >&2; exit 1; }

mkdir -p build/shaders
"$SLANGC" shaders/terrain.slang -target spirv -o build/shaders/terrain.spv

sdl_cflags="$(pkg-config --cflags sdl3)"
sdl_libs="$(pkg-config --libs sdl3) -Wl,-rpath,$(pkg-config --variable=libdir sdl3)"
# shellcheck disable=SC2086
"$CXX" $common $opt $sdl_cflags src/vr.cpp -o build/vr $sdl_libs -lvulkan -lm
# The cooker decodes ~1 GB of GeoTIFF: always optimized.
# shellcheck disable=SC2086
"$CXX" $common -g -O2 src/tools/cook_terrain.cpp -o build/cook_terrain -lpthread -lm
echo "built ($mode): build/vr build/cook_terrain build/shaders/terrain.spv"
