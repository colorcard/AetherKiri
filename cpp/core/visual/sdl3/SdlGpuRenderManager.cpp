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
#include <unordered_set>

#include "shaders/quad.vert.spv.h"
#include "shaders/quad.frag.spv.h"
#include "shaders/fill.vert.spv.h"
#include "shaders/fill.frag.spv.h"
#include "shaders/blend_d.frag.spv.h"
#include "shaders/blend_const_color_d.frag.spv.h"

extern unsigned char TVPOpacityOnOpacityTable[256 * 256];
extern unsigned char TVPNegativeMulTable[256 * 256];

namespace {
std::mutex g_live_texture_mutex;
std::unordered_set<SdlGpuTexture2D *> g_live_textures;

void RegisterLiveTexture(SdlGpuTexture2D *texture) {
    std::lock_guard<std::mutex> lock(g_live_texture_mutex);
    g_live_textures.insert(texture);
}

void UnregisterLiveTexture(SdlGpuTexture2D *texture) {
    std::lock_guard<std::mutex> lock(g_live_texture_mutex);
    g_live_textures.erase(texture);
}


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
    SDL_GPUShader *blend_const_color_d_fs = nullptr;
    SDL_GPUSampler *sampler = nullptr;
    SDL_GPUBuffer *quad_vb = nullptr;
    SDL_GPUBuffer *quad_ib = nullptr;
    std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline *> rect_pipes;
    std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline *> fill_pipes;
    SDL_GPUGraphicsPipeline *blend_d_pipe = nullptr;
    SDL_GPUGraphicsPipeline *blend_const_color_d_pipe = nullptr;
    SDL_GPUTexture *opacity_tables = nullptr;
    std::unordered_map<uint64_t, SDL_GPUTexture *> scratch_pool;
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
    authority_ = Authority::Cpu;
    cpu_dirty_ = tTVPRect(0, 0, Width, Height);
    cpu_dirty_valid_ = true;
    RegisterLiveTexture(this);
}

SdlGpuTexture2D::~SdlGpuTexture2D() {
    ReleaseGpuTexture();
    UnregisterLiveTexture(this);
}

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

void SdlGpuTexture2D::MarkCpuDirty() {
    MarkCpuDirty(tTVPRect(0, 0, Width, Height));
}

void SdlGpuTexture2D::MarkCpuDirty(const tTVPRect &rect) {
    tTVPRect clipped(std::max(0, rect.left), std::max(0, rect.top),
                     std::min(Width, rect.right), std::min(Height, rect.bottom));
    if (clipped.is_empty()) return;
    if (authority_ == Authority::Gpu && !EnsureCpuReadable()) return;
    if (!cpu_dirty_valid_) {
        cpu_dirty_ = clipped;
        cpu_dirty_valid_ = true;
    } else {
        cpu_dirty_.left = std::min(cpu_dirty_.left, clipped.left);
        cpu_dirty_.top = std::min(cpu_dirty_.top, clipped.top);
        cpu_dirty_.right = std::max(cpu_dirty_.right, clipped.right);
        cpu_dirty_.bottom = std::max(cpu_dirty_.bottom, clipped.bottom);
    }
    authority_ = Authority::Cpu;
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
    return true;
}

void SdlGpuTexture2D::UploadCpuToGpu() {
    if (authority_ != Authority::Cpu) return;
    if (format_ != TVPTextureFormat::RGBA || pixels_.empty()) return;
    if (!EnsureGpuTexture()) return;
    if (!cpu_dirty_valid_) return;
    static const bool enable_dirty_upload = []() {
        const char *v = std::getenv("AETHERKIRI_SDL_GPU_ENABLE_DIRTY_UPLOAD");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    // Some KiriKiri bitmap writers retain scanline pointers or update pixels
    // outside the rectangle advertised to Update(). Until those contracts
    // are represented explicitly, a sub-rectangle can omit fresh pixels and
    // produce repeated bands/stale UI fragments. Prefer correctness; keep
    // the optimization available only for focused diagnostics.
    const tTVPRect dirty = enable_dirty_upload
        ? cpu_dirty_ : tTVPRect(0, 0, Width, Height);
    const uint32_t w = static_cast<uint32_t>(dirty.get_width());
    const uint32_t h = static_cast<uint32_t>(dirty.get_height());
    const auto *src = pixels_.data() + static_cast<size_t>(dirty.top) * pitch_ +
                      static_cast<size_t>(dirty.left) * 4u;
    if(TVPUploadSdlGpuTextureRgbaRegion(
           gpu_tex_, static_cast<uint32_t>(Width),
           static_cast<uint32_t>(Height), static_cast<uint32_t>(dirty.left),
           static_cast<uint32_t>(dirty.top), w, h, src, pitch_)) {
        authority_ = Authority::Synchronized;
        cpu_dirty_valid_ = false;
    }
}

bool SdlGpuTexture2D::EnsureCpuReadable() {
    if (authority_ == Authority::Cpu ||
        authority_ == Authority::Synchronized) {
        EnsureCpuStorage();
        return true;
    }
    if (gpu_tex_ == nullptr) {
        EnsureCpuStorage();
        return true;
    }
    if (pixels_.size() == static_cast<size_t>(pitch_) * Height &&
        authority_ != Authority::Gpu) {
        return true;
    }
    const uint32_t w = static_cast<uint32_t>(Width);
    const uint32_t h = static_cast<uint32_t>(Height);
    const uint32_t tight_pitch = w * 4u;
    EnsureCpuStorage();
    TVPRecordSdlGpuAuthorityBarrier(static_cast<uint64_t>(w) * h * 4u);
    if (pitch_ == static_cast<int>(tight_pitch)) {
        if(!TVPReadSdlGpuTextureRgba(gpu_tex_, w, h, pixels_.data(),
                                    pixels_.size()))
            return false;
    } else {
        std::vector<uint8_t> tight(static_cast<size_t>(tight_pitch) * h);
        if(!TVPReadSdlGpuTextureRgba(gpu_tex_, w, h, tight.data(),
                                    tight.size()))
            return false;
        for (uint32_t y = 0; y < h; ++y) {
            std::memcpy(pixels_.data() + static_cast<size_t>(y) * pitch_,
                        tight.data() + static_cast<size_t>(y) * tight_pitch,
                        tight_pitch);
        }
    }
    authority_ = Authority::Synchronized;
    return true;
}

const void *SdlGpuTexture2D::GetScanLineForRead(tjs_uint l) {
    if (static_cast<tjs_uint>(Height) <= l) return nullptr;
    if (authority_ == Authority::Gpu) EnsureCpuReadable();
    if (pixels_.empty()) EnsureCpuStorage();
    return pixels_.data() + static_cast<size_t>(l) * pitch_;
}

void *SdlGpuTexture2D::GetScanLineForWrite(tjs_uint l) {
    if (static_cast<tjs_uint>(Height) <= l) return nullptr;
    // Any pending GPU draw to this texture must be flushed before CPU writes
    // so the CPU path becomes the new source of truth.
    if (authority_ == Authority::Gpu && !EnsureCpuReadable()) return nullptr;
    if (pixels_.empty()) EnsureCpuStorage();
    MarkCpuDirty(tTVPRect(0, static_cast<int>(l), Width,
                          static_cast<int>(l) + 1));
    return pixels_.data() + static_cast<size_t>(l) * pitch_;
}

uint32_t SdlGpuTexture2D::GetPoint(int x, int y) {
    if (x < 0 || y < 0 || x >= Width || y >= Height) return 0;
    if (authority_ == Authority::Gpu) EnsureCpuReadable();
    if (pixels_.empty()) EnsureCpuStorage();
    if (format_ == TVPTextureFormat::Gray) {
        return pixels_[static_cast<size_t>(y) * pitch_ + x];
    }
    const uint8_t *p = pixels_.data() + static_cast<size_t>(y) * pitch_ +
                       static_cast<size_t>(x) * 4u;
    return (static_cast<uint32_t>(p[0])) | (static_cast<uint32_t>(p[1]) << 8u) |
           (static_cast<uint32_t>(p[2]) << 16u) |
           (static_cast<uint32_t>(p[3]) << 24u);
}

void SdlGpuTexture2D::SetPoint(int x, int y, uint32_t clr) {
    if (x < 0 || y < 0 || x >= Width || y >= Height) return;
    if (authority_ == Authority::Gpu && !EnsureCpuReadable()) return;
    if (pixels_.empty()) EnsureCpuStorage();
    if (format_ == TVPTextureFormat::Gray) {
        pixels_[static_cast<size_t>(y) * pitch_ + x] =
            static_cast<uint8_t>(clr);
        MarkCpuDirty(tTVPRect(x, y, x + 1, y + 1));
        return;
    }
    uint8_t *p = pixels_.data() + static_cast<size_t>(y) * pitch_ +
                 static_cast<size_t>(x) * 4u;
    p[0] = static_cast<uint8_t>(clr);
    p[1] = static_cast<uint8_t>(clr >> 8u);
    p[2] = static_cast<uint8_t>(clr >> 16u);
    p[3] = static_cast<uint8_t>(clr >> 24u);
    MarkCpuDirty(tTVPRect(x, y, x + 1, y + 1));
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
    if (authority_ == Authority::Gpu && !EnsureCpuReadable()) return;
    if (pixels_.empty()) EnsureCpuStorage();
    const int src_pitch = pitch > 0 ? pitch : pitch_;
    const auto *src = static_cast<const uint8_t *>(pixel);
    if (format == TVPTextureFormat::Gray) {
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                pixels_[static_cast<size_t>(y) * pitch_ + x] =
                    src[static_cast<size_t>(y - rc.top) * src_pitch +
                        (x - rc.left)];
            }
        }
    } else {
        const int width = right - left;
        for (int y = top; y < bottom; ++y) {
            std::memcpy(pixels_.data() + static_cast<size_t>(y) * pitch_ +
                            static_cast<size_t>(left) * 4u,
                        src + static_cast<size_t>(y - rc.top) * src_pitch +
                            static_cast<size_t>(left - rc.left) * 4u,
                        static_cast<size_t>(width) * 4u);
        }
    }
    MarkCpuDirty(tTVPRect(left, top, right, bottom));
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
    cpu_dirty_ = tTVPRect(0, 0, Width, Height);
    cpu_dirty_valid_ = true;
    authority_ = Authority::Cpu;
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
                                blend_d_frag_spv, blend_d_frag_spv_size, 3, 0);
    p->blend_const_color_d_fs = CreateShader(
        dev, SDL_GPU_SHADERSTAGE_FRAGMENT, blend_const_color_d_frag_spv,
        blend_const_color_d_frag_spv_size, 2, 1);
    if (p->quad_vs == nullptr || p->quad_fs == nullptr ||
        p->fill_vs == nullptr || p->fill_fs == nullptr ||
        p->blend_d_fs == nullptr || p->blend_const_color_d_fs == nullptr) {
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
    SDL_GPUTextureCreateInfo table_info{};
    table_info.type = SDL_GPU_TEXTURETYPE_2D;
    table_info.format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
    table_info.width = 256;
    table_info.height = 256;
    table_info.layer_count_or_depth = 1;
    table_info.num_levels = 1;
    table_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    p->opacity_tables = SDL_CreateGPUTexture(dev, &table_info);
    if (p->sampler == nullptr || p->quad_vb == nullptr ||
        p->quad_ib == nullptr || p->opacity_tables == nullptr) {
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
    SDL_GPUTransferBufferCreateInfo ttb_info{};
    ttb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ttb_info.size = 256u * 256u * 2u;
    SDL_GPUTransferBuffer *ttb = SDL_CreateGPUTransferBuffer(dev, &ttb_info);
    if (vtb != nullptr && itb != nullptr && ttb != nullptr) {
        std::memcpy(SDL_MapGPUTransferBuffer(dev, vtb, false), kQuadVerts,
                    sizeof(kQuadVerts));
        SDL_UnmapGPUTransferBuffer(dev, vtb);
        std::memcpy(SDL_MapGPUTransferBuffer(dev, itb, false), kQuadIndices,
                    sizeof(kQuadIndices));
        SDL_UnmapGPUTransferBuffer(dev, itb);
        auto *tables = static_cast<uint8_t *>(
            SDL_MapGPUTransferBuffer(dev, ttb, false));
        for (int source_alpha = 0; source_alpha < 256; ++source_alpha) {
            for (int dest_alpha = 0; dest_alpha < 256; ++dest_alpha) {
                const int index = source_alpha * 256 + dest_alpha;
                tables[index * 2] = TVPOpacityOnOpacityTable[index];
                tables[index * 2 + 1] = TVPNegativeMulTable[index];
            }
        }
        SDL_UnmapGPUTransferBuffer(dev, ttb);
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
        SDL_GPUTextureTransferInfo tloc{};
        tloc.transfer_buffer = ttb;
        tloc.pixels_per_row = 256;
        tloc.rows_per_layer = 256;
        SDL_GPUTextureRegion treg{};
        treg.texture = p->opacity_tables;
        treg.w = 256;
        treg.h = 256;
        treg.d = 1;
        SDL_UploadToGPUTexture(cp, &tloc, &treg, false);
        SDL_EndGPUCopyPass(cp);
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(dev, vtb);
        SDL_ReleaseGPUTransferBuffer(dev, itb);
        SDL_ReleaseGPUTransferBuffer(dev, ttb);
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

SDL_GPUGraphicsPipeline *GetFillPipeline(GpuPipelines *p, bool preserve_alpha,
                                         bool *ok) {
    const uint32_t key = preserve_alpha ? 1u : 0u;
    auto it = p->fill_pipes.find(key);
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
    color_target.blend_state.enable_blend = false;
    color_target.blend_state.enable_color_write_mask = true;
    color_target.blend_state.color_write_mask =
        SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
        SDL_GPU_COLORCOMPONENT_B |
        (preserve_alpha ? 0 : SDL_GPU_COLORCOMPONENT_A);

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
    p->fill_pipes[key] = pipe;
    *ok = true;
    return pipe;
}

// Creates the pipeline used by destination-read (_d) blend modes. The
// fragment shader samples both the source and the destination (via a scratch
// copy) and computes the final color entirely in the shader, so the color
// target blend state is NONE.
SDL_GPUGraphicsPipeline *GetBlendDPipeline(GpuPipelines *p,
                                            bool const_color, bool *ok) {
    SDL_GPUGraphicsPipeline *&cached = const_color
        ? p->blend_const_color_d_pipe : p->blend_d_pipe;
    if (cached != nullptr) {
        *ok = true;
        return cached;
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
    pipe_info.fragment_shader = const_color
        ? p->blend_const_color_d_fs : p->blend_d_fs;
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
    cached = pipe;
    *ok = true;
    return pipe;
}

// Ensures the scratch texture used to hold a copy of the destination for _d
// blend modes is sized to at least (w, h) and returns it.
SDL_GPUTexture *EnsureScratch(GpuPipelines *p, int w, int h) {
    SDL_GPUDevice *dev = TVPGetSdlGpuDevice();
    if (dev == nullptr) return nullptr;
    w = std::max(w, 1);
    h = std::max(h, 1);
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(w)) << 32u) |
                         static_cast<uint32_t>(h);
    auto existing = p->scratch_pool.find(key);
    if (existing != p->scratch_pool.end()) return existing->second;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.width = static_cast<Uint32>(w);
    info.height = static_cast<Uint32>(h);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    SDL_GPUTexture *scratch = SDL_CreateGPUTexture(dev, &info);
    if (scratch != nullptr) p->scratch_pool.emplace(key, scratch);
    return scratch;
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
        if (p->blend_const_color_d_pipe)
            SDL_ReleaseGPUGraphicsPipeline(dev, p->blend_const_color_d_pipe);
        if (p->quad_vs) SDL_ReleaseGPUShader(dev, p->quad_vs);
        if (p->quad_fs) SDL_ReleaseGPUShader(dev, p->quad_fs);
        if (p->fill_vs) SDL_ReleaseGPUShader(dev, p->fill_vs);
        if (p->fill_fs) SDL_ReleaseGPUShader(dev, p->fill_fs);
        if (p->blend_d_fs) SDL_ReleaseGPUShader(dev, p->blend_d_fs);
        if (p->blend_const_color_d_fs)
            SDL_ReleaseGPUShader(dev, p->blend_const_color_d_fs);
        if (p->sampler) SDL_ReleaseGPUSampler(dev, p->sampler);
        if (p->quad_vb) SDL_ReleaseGPUBuffer(dev, p->quad_vb);
        if (p->quad_ib) SDL_ReleaseGPUBuffer(dev, p->quad_ib);
        for (auto &entry : p->scratch_pool)
            SDL_ReleaseGPUTexture(dev, entry.second);
        if (p->opacity_tables) SDL_ReleaseGPUTexture(dev, p->opacity_tables);
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
    if (name == "AlphaBlend_d") return TVP_GODOT_GPU_BLEND_ALPHA_D;
    if (name == "ConstColorAlphaBlend_d")
        return TVP_GODOT_GPU_BLEND_CONST_ALPHA_D;
    if (name == "PsScreenBlend") return TVP_GODOT_GPU_BLEND_PS_SCREEN;
    if (name == "PsAddBlend") return TVP_GODOT_GPU_BLEND_PS_ADD;
    if (name == "PsSubBlend") return TVP_GODOT_GPU_BLEND_PS_SUBTRACT;
    if (name == "PsMulBlend") return TVP_GODOT_GPU_BLEND_PS_MULTIPLY;
    return 0;
}

bool IsDestinationReadMode(uint32_t mode) {
    switch (mode) {
        case TVP_GODOT_GPU_BLEND_ALPHA:
            // KiriKiri's normal alpha blend uses 8-bit integer arithmetic
            // with a /256 shift. Fixed-function UNORM blending uses /255 and
            // cannot be pixel-identical, so keep CPU as the authority.
            return true;
        case TVP_GODOT_GPU_BLEND_ALPHA_D:
        case TVP_GODOT_GPU_BLEND_CONST_ALPHA_D:
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

bool SdlGpuRenderManager::DrawFill(SdlGpuTexture2D *dst,
                                   const tTVPRect &rctar, uint32_t color,
                                   bool preserve_alpha) {
    if (dst == nullptr || !TVPIsSdlGpuActive() ||
        dst->GetFormat() != TVPTextureFormat::RGBA)
        return false;
    if (rctar.is_empty()) return true;
    if (rctar.left < 0 || rctar.top < 0 || rctar.right > dst->GetWidth() ||
        rctar.bottom > dst->GetHeight())
        return false;
    GpuPipelines *p = EnsurePipelines();
    if (p == nullptr) return false;
    dst->UploadCpuToGpu();
    if (!dst->EnsureGpuTexture()) return false;
    bool ok = false;
    SDL_GPUGraphicsPipeline *pipe = GetFillPipeline(p, preserve_alpha, &ok);
    if (!ok || pipe == nullptr) return false;
    SDL_GPUCommandBuffer *cmd = TVPGetSdlGpuFrameCommandBuffer();
    if (cmd == nullptr) return false;
    TVPEnsureSdlGpuRenderPassReady();
    SDL_GPUColorTargetInfo target{};
    target.texture = dst->GpuTexture();
    target.load_op = SDL_GPU_LOADOP_LOAD;
    target.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &target, 1, nullptr);
    if (pass == nullptr) return false;
    SDL_BindGPUGraphicsPipeline(pass, pipe);
    SDL_GPUBufferBinding vertex{.buffer = p->quad_vb, .offset = 0};
    SDL_GPUBufferBinding index{.buffer = p->quad_ib, .offset = 0};
    SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);
    SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    const float vertex_params[6] = {
        static_cast<float>(rctar.left), static_cast<float>(rctar.top),
        static_cast<float>(rctar.get_width()),
        static_cast<float>(rctar.get_height()),
        static_cast<float>(dst->GetWidth()),
        static_cast<float>(dst->GetHeight())};
    SDL_PushGPUVertexUniformData(cmd, 0, vertex_params,
                                 sizeof(vertex_params));
    // CPU pixels are stored in the byte order of the little-endian ARGB word.
    const float fragment_params[4] = {
        static_cast<float>(color & 0xffu) / 255.0f,
        static_cast<float>((color >> 8u) & 0xffu) / 255.0f,
        static_cast<float>((color >> 16u) & 0xffu) / 255.0f,
        static_cast<float>((color >> 24u) & 0xffu) / 255.0f};
    SDL_PushGPUFragmentUniformData(cmd, 0, fragment_params,
                                   sizeof(fragment_params));
    SDL_GPUViewport viewport{.x = 0.0f, .y = 0.0f,
        .w = static_cast<float>(dst->GetWidth()),
        .h = static_cast<float>(dst->GetHeight()),
        .min_depth = 0.0f, .max_depth = 1.0f};
    SDL_SetGPUViewport(pass, &viewport);
    SDL_DrawGPUIndexedPrimitives(pass, 6, 1, 0, 0, 0);
    SDL_EndGPURenderPass(pass);
    dst->MarkGpuDirty();
    return true;
}

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

    // Self-copy / self-blend (dst==src, e.g. internal region scroll): the
    // texture would be simultaneously a color target and a fragment sampler,
    // which SDL_GPU forbids (and the Vulkan backend's layout transition does
    // not cover it). Copy the destination to the scratch texture first and
    // sample the scratch instead.
    SDL_GPUTexture *sample_src = src;
    if (dst->GpuTexture() == src) {
        SDL_GPUTexture *scratch =
            EnsureScratch(p, dst->GetWidth(), dst->GetHeight());
        if (scratch == nullptr) return false;
        SDL_GPUCopyPass *cp = TVPGetSdlGpuFrameCopyPass();
        if (cp == nullptr) return false;
        SDL_GPUTextureLocation ssrc{};
        ssrc.texture = dst->GpuTexture();
        ssrc.mip_level = 0;
        ssrc.layer = 0;
        ssrc.x = 0;
        ssrc.y = 0;
        ssrc.z = 0;
        SDL_GPUTextureLocation sdst{};
        sdst.texture = scratch;
        sdst.mip_level = 0;
        sdst.layer = 0;
        sdst.x = 0;
        sdst.y = 0;
        sdst.z = 0;
        SDL_CopyGPUTextureToTexture(
            cp, &ssrc, &sdst, static_cast<Uint32>(dst->GetWidth()),
            static_cast<Uint32>(dst->GetHeight()), 1, false);
        sample_src = scratch;
        TVPEnsureSdlGpuRenderPassReady();
    }

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

    // Sampler slot 0: source texture (scratch when dst==src self-copy).
    SDL_GPUTextureSamplerBinding sampler_bind{};
    sampler_bind.texture = sample_src;
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
    const bool const_color = method_name != nullptr &&
        std::strcmp(method_name, "ConstColorAlphaBlend_d") == 0;
    if (dst == nullptr || (!const_color && src == nullptr)) return false;
    if (!TVPIsSdlGpuActive()) return false;
    if (rctar.is_empty()) return true;
    if (rctar.left < 0 || rctar.top < 0 || rctar.right > dst->GetWidth() ||
        rctar.bottom > dst->GetHeight() || src_rc.left < 0 || src_rc.top < 0 ||
        (!const_color && (src_rc.right > src_w || src_rc.bottom > src_h ||
         rctar.get_width() != src_rc.get_width() ||
         rctar.get_height() != src_rc.get_height() ||
         dst->GpuTexture() == src)))
        return false;
    GpuPipelines *p = EnsurePipelines();
    if (p == nullptr) return false;
    dst->UploadCpuToGpu();
    if (!dst->EnsureGpuTexture()) return false;
    if (mode == 0) return false;

    bool ok = false;
    SDL_GPUGraphicsPipeline *pipe = GetBlendDPipeline(p, const_color, &ok);
    if (!ok || pipe == nullptr) return false;

    SDL_GPUCommandBuffer *cmd = TVPGetSdlGpuFrameCommandBuffer();
    if (cmd == nullptr) return false;

    // Copy the destination to the scratch texture. Use a copy pass; the
    // backend ensures any open render pass is ended first.
    SDL_GPUCommandBuffer *save_cmd = cmd;
    SDL_GPUTexture *scratch =
        EnsureScratch(p, rctar.get_width(), rctar.get_height());
    if (scratch == nullptr) return false;
    TVPEnsureSdlGpuRenderPassReady();
    SDL_GPUCopyPass *cp = TVPGetSdlGpuFrameCopyPass();
    if (cp == nullptr) return false;
    SDL_GPUTextureLocation src_loc{};
    src_loc.texture = dst->GpuTexture();
    src_loc.mip_level = 0;
    src_loc.layer = 0;
    src_loc.x = static_cast<Uint32>(rctar.left);
    src_loc.y = static_cast<Uint32>(rctar.top);
    src_loc.z = 0;
    SDL_GPUTextureLocation dst_loc{};
    dst_loc.texture = scratch;
    dst_loc.mip_level = 0;
    dst_loc.layer = 0;
    dst_loc.x = 0;
    dst_loc.y = 0;
    dst_loc.z = 0;
    const Uint32 copy_w = static_cast<Uint32>(rctar.get_width());
    const Uint32 copy_h = static_cast<Uint32>(rctar.get_height());
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

    if (const_color) {
        SDL_GPUTextureSamplerBinding sampler_binds[2] = {
            {.texture = scratch, .sampler = p->sampler},
            {.texture = p->opacity_tables, .sampler = p->sampler},
        };
        SDL_BindGPUFragmentSamplers(rp, 0, sampler_binds, 2);
    } else {
        SDL_GPUTextureSamplerBinding sampler_binds[3] = {
            {.texture = src, .sampler = p->sampler},
            {.texture = scratch, .sampler = p->sampler},
            {.texture = p->opacity_tables, .sampler = p->sampler},
        };
        SDL_BindGPUFragmentSamplers(rp, 0, sampler_binds, 3);
    }

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

    if (const_color) {
        const uint32_t fparams[4] = {
            color & 0xffu, (color >> 8u) & 0xffu,
            (color >> 16u) & 0xffu,
            static_cast<uint32_t>(std::clamp(opacity, 0, 255))};
        SDL_PushGPUFragmentUniformData(save_cmd, 0, fparams,
                                       sizeof(fparams));
    }

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

    // Fill methods have no source texture.
    bool handled = false;
    static const bool disable_gpu_draws = []() {
        const char *v = std::getenv("AETHERKIRI_SDL_GPU_DISABLE_DRAWS");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    static const bool enable_mixed_gpu_draws = []() {
        const char *v = std::getenv("AETHERKIRI_SDL_GPU_ENABLE_MIXED_DRAWS");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    static const bool enable_d_blend = []() {
        const char *v = std::getenv("AETHERKIRI_SDL_GPU_ENABLE_D_BLEND");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    static const bool disable_const_color_d_blend = []() {
        const char *v = std::getenv(
            "AETHERKIRI_SDL_GPU_DISABLE_CONST_COLOR_D_BLEND");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    static const bool disable_gpu_fill = []() {
        const char *v = std::getenv("AETHERKIRI_SDL_GPU_DISABLE_FILL");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    // Mixing GPU Copy/Fill with exact software alpha on one target forces
    // synchronous authority barriers. Keep a coherent software composite by
    // default and upload once for SDL_GPU presentation.
    if (disable_gpu_draws || !enable_mixed_gpu_draws) {
        handled = false;
    } else if (!disable_gpu_fill && dst != nullptr && TVPIsSdlGpuActive() &&
               (method_name == "FillARGB" || method_name == "FillColor")) {
        const uint32_t color =
            godot_method != nullptr ? godot_method->Color() : 0;
        handled = DrawFill(dst, rctar, color, method_name == "FillColor");
    } else if (enable_d_blend && !disable_const_color_d_blend &&
               dst != nullptr && TVPIsSdlGpuActive() &&
               method_name == "ConstColorAlphaBlend_d") {
        dst->UploadCpuToGpu();
        const int opacity = godot_method != nullptr
            ? godot_method->Opacity() : 255;
        const uint32_t color = godot_method != nullptr
            ? godot_method->Color() : 0;
        handled = DrawRectD(dst, rctar, method_name.c_str(),
                            TVP_GODOT_GPU_BLEND_CONST_ALPHA_D, opacity, color,
                            nullptr, tTVPRect(), 0, 0);
    } else if (dst != nullptr && src != nullptr &&
               src->EnsureGpuTexture() && method_name != "FillARGB") {
        // Fixed-function blending reads the existing render target. A prior
        // software operation may have made the CPU copy authoritative, so
        // synchronize both operands before recording the GPU draw.
        dst->UploadCpuToGpu();
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
        if (enable_d_blend && method_name == "AlphaBlend_d") {
            handled = DrawRectD(dst, rctar, method_name.c_str(), mode, opacity,
                                color, src->GpuTexture(), src_rc,
                                src->GetWidth(), src->GetHeight());
        } else if (!IsDestinationReadMode(mode)) {
            handled = DrawRect(dst, rctar, method_name.c_str(), mode, opacity,
                               color, src->GpuTexture(), src_rc,
                               src->GetWidth(), src->GetHeight());
        }
    }

    if (!handled) {
        SoftwareDelegate()->OperateRect(delegate_method, tar, reftar, rctar,
                                        textures);
        if (dst != nullptr) {
            dst->MarkCpuDirty();
        }
    }
    TVPRecordSdlGpuDraw(handled);
    TVPRecordSdlGpuMethod(method_name.c_str(), handled);
    static const bool trace_methods = []() {
        const char *v = std::getenv("AETHERKIRI_SDL_GPU_TRACE_METHODS");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    if(trace_methods) {
        static std::unordered_set<std::string> seen;
        const std::string key = method_name + (handled ? ":gpu" : ":software");
        if(seen.insert(key).second) {
            std::fprintf(stderr, "sdl3_gpu method %s path=%s\n",
                         method_name.c_str(), handled ? "gpu" : "software");
        }
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

void TVPReleaseAllSdlGpuTextures() {
    std::vector<SdlGpuTexture2D *> textures;
    {
        std::lock_guard<std::mutex> lock(g_live_texture_mutex);
        textures.assign(g_live_textures.begin(), g_live_textures.end());
    }
    for (SdlGpuTexture2D *texture : textures) {
        if (texture != nullptr) texture->ReleaseGpuTexture();
    }
}
