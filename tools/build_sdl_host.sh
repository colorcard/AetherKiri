#!/usr/bin/env bash
#
# build_sdl_host.sh — build the SDL3 host (engine library + host app) without
# Godot. Desktop platforms only (Linux/macOS/Windows).
#
# Usage:
#   ./tools/build_sdl_host.sh <linux|macos|windows> <debug|release> [--jobs=N] [--clean]
#
# Examples:
#   ./tools/build_sdl_host.sh linux debug
#   ./tools/build_sdl_host.sh linux release --jobs=16
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# ── Arguments ───────────────────────────────────────────────────────────
platform="${1:-linux}"
build_type="${2:-debug}"
jobs=8
clean=0
for arg in "${@:3}"; do
    case "$arg" in
        --jobs=*) jobs="${arg#--jobs=}" ;;
        --clean) clean=1 ;;
        *) echo "Unknown option: $arg" >&2; exit 1 ;;
    esac
done

case "$build_type" in
    debug|release) ;;
    *) echo "Build type must be debug|release, got: $build_type" >&2; exit 1 ;;
esac

case "$platform" in
    linux|macos|windows) ;;
    *) echo "Platform must be linux|macos|windows, got: $platform" >&2; exit 1 ;;
esac

# ── Environment (linux_env.sh equivalent, bash-agnostic) ────────────────
export AETHERKIRI_CACHE_DIR="${AETHERKIRI_CACHE_DIR:-${PROJECT_ROOT}/.aetherkiri-cache}"
export AETHERKIRI_SYSTEM_TOOLS_DIR="${AETHERKIRI_SYSTEM_TOOLS_DIR:-${AETHERKIRI_CACHE_DIR}/system-tools}"
if [[ -d "${AETHERKIRI_SYSTEM_TOOLS_DIR}/bin" ]]; then
    export PATH="${AETHERKIRI_SYSTEM_TOOLS_DIR}/bin:${PATH}"
fi
export CCACHE_DIR="${CCACHE_DIR:-${AETHERKIRI_CACHE_DIR}/ccache}"
export VCPKG_ROOT="${VCPKG_ROOT:-${AETHERKIRI_CACHE_DIR}/vcpkg}"
export VCPKG_DOWNLOADS="${VCPKG_DOWNLOADS:-${AETHERKIRI_CACHE_DIR}/vcpkg-downloads}"
export VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-${AETHERKIRI_CACHE_DIR}/vcpkg-binaries}"
mkdir -p "$VCPKG_DOWNLOADS" "$VCPKG_DEFAULT_BINARY_CACHE"

preset=""
case "$platform" in
    linux) preset="Linux" ;;
    macos) preset="MacOS" ;;
    windows) preset="Windows" ;;
esac
preset="${preset} $(tr '[:lower:]' '[:upper:]' <<< "${build_type:0:1}")${build_type:1} Config"

out_dir="$PROJECT_ROOT/out/sdl/$platform/$build_type"
if [[ $clean -eq 1 && -d "$out_dir" ]]; then
    echo "[build_sdl_host] cleaning $out_dir"
    rm -rf "$out_dir"
fi

echo "[build_sdl_host] configuring $platform/$build_type (preset: $preset)"
cmake --preset "$preset" \
    -DBUILD_GODOT_EXTENSION=OFF \
    -DBUILD_GPU_BRIDGE=OFF \
    -DBUILD_SDL_HOST=ON \
    -DAETHERKIRI_ENABLE_INTERNAL=OFF \
    -DCMAKE_MAKE_PROGRAM="${CMAKE_MAKE_PROGRAM:-$(command -v ninja || true)}"

echo "[build_sdl_host] building with $jobs jobs"
cmake --build --preset "${preset% Config} Build" --parallel "$jobs"

binary="aetherkiri_sdl"
if [[ "$platform" == "windows" ]]; then
    binary="aetherkiri_sdl.exe"
fi

echo "[build_sdl_host] done: $out_dir"
find "$out_dir" -name "$binary" -o -name "libengine_api*" 2>/dev/null
