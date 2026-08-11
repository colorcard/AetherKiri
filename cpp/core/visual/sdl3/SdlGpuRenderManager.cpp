#include "SdlGpuRenderManager.h"
#include "SdlRenderManager.h"

#include "../LayerBitmapIntf.h"
#include "MsgIntf.h"
#include "tjsHashSearch.h"
#include "sdl_gpu_backend.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "shaders/quad.vert.spv.h"
#include "shaders/quad.frag.spv.h"
#include "shaders/fill.vert.spv.h"
#include "shaders/fill.frag.spv.h"
#include "shaders/blend_d.frag.spv.h"

namespace {

// Blend-mode tags shared with the SDL_Renderer backend.
enum : uint32_t {
    TVP_GODOT_GPU_BLEND_ALPHA = 1,
    TVP_GODOT_GPU_BLEND_ALPHA_D = 2,
    TVP_GODOT_GPU_BLEND_COPY_COLOR = 3,
    TVP_GODOT_GPU_BLEND_CONST_ALPHA_SD = 4,
    TVP_GODOT_GPU_BLEND_FILL_ARGB = 5,
    TVP_GODOT_GPU_BLEND_ALPHA_A = 6,
    TVP_GODOT_GPU_BLEND_ALPHA_BLEND_A = 7,
    TVP_GODOT_GPU_BLEND_REMOVE_CONST_OPACITY = 8,
    TVP_GODOT_GPU_BLEND_CONST_ALPHA_SD_D = 9,
    TVP_GODOT_GPU_BLEND_CONST_ALPHA_D = 10,
    TVP_GODOT_GPU_BLEND_PS_SCREEN = 11,
    TVP_GODOT_GPU_BLEND_UNIVERSAL = 12,
    TVP_GODOT_GPU_BLEND_UNIVERSAL_D = 13,
    TVP_GODOT_GPU_BLEND_UNIVERSAL_A = 14,
    TVP_GODOT_GPU_BLEND_PS_MULTIPLY = 15,
    TVP_GODOT_GPU_BLEND_PS_ADD = 16,
    TVP_GODOT_GPU_BLEND_PS_SUBTRACT = 17,
    TVP_GODOT_GPU_BLEND_FILL_MASK = 18,
    TVP_GODOT_GPU_BLEND_APPLY_ALPHA_MASK = 19,
    TVP_GODOT_GPU_BLEND_COPY_RGBA = 20,
    TVP_GODOT_GPU_BLEND_ALPHA_D_MASK_MULTIPLY = 21,
    TVP_GODOT_GPU_BLEND_ALPHA_D_MASK_THRESHOLD = 22,
    TVP_GODOT_GPU_BLEND_TVP_OPERATION = 0x00010000u,
    TVP_GODOT_GPU_BLEND_MASK_WRITE = 0x00020000u,
};

// --- SDL_GPU pipeline/resource cache -------------------------------------
// Pipelines are keyed by blend-state hash plus shader program. The device is
// global (host-injected); resources are created lazily on first use and freed
// when the device is detached.

struct GpuPipelines {
    SDL_GPUShader *quad_vs = nullptr;
    SDL_GPUShader *quad_fs = nullptr;
    SDL_GPUShader *fill_vs = nullptr;
    SDL_GPUShader *fill_fs = nullptr;
    SDL_GPUShader *blend_d_fs = nullptr;
    SDL_GPUSampler *sampler = nullptr;
    SDL_GPUBuffer *quad_vb = nullptr;
    SDL_GPUBuffer *quad_ib = nullptr;
    std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline *> rect_pipes;
    std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline *> fill_pipes;
    SDL_GPUGraphicsPipeline *blend_d_pipe = nullptr;
    SDL_GPUTexture *scratch = nullptr;
    int scratch_w = 0;
    int scratch_h = 0;
};

GpuPipelines *g_pipelines = nullptr;

struct QuadVertex {
    float x, y, u, v;
};

// Unit quad in [0,1]^2, texture coords flip V so that row 0 (top) of the CPU
// image maps to the top of the framebuffer (NDC +Y is up in the vertex
// shader's final -Y flip, so V=0 -> NDC top).
constexpr QuadVertex kQuadVerts[4] = {
    {0.0f, 0.0f, 0.0f, 0.0f},
    {1.0f, 0.0f, 1.0f, 0.0f},
    {1.0f, 1.0f, 1.0f, 1.0f},
    {0.0f, 1.0f, 0.0f, 1.0f},
};
constexpr uint32_t kQuadIndices[6] = {0, 1, 2, 0, 2, 3};

// --- blend table ----------------------------------------------------------
// Maps a TVP_GODOT_GPU_BLEND_* mode to an SDL_GPUColorTargetBlendState.
// Semantics mirror krkrsdl3's RenderManager_gl.cpp SetBlendFuncSeparate calls
// (GL factors translate 1:1 to SDL_GPU_BLENDFACTOR_*).
SDL_GPUColorTargetBlendState BlendStateFor(uint32_t mode, bool *ok) {
    SDL_GPUColorTargetBlendState bs{};
    bs.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
                          SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
    bs.enable_blend = true;
    bs.enable_color_write_mask = false;
    *ok = true;
    switch (mode) {
        case TVP_GODOT_GPU_BLEND_ALPHA:
            // dst = src*srcA + dst*(1-srcA); alpha stays dstA
            bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
            bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            break;
        case TVP_GODOT_GPU_BLEND_ALPHA_D:
            // AlphaBlend_d: keep dest alpha; rgb blend as alpha
            bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
            bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            break;
        case TVP_GODOT_GPU_BLEND_COPY_COLOR:
        case TVP_GODOT_GPU_BLEND_COPY_RGBA:
            bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
            bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            break;
        case TVP_GODOT_GPU_BLEND_PS_SCREEN:
            // 1-(1-src)*(1-dst) => dst + src - src*dst
            bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
            bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
            bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            break;
        case TVP_GODOT_GPU_BLEND_PS_ADD:
            bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
            bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            break;
        case TVP_GODOT_GPU_BLEND_PS_SUBTRACT:
            bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.color_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
            bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            break;
        case TVP_GODOT_GPU_BLEND_PS_MULTIPLY:
            bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
            bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
            bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
            break;
        default:
            *ok = false;
            return bs;
    }
    return bs;
}

}  // namespace

int BytesPerPixel(TVPTextureFormat::e format) {
    switch (format) {
        case TVPTextureFormat::Gray: return 1;
        case TVPTextureFormat::RGB: return 3;
        case TVPTextureFormat::RGBA: return 4;
        default: return 4;
    }
}

iTVPRenderManager *TVPGetSoftwareRenderManager();

// --- SdlGpuTexture2D -----------------------------------------------------

SdlGpuTexture2D::SdlGpuTexture2D(const void *pixel, int pitch, unsigned int w,
                                 unsigned int h, TVPTextureFormat::e format,
                                 int create_flags)
    : iTVPTexture2D(static_cast<tjs_int>(w), static_cast<tjs_int>(h)),
      format_(format),
      pitch_(pitch > 0 ? pitch : static_cast<int>(w) * BytesPerPixel(format)) {
    (void)create_flags;
    pixels_.resize(static_cast<size_t>(pitch_) * h);
    if (pixel != nullptr) {
        const int src_pitch = pitch > 0 ? pitch : pitch_;
        const auto *src = static_cast<const uint8_t *>(pixel);
        for (unsigned int y = 0; y < h; ++y) {
            std::memcpy(pixels_.data() + static_cast<size_t>(y) * pitch_,
                        src + static_cast<size_t>(y) * src_pitch,
                        static_cast<size_t>(std::min(pitch_, src_pitch)));
        }
    }
    cpu_dirty_ = true;
    gpu_dirty_ = false;
}

SdlGpuTexture2D::~SdlGpuTexture2D() { ReleaseGpuTexture(); }

void SdlGpuTexture2D::ReleaseGpuTexture() {
    if (gpu_tex_ != nullptr) {
        TVPQueueSdlGpuTextureRelease(gpu_tex_);
        gpu_tex_ = nullptr;
    }
}

void SdlGpuTexture2D::EnsureCpuStorage() {
    const size_t required = static_cast<size_t>(pitch_) * Height;
    if (pixels_.size() != required) {
        pixels_.assign(required, 0);
    }
}

void SdlGpuTexture2D::DiscardCpuStorage() {
    if (pixels_.empty()) return;
    std::vector<uint8_t>().swap(pixels_);
}

void SdlGpuTexture2D::SetOpacityKnown(bool opaque) {
    opacity_known_ = true;
    opaque_ = opaque;
}

bool SdlGpuTexture2D::EnsureGpuTexture() {
    SDL_GPUDevice *dev = TVPGetSdlGpuDevice();
    if (dev == nullptr) return false;
    if (gpu_tex_ != nullptr) return true;
    if (format_ != TVPTextureFormat::RGBA) return false;
    SDL_GPUTextureCreateInfo tex_info{};
    tex_info.type = SDL_GPU_TEXTURETYPE_2D;
    tex_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tex_info.width = static_cast<uint32_t>(Width);
    tex_info.height = static_cast<uint32_t>(Height);
    tex_info.layer_count_or_depth = 1;
    tex_info.num_levels = 1;
    tex_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER |
                     SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    SDL_GPUTexture *tex = SDL_CreateGPUTexture(dev, &tex_info);
    if (tex == nullptr) {
        return false;
    }
    gpu_tex_ = tex;
    gpu_dirty_ = false;
    return true;
}

void SdlGpuTexture2D::UploadCpuToGpu() {
    if (!cpu_dirty_) return;
    if (format_ != TVPTextureFormat::RGBA || pixels_.empty()) return;
    if (!EnsureGpuTexture()) return;
    SDL_GPUDevice *dev = TVPGetSdlGpuDevice();
    if (dev == nullptr) return;

    SDL_GPUCommandBuffer *cmd = TVPGetSdlGpuFrameCommandBuffer();
    if (cmd == nullptr) return;
    SDL_GPUCopyPass *cp = TVPGetSdlGpuFrameCopyPass();
    if (cp == nullptr) return;

    const uint32_t w = static_cast<uint32_t>(Width);
    const uint32_t h = static_cast<uint32_t>(Height);
    const uint32_t tight_pitch = w * 4u;
    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tb_info.size = static_cast<uint32_t>(pixels_.size());
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tb_info);
    if (tb == nullptr) return;
    void *mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
    if (mapped != nullptr) {
        if (pitch_ == static_cast<int>(tight_pitch)) {
            std::memcpy(mapped, pixels_.data(), pixels_.size());
        } else {
            auto *dst = static_cast<uint8_t *>(mapped);
            for (uint32_t y = 0; y < h; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * tight_pitch,
                            pixels_.data() + static_cast<size_t>(y) * pitch_,
                            tight_pitch);
            }
        }
    }
    SDL_UnmapGPUTransferBuffer(dev, tb);

    SDL_GPUTextureTransferInfo src_info{};
    src_info.transfer_buffer = tb;
    src_info.offset = 0;
    src_info.pixels_per_row = w;
    src_info.rows_per_layer = h;
    SDL_GPUTextureRegion dst_region{};
    dst_region.texture = gpu_tex_;
    dst_region.mip_level = 0;
    dst_region.layer = 0;
    dst_region.x = 0;
    dst_region.y = 0;
    dst_region.z = 0;
    dst_region.w = w;
    dst_region.h = h;
    dst_region.d = 1;
    SDL_UploadToGPUTexture(cp, &src_info, &dst_region, false);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    cpu_dirty_ = false;
}

bool SdlGpuTexture2D::EnsureCpuReadable() {
    if (cpu_dirty_) {
        EnsureCpuStorage();
        return true;
    }
    if (gpu_tex_ == nullptr) {
        EnsureCpuStorage();
        return true;
    }
    if (pixels_.size() == static_cast<size_t>(pitch_) * Height &&
        !gpu_dirty_) {
        return true;
    }
    // Synchronous readback: submit pending frame commands, download, wait.
    SDL_GPUDevice *dev = TVPGetSdlGpuDevice();
    if (dev == nullptr) return false;
    TVPSubmitSdlGpuFrameAndWait();

    const uint32_t w = static_cast<uint32_t>(Width);
    const uint32_t h = static_cast<uint32_t>(Height);
    const uint32_t tight_pitch = w * 4u;
    SDL_GPUTransferBufferCreateInfo tb_info{};
    tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tb_info.size = tight_pitch * h;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tb_info);
    if (tb == nullptr) return false;
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src_region{};
    src_region.texture = gpu_tex_;
    src_region.mip_level = 0;
    src_region.layer = 0;
    src_region.x = 0;
    src_region.y = 0;
    src_region.z = 0;
    src_region.w = w;
    src_region.h = h;
    src_region.d = 1;
    SDL_GPUTextureTransferInfo dst_info{};
    dst_info.transfer_buffer = tb;
    dst_info.offset = 0;
    dst_info.pixels_per_row = w;
    dst_info.rows_per_layer = h;
    SDL_DownloadFromGPUTexture(cp, &src_region, &dst_info);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_WaitForGPUIdle(dev);

    EnsureCpuStorage();
    const void *mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
    if (mapped != nullptr) {
        const auto *src = static_cast<const uint8_t *>(mapped);
        if (pitch_ == static_cast<int>(tight_pitch)) {
            std::memcpy(pixels_.data(), src, pixels_.size());
        } else {
            auto *dst = pixels_.data();
            for (uint32_t y = 0; y < h; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * pitch_,
                            src + static_cast<size_t>(y) * tight_pitch,
                            tight_pitch);
            }
        }
    }
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    gpu_dirty_ = false;
    return true;
}

const void *SdlGpuTexture2D::GetScanLineForRead(tjs_uint l) {
    if (static_cast<tjs_uint>(Height) <= l) return nullptr;
    if (gpu_dirty_) EnsureCpuReadable();
    if (pixels_.empty()) EnsureCpuStorage();
    return pixels_.data() + static_cast<size_t>(l) * pitch_;
}

void *SdlGpuTexture2D::GetScanLineForWrite(tjs_uint l) {
    if (static_cast<tjs_uint>(Height) <= l) return nullptr;
    // Any pending GPU draw to this texture must be flushed before CPU writes
    // so the CPU path becomes the new source of truth.
    if (gpu_dirty_) {
        TVPSubmitSdlGpuFrameAndWait();
        gpu_dirty_ = false;
    }
    if (pixels_.empty()) EnsureCpuStorage();
    cpu_dirty_ = true;
    return pixels_.data() + static_cast<size_t>(l) * pitch_;
}

uint32_t SdlGpuTexture2D::GetPoint(int x, int y) {
    if (x < 0 || y < 0 || x >= Width || y >= Height) return 0;
    if (gpu_dirty_) EnsureCpuReadable();
    if (pixels_.empty()) EnsureCpuStorage();
    const uint8_t *p = pixels_.data() + static_cast<size_t>(y) * pitch_ +
                       static_cast<size_t>(x) * 4u;
    return (static_cast<uint32_t>(p[0])) | (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

void SdlGpuTexture2D::SetPoint(int x, int y, uint32_t clr) {
    if (x < 0 || y < 0 || x >= Width || y >= Height) return;
    if (gpu_dirty_) {
        TVPSubmitSdlGpuFrameAndWait();
        gpu_dirty_ = false;
    }
    if (pixels_.empty()) EnsureCpuStorage();
    uint8_t *p = pixels_.data() + static_cast<size_t>(y) * pitch_ +
                 static_cast<size_t>(x) * 4u;
    p[0] = static_cast<uint8_t>(clr);
    p[1] = static_cast<uint8_t>(clr >> 8u);
    p[2] = static_cast<uint8_t>(clr >> 16u);
    p[3] = static_cast<uint8_t>(clr >> 24u);
    cpu_dirty_ = true;
    gpu_dirty_ = false;
}

void SdlGpuTexture2D::Update(const void *pixel, TVPTextureFormat::e format,
                             int pitch, const tTVPRect &rc) {
    if (pixel == nullptr) return;
    if (format != TVPTextureFormat::RGBA &&
        format != TVPTextureFormat::Gray) {
        return;
    }
    const int left = std::max(0, rc.left);
    const int top = std::max(0, rc.top);
    const int right = std::min(Width, rc.right);
    const int bottom = std::min(Height, rc.bottom);
    if (left >= right || top >= bottom) return;
    if (gpu_dirty_) {
        TVPSubmitSdlGpuFrameAndWait();
        gpu_dirty_ = false;
    }
    if (pixels_.empty()) EnsureCpuStorage();
    const int src_pitch = pitch > 0 ? pitch : pitch_;
    const auto *src = static_cast<const uint8_t *>(pixel);
    if (format == TVPTextureFormat::Gray) {
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                pixels_[static_cast<size_t>(y) * pitch_ + x] =
                    src[static_cast<size_t>(y - top) * src_pitch +
                        (x - left)];
            }
        }
    } else {
        const int width = right - left;
        for (int y = top; y < bottom; ++y) {
            std::memcpy(pixels_.data() + static_cast<size_t>(y) * pitch_ +
                            static_cast<size_t>(left) * 4u,
                        src + static_cast<size_t>(y - top) * src_pitch +
                            static_cast<size_t>(left) * 4u,
                        static_cast<size_t>(width) * 4u);
        }
    }
    cpu_dirty_ = true;
    gpu_dirty_ = false;
}

void SdlGpuTexture2D::SetSize(unsigned int w, unsigned int h) {
    if (w == 0 || h == 0) return;
    if (static_cast<unsigned int>(Width) == w &&
        static_cast<unsigned int>(Height) == h) {
        return;
    }
    ReleaseGpuTexture();
    Width = static_cast<tjs_int>(w);
    Height = static_cast<tjs_int>(h);
    pitch_ = static_cast<int>(w) * BytesPerPixel(format_);
    EnsureCpuStorage();
    cpu_dirty_ = true;
    gpu_dirty_ = false;
}

// --- pipeline/resource cache ----------------------------------------------

namespace {

SDL_GPUBlendOp SDLGpuBlendOpFor(int func) {
    // Maps krkrz GL blend func ids used by the software delegate? Not needed:
    // we derive blend ops directly from our own mode table.
    return SDL_GPU_BLENDOP_ADD;
}

SDL_GPUShader *CreateShader(SDL_GPUDevice *dev, SDL_GPUShaderStage stage,
                            const unsigned char *code, int size,
                            int num_samplers, int num_uniform_buffers) {
    SDL_GPUShaderCreateInfo info{};
    info.code_size = static_cast<size_t>(size);
    info.code = code;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    info.stage = stage;
    info.num_samplers = static_cast<Uint32>(num_samplers);
    info.num_storage_textures = 0;
    info.num_storage_buffers = 0;
    info.num_uniform_buffers = static_cast<Uint32>(num_uniform_buffers);
    return SDL_CreateGPUShader(dev, &info);
}

GpuPipelines *EnsurePipelines() {
    SDL_GPUDevice *dev = TVPGetSdlGpuDevice();
    if (dev == nullptr) return nullptr;
    if (g_pipelines != nullptr) return g_pipelines;

    auto *p = new GpuPipelines();
    p->quad_vs = CreateShader(dev, SDL_GPU_SHADERSTAGE_VERTEX,
                              quad_vert_spv, quad_vert_spv_size, 0, 1);
    p->quad_fs = CreateShader(dev, SDL_GPU_SHADERSTAGE_FRAGMENT,
                              quad_frag_spv, quad_frag_spv_size, 1, 0);
    p->fill_vs = CreateShader(dev, SDL_GPU_SHADERSTAGE_VERTEX,
                              fill_vert_spv, fill_vert_spv_size, 0, 1);
    p->fill_fs = CreateShader(dev, SDL_GPU_SHADERSTAGE_FRAGMENT,
                              fill_frag_spv, fill_frag_spv_size, 0, 1);
    p->blend_d_fs = CreateShader(dev, SDL_GPU_SHADERSTAGE_FRAGMENT,
                                blend_d_frag_spv, blend_d_frag_spv_size, 2, 1);
    if (p->quad_vs == nullptr || p->quad_fs == nullptr ||
        p->fill_vs == nullptr || p->fill_fs == nullptr ||
        p->blend_d_fs == nullptr) {
        delete p;
        return nullptr;
    }

    SDL_GPUSamplerCreateInfo sampler_info{};
    sampler_info.min_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    p->sampler = SDL_CreateGPUSampler(dev, &sampler_info);

    SDL_GPUBufferCreateInfo vb_info{};
    vb_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vb_info.size = sizeof(kQuadVerts);
    p->quad_vb = SDL_CreateGPUBuffer(dev, &vb_info);
    SDL_GPUBufferCreateInfo ib_info{};
    ib_info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    ib_info.size = sizeof(kQuadIndices);
    p->quad_ib = SDL_CreateGPUBuffer(dev, &ib_info);
    if (p->sampler == nullptr || p->quad_vb == nullptr ||
        p->quad_ib == nullptr) {
        delete p;
        return nullptr;
    }

    // Upload static geometry once.
    SDL_GPUTransferBufferCreateInfo vtb_info{};
    vtb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    vtb_info.size = sizeof(kQuadVerts);
    SDL_GPUTransferBuffer *vtb = SDL_CreateGPUTransferBuffer(dev, &vtb_info);
    SDL_GPUTransferBufferCreateInfo itb_info{};
    itb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    itb_info.size = sizeof(kQuadIndices);
    SDL_GPUTransferBuffer *itb = SDL_CreateGPUTransferBuffer(dev, &itb_info);
    if (vtb != nullptr && itb != nullptr) {
        std::memcpy(SDL_MapGPUTransferBuffer(dev, vtb, false), kQuadVerts,
                    sizeof(kQuadVerts));
        SDL_UnmapGPUTransferBuffer(dev, vtb);
        std::memcpy(SDL_MapGPUTransferBuffer(dev, itb, false), kQuadIndices,
                    sizeof(kQuadIndices));
        SDL_UnmapGPUTransferBuffer(dev, itb);
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation vloc{.transfer_buffer = vtb, .offset = 0};
        SDL_GPUBufferRegion vreg{.buffer = p->quad_vb, .offset = 0,
                                 .size = sizeof(kQuadVerts)};
        SDL_UploadToGPUBuffer(cp, &vloc, &vreg, false);
        SDL_GPUTransferBufferLocation iloc{.transfer_buffer = itb, .offset = 0};
        SDL_GPUBufferRegion ireg{.buffer = p->quad_ib, .offset = 0,
                                 .size = sizeof(kQuadIndices)};
        SDL_UploadToGPUBuffer(cp, &iloc, &ireg, false);
        SDL_EndGPUCopyPass(cp);
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(dev, vtb);
        SDL_ReleaseGPUTransferBuffer(dev, itb);
    }

    g_pipelines = p;
    return p;
}

SDL_GPUGraphicsPipeline *GetRectPipeline(GpuPipelines *p, uint32_t mode,
                                         bool *ok) {
    bool blend_ok = false;
    SDL_GPUColorTargetBlendState bs = BlendStateFor(mode, &blend_ok);
    if (!blend_ok) {
        *ok = false;
        return nullptr;
    }
    auto it = p->rect_pipes.find(mode);
    if (it != p->rect_pipes.end()) {
        *ok = true;
        return it->second;
    }

    SDL_GPUVertexBufferDescription vdesc[1] = {{
        .slot = 0,
        .pitch = sizeof(QuadVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
        .instance_step_rate = 0,
    }};
    SDL_GPUVertexAttribute vattr[2] = {
        {.location = 0, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 0},
        {.location = 1, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 8},
    };
    SDL_GPUColorTargetDescription color_target{};
    color_target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    color_target.blend_state = bs;

    SDL_GPUGraphicsPipelineCreateInfo pipe_info{};
    pipe_info.vertex_shader = p->quad_vs;
    pipe_info.fragment_shader = p->quad_fs;
    pipe_info.vertex_input_state.vertex_buffer_descriptions = vdesc;
    pipe_info.vertex_input_state.num_vertex_buffers = 1;
    pipe_info.vertex_input_state.vertex_attributes = vattr;
    pipe_info.vertex_input_state.num_vertex_attributes = 2;
    pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipe_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipe_info.rasterizer_state.enable_depth_bias = false;
    pipe_info.rasterizer_state.enable_depth_clip = false;
    pipe_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pipe_info.multisample_state.sample_mask = 0;
    pipe_info.multisample_state.enable_mask = false;
    pipe_info.depth_stencil_state.enable_depth_test = false;
    pipe_info.depth_stencil_state.enable_depth_write = false;
    pipe_info.depth_stencil_state.enable_stencil_test = false;
    pipe_info.target_info.color_target_descriptions = &color_target;
    pipe_info.target_info.num_color_targets = 1;
    pipe_info.target_info.has_depth_stencil_target = false;

    SDL_GPUGraphicsPipeline *pipe =
        SDL_CreateGPUGraphicsPipeline(TVPGetSdlGpuDevice(), &pipe_info);
    if (pipe == nullptr) {
        *ok = false;
        return nullptr;
    }
    p->rect_pipes[mode] = pipe;
    *ok = true;
    return pipe;
}

SDL_GPUGraphicsPipeline *GetFillPipeline(GpuPipelines *p, bool *ok) {
    auto it = p->fill_pipes.find(0);
    if (it != p->fill_pipes.end()) {
        *ok = true;
        return it->second;
    }
    SDL_GPUVertexBufferDescription vdesc[1] = {{
        .slot = 0,
        .pitch = sizeof(QuadVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
        .instance_step_rate = 0,
    }};
    SDL_GPUVertexAttribute vattr[1] = {{
        .location = 0, .buffer_slot = 0,
        .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 0,
    }};
    SDL_GPUColorTargetDescription color_target{};
    color_target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    color_target.blend_state = BlendStateFor(TVP_GODOT_GPU_BLEND_ALPHA, ok);

    SDL_GPUGraphicsPipelineCreateInfo pipe_info{};
    pipe_info.vertex_shader = p->fill_vs;
    pipe_info.fragment_shader = p->fill_fs;
    pipe_info.vertex_input_state.vertex_buffer_descriptions = vdesc;
    pipe_info.vertex_input_state.num_vertex_buffers = 1;
    pipe_info.vertex_input_state.vertex_attributes = vattr;
    pipe_info.vertex_input_state.num_vertex_attributes = 1;
    pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipe_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipe_info.rasterizer_state.enable_depth_bias = false;
    pipe_info.rasterizer_state.enable_depth_clip = false;
    pipe_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pipe_info.multisample_state.sample_mask = 0;
    pipe_info.multisample_state.enable_mask = false;
    pipe_info.depth_stencil_state.enable_depth_test = false;
    pipe_info.depth_stencil_state.enable_depth_write = false;
    pipe_info.depth_stencil_state.enable_stencil_test = false;
    pipe_info.target_info.color_target_descriptions = &color_target;
    pipe_info.target_info.num_color_targets = 1;
    pipe_info.target_info.has_depth_stencil_target = false;

    SDL_GPUGraphicsPipeline *pipe =
        SDL_CreateGPUGraphicsPipeline(TVPGetSdlGpuDevice(), &pipe_info);
    if (pipe == nullptr) {
        *ok = false;
        return nullptr;
    }
    p->fill_pipes[0] = pipe;
    *ok = true;
    return pipe;
}

// Creates the pipeline used by destination-read (_d) blend modes. The
// fragment shader samples both the source and the destination (via a scratch
// copy) and computes the final color entirely in the shader, so the color
// target blend state is NONE.
SDL_GPUGraphicsPipeline *GetBlendDPipeline(GpuPipelines *p, bool *ok) {
    if (p->blend_d_pipe != nullptr) {
        *ok = true;
        return p->blend_d_pipe;
    }
    SDL_GPUVertexBufferDescription vdesc[1] = {{
        .slot = 0,
        .pitch = sizeof(QuadVertex),
        .input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
        .instance_step_rate = 0,
    }};
    SDL_GPUVertexAttribute vattr[2] = {
        {.location = 0, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 0},
        {.location = 1, .buffer_slot = 0,
         .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = 8},
    };
    SDL_GPUColorTargetDescription color_target{};
    color_target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    color_target.blend_state.enable_blend = false;
    color_target.blend_state.enable_color_write_mask = false;

    SDL_GPUGraphicsPipelineCreateInfo pipe_info{};
    pipe_info.vertex_shader = p->quad_vs;
    pipe_info.fragment_shader = p->blend_d_fs;
    pipe_info.vertex_input_state.vertex_buffer_descriptions = vdesc;
    pipe_info.vertex_input_state.num_vertex_buffers = 1;
    pipe_info.vertex_input_state.vertex_attributes = vattr;
    pipe_info.vertex_input_state.num_vertex_attributes = 2;
    pipe_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipe_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipe_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipe_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pipe_info.rasterizer_state.enable_depth_bias = false;
    pipe_info.rasterizer_state.enable_depth_clip = false;
    pipe_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    pipe_info.multisample_state.sample_mask = 0;
    pipe_info.multisample_state.enable_mask = false;
    pipe_info.depth_stencil_state.enable_depth_test = false;
    pipe_info.depth_stencil_state.enable_depth_write = false;
    pipe_info.depth_stencil_state.enable_stencil_test = false;
    pipe_info.target_info.color_target_descriptions = &color_target;
    pipe_info.target_info.num_color_targets = 1;
    pipe_info.target_info.has_depth_stencil_target = false;

    SDL_GPUGraphicsPipeline *pipe =
        SDL_CreateGPUGraphicsPipeline(TVPGetSdlGpuDevice(), &pipe_info);
    if (pipe == nullptr) {
        *ok = false;
        return nullptr;
    }
    p->blend_d_pipe = pipe;
    *ok = true;
    return pipe;
}

// Ensures the scratch texture used to hold a copy of the destination for _d
// blend modes is sized to at least (w, h) and returns it.
SDL_GPUTexture *EnsureScratch(GpuPipelines *p, int w, int h) {
    SDL_GPUDevice *dev = TVPGetSdlGpuDevice();
    if (dev == nullptr) return nullptr;
    if (p->scratch != nullptr && p->scratch_w >= w && p->scratch_h >= h) {
        return p->scratch;
    }
    if (p->scratch != nullptr) {
        SDL_ReleaseGPUTexture(dev, p->scratch);
        p->scratch = nullptr;
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = static_cast<Uint32>(std::max(w, 1));
    info.height = static_cast<Uint32>(std::max(h, 1));
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    p->scratch = SDL_CreateGPUTexture(dev, &info);
    if (p->scratch != nullptr) {
        p->scratch_w = std::max(w, 1);
        p->scratch_h = std::max(h, 1);
    }
    return p->scratch;
}

void ReleasePipelines() {
    SDL_GPUDevice *dev = TVPGetSdlGpuDevice();
    if (g_pipelines == nullptr) return;
    GpuPipelines *p = g_pipelines;
    g_pipelines = nullptr;
    if (dev != nullptr) {
        for (auto &kv : p->rect_pipes) SDL_ReleaseGPUGraphicsPipeline(dev, kv.second);
        for (auto &kv : p->fill_pipes) SDL_ReleaseGPUGraphicsPipeline(dev, kv.second);
        if (p->blend_d_pipe) SDL_ReleaseGPUGraphicsPipeline(dev, p->blend_d_pipe);
        if (p->quad_vs) SDL_ReleaseGPUShader(dev, p->quad_vs);
        if (p->quad_fs) SDL_ReleaseGPUShader(dev, p->quad_fs);
        if (p->fill_vs) SDL_ReleaseGPUShader(dev, p->fill_vs);
        if (p->fill_fs) SDL_ReleaseGPUShader(dev, p->fill_fs);
        if (p->blend_d_fs) SDL_ReleaseGPUShader(dev, p->blend_d_fs);
        if (p->sampler) SDL_ReleaseGPUSampler(dev, p->sampler);
        if (p->quad_vb) SDL_ReleaseGPUBuffer(dev, p->quad_vb);
        if (p->quad_ib) SDL_ReleaseGPUBuffer(dev, p->quad_ib);
        if (p->scratch) SDL_ReleaseGPUTexture(dev, p->scratch);
    }
    delete p;
}

}  // namespace

// --- SdlGpuRenderManager --------------------------------------------------

namespace {
// GPU copy of a same-size rect from src into dst (used by clone paths).
bool CopyGpuFrom(SdlGpuTexture2D *dst, SdlGpuTexture2D *src,
                 const tTVPRect &dst_rc, const tTVPRect &src_rc) {
    if (dst == nullptr || src == nullptr) return false;
    if (!TVPIsSdlGpuActive()) return false;
    if (dst_rc.is_empty() || src_rc.is_empty()) return true;
    if (!dst->EnsureGpuTexture() || !src->EnsureGpuTexture()) return false;
    src->UploadCpuToGpu();
    SDL_GPUCommandBuffer *cmd = TVPGetSdlGpuFrameCommandBuffer();
    SDL_GPUCopyPass *cp = TVPGetSdlGpuFrameCopyPass();
    if (cmd == nullptr || cp == nullptr) return false;
    SDL_GPUTextureLocation src_loc{};
    src_loc.texture = src->GpuTexture();
    src_loc.mip_level = 0;
    src_loc.layer = 0;
    src_loc.x = static_cast<Uint32>(src_rc.left);
    src_loc.y = static_cast<Uint32>(src_rc.top);
    src_loc.z = 0;
    SDL_GPUTextureLocation dst_loc{};
    dst_loc.texture = dst->GpuTexture();
    dst_loc.mip_level = 0;
    dst_loc.layer = 0;
    dst_loc.x = static_cast<Uint32>(dst_rc.left);
    dst_loc.y = static_cast<Uint32>(dst_rc.top);
    dst_loc.z = 0;
    const Uint32 w = static_cast<Uint32>(std::min(src_rc.get_width(),
                                                  dst_rc.get_width()));
    const Uint32 h = static_cast<Uint32>(std::min(src_rc.get_height(),
                                                  dst_rc.get_height()));
    SDL_CopyGPUTextureToTexture(cp, &src_loc, &dst_loc, w, h, 1, false);
    dst->MarkGpuDirty();
    return true;
}
}  // namespace

iTVPRenderManager *SdlGpuRenderManager::SoftwareDelegate() {
    if (software_delegate_ == nullptr) {
        software_delegate_ = TVPGetSoftwareRenderManager();
    }
    return software_delegate_;
}

iTVPTexture2D *SdlGpuRenderManager::CreateTexture2D(
    const void *pixel, int pitch, unsigned int w, unsigned int h,
    TVPTextureFormat::e format, int flags) {
    auto *texture = new SdlGpuTexture2D(pixel, pitch, w, h, format, flags);
    vmem_size_ += static_cast<uint64_t>(texture->GetPitch()) * h;
    return texture;
}

iTVPTexture2D *SdlGpuRenderManager::CreateTexture2D(tTVPBitmap *bmp) {
    if (bmp == nullptr) {
        return CreateTexture2D(nullptr, 0, 1, 1, TVPTextureFormat::RGBA);
    }
    return CreateTexture2D(bmp->GetScanLine(0), bmp->GetPitch(),
                           bmp->GetWidth(), bmp->GetHeight(),
                           bmp->GetBPP() == 8 ? TVPTextureFormat::Gray
                                              : TVPTextureFormat::RGBA);
}

iTVPTexture2D *SdlGpuRenderManager::CreateTexture2D(TJS::tTJSBinaryStream *) {
    return CreateTexture2D(nullptr, 0, 1, 1, TVPTextureFormat::RGBA);
}

iTVPTexture2D *SdlGpuRenderManager::CreateTexture2D(
    unsigned int neww, unsigned int newh, iTVPTexture2D *tex) {
    auto *ret = new SdlGpuTexture2D(
        nullptr, 0, neww, newh,
        tex != nullptr ? tex->GetFormat() : TVPTextureFormat::RGBA);
    if (tex != nullptr) {
        const tTVPRect copy_rc(0, 0,
                               std::min<tjs_int>(neww, tex->GetWidth()),
                               std::min<tjs_int>(newh, tex->GetHeight()));
        if (!copy_rc.is_empty()) {
            auto *gpu_src = dynamic_cast<SdlGpuTexture2D *>(tex);
            if (gpu_src != nullptr && gpu_src->EnsureGpuTexture()) {
                gpu_src->UploadCpuToGpu();
                if (ret->EnsureGpuTexture() &&
                    CopyGpuFrom(ret, gpu_src, copy_rc, copy_rc)) {
                    return ret;
                }
            }
            const void *src_pixels = tex->GetScanLineForRead(0);
            if (src_pixels != nullptr) {
                ret->Update(src_pixels, tex->GetFormat(), tex->GetPitch(),
                            copy_rc);
            }
        }
    }
    return ret;
}

iTVPRenderMethod *SdlGpuRenderManager::GetRenderMethod(const char *name,
                                                        uint32_t *hint) {
    uint32_t hash = 0;
    if (hint != nullptr && *hint != 0) {
        hash = *hint;
    } else {
        hash = tTJSHashFunc<tjs_nchar *>::Make(name);
        if (hint != nullptr) *hint = hash;
    }
    auto it = method_wrappers_.find(hash);
    if (it != method_wrappers_.end()) return it->second;
    iTVPRenderMethod *delegate = SoftwareDelegate()->GetRenderMethod(name, &hash);
    auto *wrapper = new SDLRenderMethod(delegate);
    wrapper->SetName(name);
    method_wrappers_[hash] = wrapper;
    return wrapper;
}

bool SdlGpuRenderManager::GetRenderStat(unsigned int &drawCount,
                                         uint64_t &vmemsize) {
    unsigned int delegate_draws = 0;
    uint64_t delegate_vmem = 0;
    const bool ok = SoftwareDelegate()->GetRenderStat(delegate_draws, delegate_vmem);
    drawCount = draw_count_ + delegate_draws;
    draw_count_ = 0;
    vmemsize = std::max(vmem_size_, delegate_vmem);
    return ok;
}

bool SdlGpuRenderManager::GetTextureStat(iTVPTexture2D *texture,
                                          uint64_t &vmemsize) {
    return SoftwareDelegate()->GetTextureStat(texture, vmemsize);
}

int SdlGpuRenderManager::EnumParameterID(const char *name) {
    const int id = SoftwareDelegate()->EnumParameterID(name);
    if (name != nullptr && std::strcmp(name, "StretchType") == 0) {
        stretch_parameter_id_ = id;
    }
    return id;
}

void SdlGpuRenderManager::SetParameterUInt(int id, unsigned int Value) {
    SoftwareDelegate()->SetParameterUInt(id, Value);
}

void SdlGpuRenderManager::SetParameterInt(int id, int Value) {
    if (id >= 0 && id == stretch_parameter_id_) stretch_type_ = Value;
    SoftwareDelegate()->SetParameterInt(id, Value);
}

void SdlGpuRenderManager::SetParameterPtr(int id, const void *Value) {
    SoftwareDelegate()->SetParameterPtr(id, Value);
}

void SdlGpuRenderManager::SetParameterFloat(int id, float Value) {
    SoftwareDelegate()->SetParameterFloat(id, Value);
}

namespace {
uint32_t ModeForMethodName(const std::string &name) {
    if (name == "Copy" || name == "CopyOpaqueImage") return TVP_GODOT_GPU_BLEND_COPY_RGBA;
    if (name == "CopyColor") return TVP_GODOT_GPU_BLEND_COPY_COLOR;
    if (name == "AlphaBlend") return TVP_GODOT_GPU_BLEND_ALPHA;
    if (name == "PsScreenBlend") return TVP_GODOT_GPU_BLEND_PS_SCREEN;
    if (name == "PsAddBlend") return TVP_GODOT_GPU_BLEND_PS_ADD;
    if (name == "PsSubBlend") return TVP_GODOT_GPU_BLEND_PS_SUBTRACT;
    if (name == "PsMulBlend") return TVP_GODOT_GPU_BLEND_PS_MULTIPLY;
    return 0;
}

bool IsDestinationReadMode(uint32_t mode) {
    switch (mode) {
        case TVP_GODOT_GPU_BLEND_ALPHA_D:
            // Destination-read (_d) modes read the destination inside the
            // fragment shader (dst.rgb = mix(dst, src, srcA/outA)). The
            // fixed-function blend state cannot express this, and a shader
            // reading a copy of the destination diverges from the software
            // delegate once GPU and CPU dst content split across incremental
            // composites. Fall back to the software delegate so results stay
            // pixel-identical; a full GPU-only composite pipeline is the
            // follow-up.
            return true;
        default:
            return false;
    }
}
}  // namespace

// Draws src (full GPU texture) into dst rect rctar with the given blend mode.
// Returns true if fully handled on the GPU path.
bool SdlGpuRenderManager::DrawRect(SdlGpuTexture2D *dst,
                                   const tTVPRect &rctar,
                                   const char *method_name, uint32_t mode,
                                   int opacity, uint32_t color,
                                   SDL_GPUTexture *src,
                                   const tTVPRect &src_rc, int src_w,
                                   int src_h) {
    if (dst == nullptr || src == nullptr) return false;
    if (!TVPIsSdlGpuActive()) return false;
    if (rctar.is_empty()) return true;
    GpuPipelines *p = EnsurePipelines();
    if (p == nullptr) return false;
    // If the destination has fresh CPU pixels that were never uploaded to the
    // GPU texture, upload them first so a LOAD render pass starts from the
    // current CPU content (the engine composites incrementally: a software
    // Fill/clear on the CPU must precede GPU layer blits).
    dst->UploadCpuToGpu();
    if (!dst->EnsureGpuTexture()) return false;
    if (mode == 0) mode = ModeForMethodName(method_name != nullptr ? method_name : "");
    if (mode == 0) return false;

    bool ok = false;
    SDL_GPUGraphicsPipeline *pipe = GetRectPipeline(p, mode, &ok);
    if (!ok || pipe == nullptr) return false;

    SDL_GPUCommandBuffer *cmd = TVPGetSdlGpuFrameCommandBuffer();
    if (cmd == nullptr) return false;
    TVPEnsureSdlGpuRenderPassReady();

    // Begin a render pass targeting dst. Each OperateRect gets its own pass
    // because targets change arbitrarily between calls.
    SDL_GPUColorTargetInfo tgt{};
    tgt.texture = dst->GpuTexture();
    tgt.mip_level = 0;
    tgt.layer_or_depth_plane = 0;
    tgt.clear_color = (SDL_FColor){0, 0, 0, 1};
    tgt.load_op = SDL_GPU_LOADOP_LOAD;
    tgt.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &tgt, 1, nullptr);
    if (rp == nullptr) return false;

    SDL_BindGPUGraphicsPipeline(rp, pipe);
    SDL_GPUBufferBinding vbind{};
    vbind.buffer = p->quad_vb;
    vbind.offset = 0;
    SDL_BindGPUVertexBuffers(rp, 0, &vbind, 1);
    SDL_GPUBufferBinding ibind{};
    ibind.buffer = p->quad_ib;
    ibind.offset = 0;
    SDL_BindGPUIndexBuffer(rp, &ibind, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    // Sampler slot 0: source texture.
    SDL_GPUTextureSamplerBinding sampler_bind{};
    sampler_bind.texture = src;
    sampler_bind.sampler = p->sampler;
    SDL_BindGPUFragmentSamplers(rp, 0, &sampler_bind, 1);

    // Vertex uniform: dst rect, viewport = dst size, src uv rect.
    const float viewport_w = static_cast<float>(dst->GetWidth());
    const float viewport_h = static_cast<float>(dst->GetHeight());
    const float src_w_f = static_cast<float>(std::max(src_w, 1));
    const float src_h_f = static_cast<float>(std::max(src_h, 1));
    const float src_uv_x = static_cast<float>(src_rc.left) /
                           std::max(src_w_f, 1.0f);
    const float src_uv_y = static_cast<float>(src_rc.top) /
                           std::max(src_h_f, 1.0f);
    const float src_uv_w = static_cast<float>(src_rc.get_width()) /
                           std::max(src_w_f, 1.0f);
    const float src_uv_h = static_cast<float>(src_rc.get_height()) /
                           std::max(src_h_f, 1.0f);
    // UV rect covers the source sub-rect; flip V so top row maps to NDC top.
    const float vparams[10] = {
        static_cast<float>(rctar.left), static_cast<float>(rctar.top),
        static_cast<float>(rctar.get_width()),
        static_cast<float>(rctar.get_height()),
        viewport_w, viewport_h,
        src_uv_x, src_uv_y,
        src_uv_w, src_uv_h};
    SDL_PushGPUVertexUniformData(cmd, 0, vparams, sizeof(vparams));

    SDL_GPUViewport vp{};
    vp.x = 0;
    vp.y = 0;
    vp.w = viewport_w;
    vp.h = viewport_h;
    vp.min_depth = 0;
    vp.max_depth = 1;
    SDL_SetGPUViewport(rp, &vp);

    SDL_DrawGPUIndexedPrimitives(rp, 6, 1, 0, 0, 0);
    SDL_EndGPURenderPass(rp);

    dst->MarkGpuDirty();
    return true;
}

// Destination-read (_d) blend: the fragment shader samples the source and a
// copy of the destination and computes the blended color in-shader. The
// destination must be copied to a scratch texture first because a render pass
// color target cannot also be sampled.
//
// NOTE: not currently dispatched from OperateRect (destination-read modes
// fall back to the software delegate for pixel-identical output). This stays
// as the reference implementation for a future full-GPU composite pipeline.
bool SdlGpuRenderManager::DrawRectD(SdlGpuTexture2D *dst,
                                    const tTVPRect &rctar,
                                    const char *method_name, uint32_t mode,
                                    int opacity, uint32_t color,
                                    SDL_GPUTexture *src,
                                    const tTVPRect &src_rc, int src_w,
                                    int src_h) {
    if (dst == nullptr || src == nullptr) return false;
    if (!TVPIsSdlGpuActive()) return false;
    if (rctar.is_empty()) return true;
    GpuPipelines *p = EnsurePipelines();
    if (p == nullptr) return false;
    dst->UploadCpuToGpu();
    if (!dst->EnsureGpuTexture()) return false;
    if (mode == 0) return false;

    bool ok = false;
    SDL_GPUGraphicsPipeline *pipe = GetBlendDPipeline(p, &ok);
    if (!ok || pipe == nullptr) return false;

    SDL_GPUCommandBuffer *cmd = TVPGetSdlGpuFrameCommandBuffer();
    if (cmd == nullptr) return false;

    // Copy the destination to the scratch texture. Use a copy pass; the
    // backend ensures any open render pass is ended first.
    SDL_GPUCommandBuffer *save_cmd = cmd;
    SDL_GPUTexture *scratch =
        EnsureScratch(p, dst->GetWidth(), dst->GetHeight());
    if (scratch == nullptr) return false;
    TVPEnsureSdlGpuRenderPassReady();
    SDL_GPUCopyPass *cp = TVPGetSdlGpuFrameCopyPass();
    if (cp == nullptr) return false;
    SDL_GPUTextureLocation src_loc{};
    src_loc.texture = dst->GpuTexture();
    src_loc.mip_level = 0;
    src_loc.layer = 0;
    src_loc.x = 0;
    src_loc.y = 0;
    src_loc.z = 0;
    SDL_GPUTextureLocation dst_loc{};
    dst_loc.texture = scratch;
    dst_loc.mip_level = 0;
    dst_loc.layer = 0;
    dst_loc.x = 0;
    dst_loc.y = 0;
    dst_loc.z = 0;
    const Uint32 copy_w = static_cast<Uint32>(dst->GetWidth());
    const Uint32 copy_h = static_cast<Uint32>(dst->GetHeight());
    SDL_CopyGPUTextureToTexture(cp, &src_loc, &dst_loc, copy_w, copy_h, 1,
                                false);

    // Now the render pass to dst, sampling both src and the scratch.
    TVPEnsureSdlGpuRenderPassReady();
    SDL_GPUColorTargetInfo tgt{};
    tgt.texture = dst->GpuTexture();
    tgt.mip_level = 0;
    tgt.layer_or_depth_plane = 0;
    tgt.clear_color = (SDL_FColor){0, 0, 0, 1};
    tgt.load_op = SDL_GPU_LOADOP_LOAD;
    tgt.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(save_cmd, &tgt, 1, nullptr);
    if (rp == nullptr) return false;

    SDL_BindGPUGraphicsPipeline(rp, pipe);
    SDL_GPUBufferBinding vbind{};
    vbind.buffer = p->quad_vb;
    vbind.offset = 0;
    SDL_BindGPUVertexBuffers(rp, 0, &vbind, 1);
    SDL_GPUBufferBinding ibind{};
    ibind.buffer = p->quad_ib;
    ibind.offset = 0;
    SDL_BindGPUIndexBuffer(rp, &ibind, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    // src at binding 0, dst-scratch at binding 1.
    SDL_GPUTextureSamplerBinding sampler_binds[2] = {
        {.texture = src, .sampler = p->sampler},
        {.texture = scratch, .sampler = p->sampler},
    };
    SDL_BindGPUFragmentSamplers(rp, 0, sampler_binds, 2);

    // Vertex uniform: dst rect, viewport, src uv rect.
    const float viewport_w = static_cast<float>(dst->GetWidth());
    const float viewport_h = static_cast<float>(dst->GetHeight());
    const float src_w_f = static_cast<float>(std::max(src_w, 1));
    const float src_h_f = static_cast<float>(std::max(src_h, 1));
    const float src_uv_x = static_cast<float>(src_rc.left) / src_w_f;
    const float src_uv_y = static_cast<float>(src_rc.top) / src_h_f;
    const float src_uv_w = static_cast<float>(src_rc.get_width()) / src_w_f;
    const float src_uv_h = static_cast<float>(src_rc.get_height()) / src_h_f;
    const float vparams[10] = {
        static_cast<float>(rctar.left), static_cast<float>(rctar.top),
        static_cast<float>(rctar.get_width()),
        static_cast<float>(rctar.get_height()),
        viewport_w, viewport_h,
        src_uv_x, src_uv_y,
        src_uv_w, src_uv_h};
    SDL_PushGPUVertexUniformData(save_cmd, 0, vparams, sizeof(vparams));

    // Fragment uniform: tint color + opacity. Color is AARRGGBB; a value of 0
    // means no tint (white).
    const float a = static_cast<float>(opacity) / 255.0f;
    const uint32_t tint = color != 0 ? color : 0xffffffffu;
    const float fr = ((tint >> 16u) & 0xffu) / 255.0f;
    const float fg = ((tint >> 8u) & 0xffu) / 255.0f;
    const float fb = ((tint >> 0u) & 0xffu) / 255.0f;
    const float fparams[4] = {fr, fg, fb, a};
    SDL_PushGPUFragmentUniformData(save_cmd, 0, fparams, sizeof(fparams));

    SDL_GPUViewport vp{};
    vp.x = 0;
    vp.y = 0;
    vp.w = viewport_w;
    vp.h = viewport_h;
    vp.min_depth = 0;
    vp.max_depth = 1;
    SDL_SetGPUViewport(rp, &vp);

    SDL_DrawGPUIndexedPrimitives(rp, 6, 1, 0, 0, 0);
    SDL_EndGPURenderPass(rp);

    dst->MarkGpuDirty();
    return true;
}

void SdlGpuRenderManager::OperateRect(iTVPRenderMethod *method,
                                       iTVPTexture2D *tar,
                                       iTVPTexture2D *reftar,
                                       const tTVPRect &rctar,
                                       const tRenderTexRectArray &textures) {
    ++draw_count_;
    auto *godot_method = dynamic_cast<SDLRenderMethod *>(method);
    iTVPRenderMethod *delegate_method =
        godot_method != nullptr ? godot_method->Delegate() : method;
    const std::string method_name =
        method != nullptr ? method->GetName() : std::string();

    auto *dst = dynamic_cast<SdlGpuTexture2D *>(tar);
    SdlGpuTexture2D *src = nullptr;
    tTVPRect src_rc{};
    if (textures.size() == 1) {
        src = dynamic_cast<SdlGpuTexture2D *>(textures[0].first);
        src_rc = textures[0].second;
    }

    // FillARGB has no source texture.
    bool handled = false;
    if (dst != nullptr && TVPIsSdlGpuActive() &&
        method_name == "FillARGB") {
        // GPU fill via clear is not precise for sub-rects; route FillARGB to
        // the software delegate for now (it is rare on the hot path).
        handled = false;
    } else if (dst != nullptr && src != nullptr && dst->GpuTexture() != nullptr &&
               src->EnsureGpuTexture() && method_name != "FillARGB") {
        src->UploadCpuToGpu();
        uint32_t mode = ModeForMethodName(method_name);
        // Debug: force all ops through the opaque Copy blend to isolate
        // sampler correctness from blend semantics.
        static const bool force_copy = []() {
            const char *v = std::getenv("AETHERKIRI_SDL_GPU_FORCE_COPY");
            return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
        }();
        if (force_copy) mode = TVP_GODOT_GPU_BLEND_COPY_RGBA;
        const int opacity =
            godot_method != nullptr ? godot_method->Opacity() : 255;
        const uint32_t color =
            godot_method != nullptr ? godot_method->Color() : 0;
        // _d modes resolve to mode==0 and fall back to the software delegate
        // below (pixel-identical, see IsDestinationReadMode).
        handled = DrawRect(dst, rctar, method_name.c_str(), mode, opacity,
                           color, src->GpuTexture(), src_rc,
                           src->GetWidth(), src->GetHeight());
    }

    if (!handled) {
        SoftwareDelegate()->OperateRect(delegate_method, tar, reftar, rctar,
                                        textures);
        if (dst != nullptr) dst->MarkCpuDirty();
    }
}

void SdlGpuRenderManager::OperateTriangles(
    iTVPRenderMethod *method, int nTriangles, iTVPTexture2D *target,
    iTVPTexture2D *reftar, const tTVPRect &rcclip,
    const tTVPPointD *pttar, const tRenderTexQuadArray &textures) {
    ++draw_count_;
    auto *godot_method = dynamic_cast<SDLRenderMethod *>(method);
    SoftwareDelegate()->OperateTriangles(
        godot_method != nullptr ? godot_method->Delegate() : method,
        nTriangles, target, reftar, rcclip, pttar, textures);
    if (auto *dst = dynamic_cast<SdlGpuTexture2D *>(target)) {
        dst->MarkCpuDirty();
    }
}

void SdlGpuRenderManager::OperatePerspective(
    iTVPRenderMethod *method, int nQuads, iTVPTexture2D *target,
    iTVPTexture2D *reftar, const tTVPRect &rcclip,
    const tTVPPointD *pttar, const tRenderTexQuadArray &textures) {
    ++draw_count_;
    auto *godot_method = dynamic_cast<SDLRenderMethod *>(method);
    SoftwareDelegate()->OperatePerspective(
        godot_method != nullptr ? godot_method->Delegate() : method,
        nQuads, target, reftar, rcclip, pttar, textures);
    if (auto *dst = dynamic_cast<SdlGpuTexture2D *>(target)) {
        dst->MarkCpuDirty();
    }
}

void SdlGpuRenderManager::BeginComposite() { TVPBeginSdlGpuComposite(); }
void SdlGpuRenderManager::EndComposite() { TVPEndSdlGpuComposite(); }

// --- registration ---------------------------------------------------------

namespace {
iTVPRenderManager *CreateSdlGpuRenderManager() {
    return new SdlGpuRenderManager();
}

class SdlGpuRenderManagerAutoRegister {
public:
    SdlGpuRenderManagerAutoRegister() {
        TVPRegisterRenderManager("sdl3_gpu", CreateSdlGpuRenderManager);
    }
} sdl_gpu_render_manager_auto_register;
}  // namespace

void TVPForceRegisterSdlGpuRenderManager() {}

// Releases the cached SDL_GPU pipelines/geometry. Called when the injected
// SDL_GPUDevice is detached or the engine is destroyed (the device must still
// be alive).
void TVPReleaseSdlGpuPipelines() { ReleasePipelines(); }
