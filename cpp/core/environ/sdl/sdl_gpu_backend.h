#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstddef>
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
bool TVPBeginSdlGpuBorrowedFrame(SDL_GPUCommandBuffer *command_buffer);
bool TVPEndSdlGpuBorrowedFrame();
bool TVPIsSdlGpuBorrowedFrame();
void TVPFlushBorrowedSdlGpuTransferBuffers();

struct TVPSdlGpuBenchmarkStats {
    uint64_t command_buffers_acquired = 0;
    uint64_t command_buffers_submitted = 0;
    uint64_t render_passes = 0;
    uint64_t copy_passes = 0;
    uint64_t wait_idle_calls = 0;
    uint64_t upload_calls = 0;
    uint64_t upload_bytes = 0;
    uint64_t full_upload_calls = 0;
    uint64_t full_upload_bytes = 0;
    uint64_t dirty_upload_calls = 0;
    uint64_t dirty_upload_bytes = 0;
    uint64_t readback_calls = 0;
    uint64_t readback_bytes = 0;
    uint64_t gpu_draw_calls = 0;
    uint64_t software_fallback_calls = 0;
    uint64_t fill_argb_gpu_calls = 0;
    uint64_t fill_color_gpu_calls = 0;
    uint64_t alpha_blend_d_gpu_calls = 0;
    uint64_t const_color_alpha_blend_d_gpu_calls = 0;
    uint64_t authority_barrier_calls = 0;
    uint64_t authority_barrier_bytes = 0;
};
TVPSdlGpuBenchmarkStats TVPGetSdlGpuBenchmarkStats();
void TVPResetSdlGpuBenchmarkStats();
void TVPRecordSdlGpuDraw(bool handled_on_gpu);
void TVPRecordSdlGpuMethod(const char *method_name, bool handled_on_gpu);
void TVPRecordSdlGpuAuthorityBarrier(uint64_t bytes);

// Synchronously downloads an SDL_GPU texture into tightly packed RGBA8.
// This is reserved for explicit host readback paths such as screenshots.
bool TVPReadSdlGpuTextureRgba(SDL_GPUTexture *texture, uint32_t width,
                             uint32_t height, void *out_pixels,
                             size_t out_pixels_size);
bool TVPUploadSdlGpuTextureRgba(SDL_GPUTexture *texture, uint32_t width,
                               uint32_t height, const void *pixels,
                               uint32_t pitch);
bool TVPUploadSdlGpuTextureRgbaRegion(
    SDL_GPUTexture *texture, uint32_t texture_width, uint32_t texture_height,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    const void *pixels, uint32_t pitch);
