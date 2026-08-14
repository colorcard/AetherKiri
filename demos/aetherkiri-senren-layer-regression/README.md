# Synthetic visual-novel layer regression

This demo reproduces only generic layer behavior observed while investigating
an authorized local game: four stage roots (logo, title animation result,
title menu, first scene), nested ordering, opacity, transparent text edges,
character occlusion, message visibility, state `assign`, and local
invalidation. It contains no extracted scripts, images, names, or other game
assets.

Run at 60 fps and capture stable points halfway through each one-second stage:

```sh
export LD_LIBRARY_PATH="$PWD/out/linux/debug/vcpkg_installed/x64-linux/lib"
export AETHERKIRI_LAYER_SNAPSHOT_JSONL="$PWD/out/compat/synthetic-layers.jsonl"
export AETHERKIRI_LAYER_SNAPSHOT_FRAMES=30,90,150,195
out/linux/debug/apps/aetherkiri_engine/aetherkiri_engine \
  --game "$PWD/demos/aetherkiri-senren-layer-regression/data" \
  --render-backend software --fps 60 --screenshot out/compat/software.ppm \
  --screenshot-frames 210
```

Repeat with `--render-backend sdl3_gpu` and a different screenshot/JSONL path.
The expected stable stage at frames 30, 90, 150, and 195 is respectively
`stage_logo`, `stage_title` without menu, `stage_title` with `title_menu`, and
`stage_scene` with `message_root` visible. Compare PPM RGB channels exactly;
the acceptance threshold is AE=0.

`sdl3_gpu` defaults to one coherent software composite followed by one GPU
upload/present. KiriKiri specifies byte-exact `/256` alpha arithmetic, while
fixed-function UNORM blending uses `/255`; alternating GPU Copy/fill and CPU
alpha also introduces synchronous authority barriers. The mixed compositor is
therefore diagnostic-only via `AETHERKIRI_SDL_GPU_ENABLE_MIXED_DRAWS=1`.

Keys `1` through `4` select a stage manually; Escape closes the demo.
