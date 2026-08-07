#!/usr/bin/env bash
#
# build.sh — generate AetherKiri self-test demo resources and (optionally)
# pack data.xp3 for the built-in flow. The engine also accepts the plain
# data directory, which is the fast development path:
#
#   aetherkiri_sdl --game demos/aetherkiri-test/data
#
set -euo pipefail

demo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
data_dir="${demo_dir}/data"

echo "==> Generating resources"
python3 "${demo_dir}/tools/gen_test_wav.py" "${data_dir}/bgm/test_bgm.wav"

if [[ "${1:-}" == "--xp3" ]]; then
    krkrrel_bin="${KRKRREL_BIN:-krkrrel}"
    if ! command -v "${krkrrel_bin}" >/dev/null 2>&1; then
        echo "krkrrel not found; skipping xp3 packaging" >&2
        exit 0
    fi
    output="${demo_dir}/data.xp3"
    "${krkrrel_bin}" "${data_dir}" -out "${output}"
    shasum -a 256 "${output}"
    echo "packed ${output}"
fi

echo "Run with: aetherkiri_sdl --game ${data_dir}"
