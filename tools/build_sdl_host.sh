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
#   ./tools/build_sdl_host.sh macos debug
#
# macOS: reuses a shared vcpkg checkout if one exists
# (AETHERKIRI_REF_VCPKG_ROOT, default <project>/.devtools/vcpkg, fallback
# ~/Documents/AetherKiri/.devtools/vcpkg) for cached downloads/buildtrees;
# embeds @rpath to the vcpkg_installed dylibs and ad-hoc codesigns the
# binaries afterwards.

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
# On macOS, reuse a pre-existing shared vcpkg checkout (downloads/buildtrees
# from other machines/clones) instead of bootstrapping a fresh one. Point
# AETHERKIRI_REF_VCPKG_ROOT at any full vcpkg root to reuse its caches.
if [[ "$platform" == "macos" ]]; then
    _ref_vcpkg="${AETHERKIRI_REF_VCPKG_ROOT:-${PROJECT_ROOT}/.devtools/vcpkg}"
    if [[ ! -x "$_ref_vcpkg/vcpkg" ]]; then
        _ref_vcpkg="${AETHERKIRI_REF_VCPKG_ROOT:-/Users/colorcard/Documents/AetherKiri/.devtools/vcpkg}"
    fi
    if [[ -x "$_ref_vcpkg/vcpkg" ]]; then
        export VCPKG_ROOT="${VCPKG_ROOT:-$_ref_vcpkg}"
        export VCPKG_DOWNLOADS="${VCPKG_DOWNLOADS:-${VCPKG_ROOT}/downloads}"
        echo "[build_sdl_host] reusing shared vcpkg root: $VCPKG_ROOT"
    fi
fi
export VCPKG_ROOT="${VCPKG_ROOT:-${AETHERKIRI_CACHE_DIR}/vcpkg}"
export VCPKG_DOWNLOADS="${VCPKG_DOWNLOADS:-${AETHERKIRI_CACHE_DIR}/vcpkg-downloads}"
export VCPKG_DEFAULT_BINARY_CACHE="${VCPKG_DEFAULT_BINARY_CACHE:-${AETHERKIRI_CACHE_DIR}/vcpkg-binaries}"
mkdir -p "$VCPKG_DOWNLOADS" "$VCPKG_DEFAULT_BINARY_CACHE"

if [[ "$platform" == "macos" && -z "$(command -v ninja)" ]]; then
    echo "error: ninja is required on macOS (brew install ninja)" >&2
    exit 1
fi

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

binary="aetherkiri_ui"
if [[ "$platform" == "windows" ]]; then
    binary="aetherkiri_ui.exe"
fi

echo "[build_sdl_host] done: $out_dir"
# The preset binaryDir is ${sourceDir}/out/<platform>/<type> (not the
# out/sdl/... path used for clean/echo above). Verify the real artifacts;
# || true keeps the script exit code 0 when nothing matches.
find "$PROJECT_ROOT/out/$platform/$build_type" \
    \( -name "$binary" -o -name "libengine_api*" \) 2>/dev/null || true

# ── macOS post-build fixups ─────────────────────────────────────────────
# vcpkg dylibs (SDL3, ffmpeg, openal-soft, ...) and libengine_api.dylib are
# not on the default dylib search path. Add @rpath entries so the binary and
# the engine dylib resolve them without DYLD_* env vars, then re-sign
# ad-hoc (install_name_tool invalidates the linker's signature).
if [[ "$platform" == "macos" ]]; then
    preset_dir="$PROJECT_ROOT/out/$platform/$build_type"
    vcpkg_lib_dir="$preset_dir/vcpkg_installed/arm64-osx/lib"
    engine_api_dir="$preset_dir/bridge/engine_api"
    machos=()
    for f in "$preset_dir/apps/sdl_host/$binary" "$engine_api_dir/libengine_api.dylib"; do
        [[ -f "$f" ]] && machos+=("$f")
    done
    if [[ ${#machos[@]} -gt 0 ]]; then
        echo "[build_sdl_host] fixing macOS dylib search paths (rpath + ad-hoc sign)"
        for f in "${machos[@]}"; do
            existing="$(otool -l "$f" 2>/dev/null | grep -A2 "LC_RPATH" | grep path || true)"
            if ! grep -q "vcpkg_installed/arm64-osx/lib" <<<"$existing"; then
                install_name_tool -add_rpath "$vcpkg_lib_dir" "$f" \
                    >/dev/null 2>&1 || true
            fi
            if [[ "$f" == *"aetherkiri_sdl" ]] && ! grep -q "bridge/engine_api" <<<"$existing"; then
                install_name_tool -add_rpath "$engine_api_dir" "$f" \
                    >/dev/null 2>&1 || true
            fi
            codesign --force -s - "$f" >/dev/null 2>&1 || \
                echo "warning: ad-hoc codesign failed for $f" >&2
        done
        echo "[build_sdl_host] macOS run: ${machos[0]} --game <abs-path>"
    else
        echo "warning: no Mach-O binaries found for macOS fixups" >&2
    fi
fi
