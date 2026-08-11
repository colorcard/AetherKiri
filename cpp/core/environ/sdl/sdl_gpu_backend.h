#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>

// SDL_GPU device/command-buffer backend for the engine's in-engine GPU
// compositor (sdl3_gpu render backend). The host creates the SDL_GPUDevice,
// claims the window, and injects it here; the engine records compositing
// operations onto a per-frame command buffer and the host submits/presents.
//
// Mirrors sdl_render_backend.h but for the command-buffer SDL_GPU API.

void TVPSetSdlGpuDevice(SDL_GPUDevice *device);
SDL_GPUDevice *TVPGetSdlGpuDevice();

// Queue a GPU texture for destruction after the present that no longer
// references it. The host calls TVPFlushReleasedSdlGpuTextures after present.
void TVPQueueSdlGpuTextureRelease(SDL_GPUTexture *tex);
void TVPFlushReleasedSdlGpuTextures();

// Per-frame command buffer. The engine acquires one lazily at the start of a
// composite and submits it at the end of the frame (or when the host drains
// frames). Returns the current command buffer, creating it if needed.
SDL_GPUCommandBuffer *TVPGetSdlGpuFrameCommandBuffer();
SDL_GPUCopyPass *TVPGetSdlGpuFrameCopyPass();
SDL_GPURenderPass *TVPGetSdlGpuFrameRenderPass();

// Ends any open copy pass so a render pass can begin on the same command
// buffer (SDL_GPU forbids both being open simultaneously).
void TVPEnsureSdlGpuRenderPassReady();

// Submits the current frame command buffer (if any) and waits for GPU idle.
// Called by the host after present and by the engine when CPU readback of a
// GPU-composited frame is required.
void TVPSubmitSdlGpuFrame();
void TVPSubmitSdlGpuFrameAndWait();

// Marks the start/end of engine compositing for the current frame. The engine
// calls TVPBeginSdlGpuComposite once per engine tick; TVPEndSdlGpuComposite
// submits the recorded commands unless the host has already drained them.
void TVPBeginSdlGpuComposite();
void TVPEndSdlGpuComposite();

// Whether a GPU device is currently injected.
bool TVPIsSdlGpuActive();
