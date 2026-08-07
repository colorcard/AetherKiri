#pragma once

#include <SDL3/SDL.h>

struct TVPGodotGpuBridgeCallbacks;
struct TVPGodotGpuBatchCallbacks;

namespace aetherkiri {

// Sets the SDL_Renderer backing the GPU bridge callbacks. Call before
// registering the bridge (engine_register_godot_gpu_bridge) and pass nullptr
// before destroying the renderer. All bridge callbacks run on the host main
// thread (engine_tick), so no internal locking is required for the renderer.
void GpuBridgeSetRenderer(SDL_Renderer *renderer);

// Callback tables to pass to engine_register_godot_gpu_bridge /
// engine_register_godot_gpu_batch_bridge. The tables are static and valid for
// the process lifetime.
const TVPGodotGpuBridgeCallbacks *GpuBridgeCallbacks();
const TVPGodotGpuBatchCallbacks *GpuBridgeBatchCallbacks();

// Textures released by the engine (release_texture) are deferred until this
// call so a host frame still referencing one for presentation is never
// destroyed mid-frame. Call after SDL_RenderPresent each frame.
void GpuBridgeFlushReleasedTextures();

}  // namespace aetherkiri
