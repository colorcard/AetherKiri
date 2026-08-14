#!/usr/bin/env bash
set -euo pipefail

shader_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../cpp/core/visual/sdl3/shaders" && pwd)"
check=0
if [[ "${1:-}" == "--check" ]]; then check=1; fi
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

for source in quad.vert quad.frag fill.vert fill.frag blend_d.frag \
              blend_const_color_d.frag; do
    symbol="${source//./_}_spv"
    glslangValidator -V --target-env vulkan1.0 -o "$tmp_dir/$source.spv" \
        "$shader_dir/$source" >/dev/null
    xxd -i -n "$symbol" "$tmp_dir/$source.spv" >"$tmp_dir/$source.spv.h"
    sed -i 's/^unsigned /static const unsigned /; s/^unsigned int /static const unsigned int /' \
        "$tmp_dir/$source.spv.h"
    sed -i "s/${symbol}_len/${symbol}_size/" "$tmp_dir/$source.spv.h"
    if (( check )); then
        cmp "$tmp_dir/$source.spv.h" "$shader_dir/$source.spv.h"
    else
        cp "$tmp_dir/$source.spv.h" "$shader_dir/$source.spv.h"
    fi
done
