#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
host="$repo_dir/out/linux/release/apps/sdl_host/aetherkiri_ui"
runtime_lib="$repo_dir/out/linux/release/vcpkg_installed/x64-linux/lib"
game="$repo_dir/demos/aetherkiri-gpu-bench/data"
runs="${AETHERKIRI_BENCHMARK_RUNS:-5}"
duration="${AETHERKIRI_BENCHMARK_SECONDS:-15}"
warmup="${AETHERKIRI_BENCHMARK_WARMUP_SECONDS:-5}"
output_dir="${1:-$repo_dir/out/benchmarks/sdl_gpu_ab}"

if [[ ! -x "$host" ]]; then
    echo "Release host not found: $host" >&2
    exit 2
fi
if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "A real desktop session is required; Xvfb results are not accepted." >&2
    exit 2
fi
mkdir -p "$output_dir"

run_case() {
    local name="$1" d_blend="$2" disable_fill="$3" disable_dirty="$4"
    local offscreen="$5"
    local run log enable_dirty
    enable_dirty=$((disable_dirty ? 0 : 1))
    local extra=()
    if [[ "$offscreen" == 1 ]]; then extra+=(--benchmark-offscreen); fi
    for ((run = 1; run <= runs; ++run)); do
        log="$output_dir/${name}-${run}.log"
        echo "benchmark_case=$name run=$run/$runs" >&2
        if ! AETHERKIRI_SDL_GPU_ENABLE_D_BLEND="$d_blend" \
             AETHERKIRI_SDL_GPU_DISABLE_FILL="$disable_fill" \
             AETHERKIRI_SDL_GPU_ENABLE_DIRTY_UPLOAD="$enable_dirty" \
             AETHERKIRI_SDL_GPU_ENABLE_MIXED_DRAWS=1 \
             LD_LIBRARY_PATH="$runtime_lib" \
             "$host" --game "$game" --render-backend sdl3_gpu --fps 0 \
                 --benchmark "$duration" --benchmark-warmup "$warmup" \
                 --present-mode immediate "${extra[@]}" >"$log" 2>&1; then
            echo "benchmark failed; log: $log" >&2
            tail -80 "$log" >&2
            return 1
        fi
        sed -n 's/^benchmark_json: //p' "$log"
    done
}

run_case baseline-present 0 1 1 0
run_case fill-dirty-present 0 0 0 0
run_case d-blend-present 1 0 0 0
run_case baseline-offscreen 0 1 1 1
run_case fill-dirty-offscreen 0 0 0 1
run_case d-blend-offscreen 1 0 0 1
