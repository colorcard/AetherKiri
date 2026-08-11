#include "sdl_gpu_backend.h"

#include <mutex>
#include <vector>

namespace {

SDL_GPUDevice *g_sdl_gpu_device = nullptr;

std::mutex g_released_mutex;
std::vector<SDL_GPUTexture *> g_released_textures;

// Per-frame command buffer state. Compositing operations are recorded onto
// one command buffer per engine tick; it is submitted at the end of the frame
// (TVPEndSdlGpuComposite) or drained by the host after present.
SDL_GPUCommandBuffer *g_frame_cmd = nullptr;
SDL_GPUCopyPass *g_frame_copy_pass = nullptr;
SDL_GPURenderPass *g_frame_render_pass = nullptr;
bool g_compositing = false;

}  // namespace

void TVPSetSdlGpuDevice(SDL_GPUDevice *device) {
    if (g_frame_cmd != nullptr) {
        TVPSubmitSdlGpuFrameAndWait();
    }
    g_sdl_gpu_device = device;
}

SDL_GPUDevice *TVPGetSdlGpuDevice() { return g_sdl_gpu_device; }

bool TVPIsSdlGpuActive() { return g_sdl_gpu_device != nullptr; }

void TVPQueueSdlGpuTextureRelease(SDL_GPUTexture *tex) {
    if (tex == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_released_mutex);
    g_released_textures.push_back(tex);
}

void TVPFlushReleasedSdlGpuTextures() {
    std::vector<SDL_GPUTexture *> pending;
    {
        std::lock_guard<std::mutex> lock(g_released_mutex);
        pending.swap(g_released_textures);
    }
    if (pending.empty()) {
        return;
    }
    if (g_sdl_gpu_device == nullptr) {
        return;
    }
    for (SDL_GPUTexture *tex : pending) {
        SDL_ReleaseGPUTexture(g_sdl_gpu_device, tex);
    }
}

SDL_GPUCommandBuffer *TVPGetSdlGpuFrameCommandBuffer() {
    if (g_sdl_gpu_device == nullptr) {
        return nullptr;
    }
    if (g_frame_cmd == nullptr) {
        g_frame_cmd = SDL_AcquireGPUCommandBuffer(g_sdl_gpu_device);
    }
    return g_frame_cmd;
}

SDL_GPUCopyPass *TVPGetSdlGpuFrameCopyPass() {
    SDL_GPUCommandBuffer *cmd = TVPGetSdlGpuFrameCommandBuffer();
    if (cmd == nullptr) {
        return nullptr;
    }
    // A copy pass and a render pass cannot be open on the same command buffer
    // at once. Ending an open render pass before a copy pass (or vice versa)
    // is required by SDL_GPU.
    if (g_frame_render_pass != nullptr) {
        SDL_EndGPURenderPass(g_frame_render_pass);
        g_frame_render_pass = nullptr;
    }
    if (g_frame_copy_pass == nullptr) {
        g_frame_copy_pass = SDL_BeginGPUCopyPass(cmd);
    }
    return g_frame_copy_pass;
}

SDL_GPURenderPass *TVPGetSdlGpuFrameRenderPass() {
    SDL_GPUCommandBuffer *cmd = TVPGetSdlGpuFrameCommandBuffer();
    if (cmd == nullptr) {
        return nullptr;
    }
    if (g_frame_copy_pass != nullptr) {
        SDL_EndGPUCopyPass(g_frame_copy_pass);
        g_frame_copy_pass = nullptr;
    }
    return g_frame_render_pass;
}

void TVPEnsureSdlGpuRenderPassReady() {
    if (g_frame_copy_pass != nullptr) {
        SDL_EndGPUCopyPass(g_frame_copy_pass);
        g_frame_copy_pass = nullptr;
    }
}

void TVPSubmitSdlGpuFrame() {
    if (g_sdl_gpu_device == nullptr) {
        return;
    }
    if (g_frame_copy_pass != nullptr) {
        SDL_EndGPUCopyPass(g_frame_copy_pass);
        g_frame_copy_pass = nullptr;
    }
    if (g_frame_render_pass != nullptr) {
        SDL_EndGPURenderPass(g_frame_render_pass);
        g_frame_render_pass = nullptr;
    }
    if (g_frame_cmd != nullptr) {
        SDL_SubmitGPUCommandBuffer(g_frame_cmd);
        g_frame_cmd = nullptr;
    }
    g_compositing = false;
}

void TVPSubmitSdlGpuFrameAndWait() {
    if (g_sdl_gpu_device == nullptr) {
        return;
    }
    TVPSubmitSdlGpuFrame();
    SDL_WaitForGPUIdle(g_sdl_gpu_device);
}

void TVPBeginSdlGpuComposite() {
    if (g_sdl_gpu_device == nullptr) {
        return;
    }
    g_compositing = true;
}

void TVPEndSdlGpuComposite() { TVPSubmitSdlGpuFrame(); }
