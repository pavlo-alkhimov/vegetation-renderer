#!/usr/bin/env bash
# Unity builds with clang, no CMake (docs/01). Linux and macOS.
#
#   ./build.sh [debug|release]      -> build/vr, build/cook_terrain, build/shaders/*.spv
#
# Needs: clang++, Vulkan headers (loader at runtime, opened by SDL), SDL3 (pkg-config sdl3),
# slangc (Vulkan SDK >= 1.3.296 or Slang release). With $VULKAN_SDK set (SDK setup-env.sh), headers and slangc are
# taken from it. Override tools with CXX=..., SLANGC=...; extra SDL3 location with PKG_CONFIG_PATH=<prefix>/lib/pkgconfig.
set -euo pipefail
cd "$(dirname "$0")"

mode="${1:-debug}"
CXX="${CXX:-clang++}"
if [[ -z "${SLANGC:-}" ]]; then
    SLANGC=slangc
    [[ -n "${VULKAN_SDK:-}" && -x "$VULKAN_SDK/bin/slangc" ]] && SLANGC="$VULKAN_SDK/bin/slangc"
fi
common="-std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-function"
case "$mode" in
    debug)   opt="-g -O0 -DVR_DEBUG" ;;
    release) opt="-g -O2" ;;
    *)       echo "usage: ./build.sh [debug|release]" >&2; exit 1 ;;
esac

pkg-config --exists sdl3 || { echo "SDL3 not found by pkg-config (libsdl3-dev / SDL3-devel / brew install sdl3, or set PKG_CONFIG_PATH)" >&2; exit 1; }
command -v "$SLANGC" >/dev/null || { echo "slangc not found (Vulkan SDK or github.com/shader-slang/slang releases; or set SLANGC)" >&2; exit 1; }

mkdir -p build/shaders
"$SLANGC" shaders/terrain.slang -target spirv -o build/shaders/terrain.spv
"$SLANGC" shaders/overlay.slang -target spirv -o build/shaders/overlay.spv
"$SLANGC" shaders/taa.slang -target spirv -o build/shaders/taa.spv
# Task shaders in their own modules (driver issue with task + mesh in one module, see vk.cpp PipelineDesc).
for v in trees; do
    "$SLANGC" shaders/vegetation.slang -target spirv -fvk-use-entrypoint-name -entry "as_$v" -stage amplification \
        -o "build/shaders/veg_${v}_task.spv"
    "$SLANGC" shaders/vegetation.slang -target spirv -fvk-use-entrypoint-name -entry "ms_$v" -stage mesh \
        -entry "fs_$v" -stage fragment -entry "fs_${v}_shadow" -stage fragment -o "build/shaders/veg_$v.spv"
done
"$SLANGC" shaders/ground_cover.slang -target spirv -fvk-use-entrypoint-name -entry as_gc -stage amplification \
    -o build/shaders/veg_gc_task.spv
"$SLANGC" shaders/ground_cover.slang -target spirv -fvk-use-entrypoint-name -entry ms_gc -stage mesh \
    -entry fs_gc -stage fragment -entry fs_gc_shadow -stage fragment -entry fs_gc_bake -stage fragment -entry fs_gc_imp_bake -stage fragment -entry fs_gc_wire -stage fragment -o build/shaders/veg_gc.spv
"$SLANGC" shaders/ground_cover.slang -target spirv -fvk-use-entrypoint-name -entry as_gc_imp -stage amplification \
    -o build/shaders/veg_gc_imp_task.spv
"$SLANGC" shaders/ground_cover.slang -target spirv -fvk-use-entrypoint-name -entry ms_gc_imp -stage mesh \
    -entry fs_gc_imp -stage fragment -entry fs_gc_imp_wire -stage fragment -o build/shaders/veg_gc_imp.spv

vk_cflags=""
[[ -n "${VULKAN_SDK:-}" ]] && vk_cflags="-I$VULKAN_SDK/include"
sdl_cflags="$(pkg-config --cflags sdl3)"
sdl_libs="$(pkg-config --libs sdl3) -Wl,-rpath,$(pkg-config --variable=libdir sdl3)"
# shellcheck disable=SC2086
"$CXX" $common $opt $vk_cflags $sdl_cflags src/vr.cpp -o build/vr $sdl_libs -lm
# The cooker decodes ~1 GB of GeoTIFF: always optimized.
# shellcheck disable=SC2086
"$CXX" $common -g -O2 src/tools/cook_terrain.cpp -o build/cook_terrain -lpthread -lm
# shellcheck disable=SC2086
"$CXX" $common -g -O2 src/tools/cook_ground_cover.cpp -o build/cook_ground_cover -lm
echo "built ($mode): build/vr build/cook_terrain build/cook_ground_cover build/shaders/*.spv"
