#include "sdl_gpu_backend.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
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

bool TVPReadSdlGpuTextureRgba(SDL_GPUTexture *texture, uint32_t width,
                             uint32_t height, void *out_pixels,
                             size_t out_pixels_size) {
    if (g_sdl_gpu_device == nullptr || texture == nullptr ||
        out_pixels == nullptr || width == 0 || height == 0) {
        return false;
    }
    const size_t required = static_cast<size_t>(width) * height * 4u;
    if (out_pixels_size < required || required > UINT32_MAX) {
        return false;
    }

    TVPSubmitSdlGpuFrameAndWait();
    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tb_info.size = static_cast<Uint32>(required);
    SDL_GPUTransferBuffer *tb =
        SDL_CreateGPUTransferBuffer(g_sdl_gpu_device, &tb_info);
    if (tb == nullptr) return false;

    bool ok = false;
    SDL_GPUCommandBuffer *cmd =
        SDL_AcquireGPUCommandBuffer(g_sdl_gpu_device);
    if (cmd != nullptr) {
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        if (cp != nullptr) {
            SDL_GPUTextureRegion src{};
            src.texture = texture;
            src.w = width;
            src.h = height;
            src.d = 1;
            SDL_GPUTextureTransferInfo dst{};
            dst.transfer_buffer = tb;
            dst.pixels_per_row = width;
            dst.rows_per_layer = height;
            SDL_DownloadFromGPUTexture(cp, &src, &dst);
            SDL_EndGPUCopyPass(cp);
            if (SDL_SubmitGPUCommandBuffer(cmd)) {
                SDL_WaitForGPUIdle(g_sdl_gpu_device);
                const void *mapped =
                    SDL_MapGPUTransferBuffer(g_sdl_gpu_device, tb, false);
                if (mapped != nullptr) {
                    std::memcpy(out_pixels, mapped, required);
                    SDL_UnmapGPUTransferBuffer(g_sdl_gpu_device, tb);
                    ok = true;
                }
            }
        } else {
            SDL_CancelGPUCommandBuffer(cmd);
        }
    }
    SDL_ReleaseGPUTransferBuffer(g_sdl_gpu_device, tb);
    return ok;
}

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
