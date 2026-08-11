#include "sdl_gpu_backend.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

SDL_GPUDevice *g_sdl_gpu_device = nullptr;
SDL_GPUTransferBuffer *g_readback_buffer = nullptr;
uint32_t g_readback_buffer_size = 0;
SDL_GPUTransferBuffer *g_upload_buffer = nullptr;
uint32_t g_upload_buffer_size = 0;

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
    if(g_readback_buffer != nullptr && g_sdl_gpu_device != nullptr) {
        SDL_WaitForGPUIdle(g_sdl_gpu_device);
        SDL_ReleaseGPUTransferBuffer(g_sdl_gpu_device, g_readback_buffer);
        g_readback_buffer = nullptr;
        g_readback_buffer_size = 0;
    }
    if(g_upload_buffer != nullptr && g_sdl_gpu_device != nullptr) {
        SDL_WaitForGPUIdle(g_sdl_gpu_device);
        SDL_ReleaseGPUTransferBuffer(g_sdl_gpu_device, g_upload_buffer);
        g_upload_buffer = nullptr;
        g_upload_buffer_size = 0;
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
    if(g_readback_buffer == nullptr || g_readback_buffer_size < required) {
        if(g_readback_buffer != nullptr) {
            SDL_WaitForGPUIdle(g_sdl_gpu_device);
            SDL_ReleaseGPUTransferBuffer(g_sdl_gpu_device,
                                         g_readback_buffer);
        }
        SDL_GPUTransferBufferCreateInfo tb_info{};
        tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tb_info.size = static_cast<Uint32>(required);
        g_readback_buffer =
            SDL_CreateGPUTransferBuffer(g_sdl_gpu_device, &tb_info);
        g_readback_buffer_size = g_readback_buffer != nullptr
            ? static_cast<uint32_t>(required)
            : 0;
    }
    SDL_GPUTransferBuffer *tb = g_readback_buffer;
    if(tb == nullptr) return false;

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
    return ok;
}

bool TVPUploadSdlGpuTextureRgba(SDL_GPUTexture *texture, uint32_t width,
                               uint32_t height, const void *pixels,
                               uint32_t pitch) {
    if(g_sdl_gpu_device == nullptr || texture == nullptr || pixels == nullptr ||
       width == 0 || height == 0)
        return false;
    const uint64_t required64 = static_cast<uint64_t>(width) * height * 4u;
    if(required64 > UINT32_MAX) return false;
    const uint32_t required = static_cast<uint32_t>(required64);
    // The single staging buffer is deliberately serialized. Mapping with
    // cycle=true lets SDL allocate one backing store per upload until submit;
    // asset-heavy scene transitions can then reserve multiple gigabytes in a
    // single frame. Submit before reuse to keep memory strictly bounded.
    TVPSubmitSdlGpuFrameAndWait();
    if(g_upload_buffer == nullptr || g_upload_buffer_size < required) {
        if(g_upload_buffer != nullptr)
            SDL_ReleaseGPUTransferBuffer(g_sdl_gpu_device, g_upload_buffer);
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = required;
        g_upload_buffer = SDL_CreateGPUTransferBuffer(g_sdl_gpu_device, &info);
        g_upload_buffer_size = g_upload_buffer != nullptr ? required : 0;
    }
    if(g_upload_buffer == nullptr) return false;
    void *mapped =
        SDL_MapGPUTransferBuffer(g_sdl_gpu_device, g_upload_buffer, false);
    if(mapped == nullptr) return false;
    const uint32_t tight_pitch = width * 4u;
    const auto *src = static_cast<const uint8_t *>(pixels);
    auto *dst = static_cast<uint8_t *>(mapped);
    for(uint32_t y = 0; y < height; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * tight_pitch,
                    src + static_cast<size_t>(y) * pitch, tight_pitch);
    SDL_UnmapGPUTransferBuffer(g_sdl_gpu_device, g_upload_buffer);

    SDL_GPUCopyPass *cp = TVPGetSdlGpuFrameCopyPass();
    if(cp == nullptr) return false;
    SDL_GPUTextureTransferInfo src_info{};
    src_info.transfer_buffer = g_upload_buffer;
    src_info.pixels_per_row = width;
    src_info.rows_per_layer = height;
    SDL_GPUTextureRegion dst_region{};
    dst_region.texture = texture;
    dst_region.w = width;
    dst_region.h = height;
    dst_region.d = 1;
    SDL_UploadToGPUTexture(cp, &src_info, &dst_region, false);
    TVPSubmitSdlGpuFrameAndWait();
    return true;
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
