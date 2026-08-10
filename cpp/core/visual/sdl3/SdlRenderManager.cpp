#include "SdlRenderManager.h"

#include "../LayerBitmapIntf.h"
#include "MsgIntf.h"
#include "tjsHashSearch.h"
#include "sdl_render_backend.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace {

// Blend-mode tags previously supplied by GodotGpuBridge.h. The Godot GPU
// bridge is gone; these values are kept so the (now always-failing) GPU fast
// paths and E-mote mask helpers compile unchanged and fall back to software.
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
constexpr uint32_t TVP_GODOT_GPU_TRIANGLE_TVP_BLEND =
    TVP_GODOT_GPU_BLEND_TVP_OPERATION;

int BytesPerPixel(TVPTextureFormat::e format) {
    switch (format) {
        case TVPTextureFormat::Gray:
            return 1;
        case TVPTextureFormat::RGB:
            return 3;
        case TVPTextureFormat::RGBA:
            return 4;
        default:
            return 4;
    }
}

bool CopyRect(uint8_t *dst, int dst_pitch, int dst_width, int dst_height,
              const uint8_t *src, int src_pitch, int bytes_per_pixel,
              const tTVPRect &rc) {
    if(dst == nullptr || src == nullptr || dst_pitch <= 0 || src_pitch <= 0 ||
       dst_width <= 0 || dst_height <= 0 || bytes_per_pixel <= 0 ||
       rc.right <= rc.left || rc.bottom <= rc.top) {
        return false;
    }

    const int left = std::max(0, rc.left);
    const int top = std::max(0, rc.top);
    const int right = std::min(dst_width, rc.right);
    const int bottom = std::min(dst_height, rc.bottom);
    if(left >= right || top >= bottom) {
        return true;
    }

    const size_t source_x = static_cast<size_t>(left - rc.left) * bytes_per_pixel;
    const size_t width_bytes = static_cast<size_t>(right - left) * bytes_per_pixel;
    if(source_x + width_bytes > static_cast<size_t>(src_pitch) ||
       static_cast<size_t>(right) * bytes_per_pixel >
           static_cast<size_t>(dst_pitch)) {
        return false;
    }
    for(int y = top; y < bottom; ++y) {
        const size_t source_y = static_cast<size_t>(y - rc.top) * src_pitch;
        const size_t destination_y = static_cast<size_t>(y) * dst_pitch;
        std::memcpy(dst + destination_y + static_cast<size_t>(left) * bytes_per_pixel,
                    src + source_y + source_x, width_bytes);
    }
    return true;
}

std::mutex g_method_stats_mutex;
std::unordered_map<std::string, uint64_t> g_method_stats;
std::atomic<uint64_t> g_texture_create_count{0};
std::atomic<uint64_t> g_texture_clone_count{0};
std::atomic<uint64_t> g_texture_resize_count{0};
std::atomic<uint64_t> g_software_fallback_count{0};
std::atomic<uint64_t> g_gpu_fastpath_count{0};
std::atomic<bool> g_gpu_fastpath_enabled{true};
std::unordered_map<std::string, uint64_t> g_gpu_method_stats;
std::unordered_map<std::string, uint64_t> g_copy_fallback_stats;

bool DetailedRenderStats() {
    static const bool enabled = []() {
        const char *value = std::getenv("AETHERKIRI_GODOT_RENDER_STATS");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void CountMethodFallback(iTVPRenderMethod *method) {
    g_software_fallback_count.fetch_add(1, std::memory_order_relaxed);
    if (!DetailedRenderStats()) return;
    std::lock_guard<std::mutex> lock(g_method_stats_mutex);
    const std::string name = method != nullptr ? method->GetName() : "(null)";
    g_method_stats[name] += 1;
}

void CountGpuFastPath(const std::string &name) {
    g_gpu_fastpath_count.fetch_add(1, std::memory_order_relaxed);
    if (!DetailedRenderStats()) return;
    std::lock_guard<std::mutex> lock(g_method_stats_mutex);
    g_gpu_method_stats[name] += 1;
}

void CountCopyFallbackReason(const std::string &reason) {
    if (!DetailedRenderStats()) return;
    std::lock_guard<std::mutex> lock(g_method_stats_mutex);
    g_copy_fallback_stats[reason] += 1;
}

void CountCopyFallbackReason(const char *reason) {
    if (!DetailedRenderStats()) return;
    std::lock_guard<std::mutex> lock(g_method_stats_mutex);
    g_copy_fallback_stats[reason != nullptr ? reason : "(null)"] += 1;
}

bool TraceGpuFallback() {
    static const bool enabled = []() {
        const char *value = std::getenv("AETHERKIRI_GODOT_GPU_TRACE_FALLBACK");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool DeferredSdlGpuDrainEnabled() {
    // The Godot GPU bridge batch machinery was removed with the bridge;
    // deferred draining is no longer available.
    return false;
}

bool IsGpuRectFastPathEnabled(const char *name) {
    if (!g_gpu_fastpath_enabled.load(std::memory_order_relaxed)) {
        return false;
    }
    const auto is_default_enabled = [&]() {
        return std::strcmp(name, "FillARGB") == 0 ||
               std::strcmp(name, "FillMask") == 0 ||
               std::strcmp(name, "Copy") == 0 ||
               std::strcmp(name, "RemoveConstOpacity") == 0 ||
               std::strcmp(name, "AlphaBlend") == 0 ||
               std::strcmp(name, "AlphaBlend_a") == 0 ||
               std::strcmp(name, "AlphaBlend_d") == 0 ||
               std::strcmp(name, "ConstAlphaBlend_d") == 0 ||
               std::strcmp(name, "ConstAlphaBlend_SD") == 0 ||
               std::strcmp(name, "ConstAlphaBlend_SD_d") == 0 ||
               std::strcmp(name, "UnivTransBlend") == 0 ||
               std::strcmp(name, "UnivTransBlend_d") == 0 ||
               std::strcmp(name, "UnivTransBlend_a") == 0 ||
               std::strcmp(name, "CopyColor") == 0 ||
               std::strcmp(name, "PsAddBlend") == 0 ||
               std::strcmp(name, "PsSubBlend") == 0 ||
               std::strcmp(name, "PsScreenBlend") == 0 ||
               std::strcmp(name, "PsMulBlend") == 0;
    };
    static const std::string setting = []() {
        const char *value = std::getenv("AETHERKIRI_GODOT_GPU_RECT_FASTPATH");
        return value != nullptr ? std::string(value) : std::string();
    }();
    if (setting.empty()) return is_default_enabled();
    if (setting == "0" || setting == "off" || setting == "none") return false;
    if (setting == "1" || setting == "all" || setting == "default") {
        return is_default_enabled();
    }

    size_t start = 0;
    while (start < setting.size()) {
        const size_t end = setting.find_first_of(",;: ", start);
        const std::string token =
            setting.substr(start, end == std::string::npos ? end : end - start);
        if (token == name) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

constexpr int kDefaultGpuRectMinArea = 32768;
constexpr int kDefaultAlphaRectMinArea = 32768;

int GpuRectMinArea() {
    static const int min_area = []() {
        const char *value = std::getenv("AETHERKIRI_GODOT_GPU_RECT_MIN_AREA");
        if (value == nullptr || value[0] == '\0') return kDefaultGpuRectMinArea;
        char *end = nullptr;
        long parsed = std::strtol(value, &end, 10);
        if (end == value || parsed < 0) return kDefaultGpuRectMinArea;
        return static_cast<int>(std::min<long>(parsed, 1 << 30));
    }();
    return min_area;
}

int GpuRectMinAreaForMethod(const char *name) {
    if (name != nullptr &&
        (std::strcmp(name, "AlphaBlend") == 0 ||
         std::strcmp(name, "AlphaBlend_a") == 0 ||
         std::strcmp(name, "AlphaBlend_d") == 0)) {
        static const int min_area = []() {
            const char *value =
                std::getenv("AETHERKIRI_GODOT_GPU_ALPHA_RECT_MIN_AREA");
            if (value == nullptr || value[0] == '\0') {
                return kDefaultAlphaRectMinArea;
            }
            char *end = nullptr;
            long parsed = std::strtol(value, &end, 10);
            if (end == value || parsed < 0) return kDefaultAlphaRectMinArea;
            return static_cast<int>(std::min<long>(parsed, 1 << 30));
        }();
        return min_area;
    }
    return GpuRectMinArea();
}

bool IsGpuRectLargeEnoughForMethod(const tTVPRect &rect, const char *name) {
    return rect.get_width() > 0 && rect.get_height() > 0 &&
           rect.get_width() * rect.get_height() >= GpuRectMinAreaForMethod(name);
}

bool ShouldUseGpuRectFastPath(const tTVPRect &rect, const char *name,
                              const SDLTexture2D *dst,
                              const SDLTexture2D *src = nullptr,
                              const SDLTexture2D *src2 = nullptr,
                              const SDLTexture2D *src3 = nullptr) {
    if (IsGpuRectLargeEnoughForMethod(rect, name)) return true;
    return (dst != nullptr && dst->HasPendingGpuWrites()) ||
           (src != nullptr && src->HasPendingGpuWrites()) ||
           (src2 != nullptr && src2->HasPendingGpuWrites()) ||
           (src3 != nullptr && src3->HasPendingGpuWrites()) ||
           (dst != nullptr && dst->RequiresGpuReadback()) ||
           (src != nullptr && src->RequiresGpuReadback()) ||
           (src2 != nullptr && src2->RequiresGpuReadback()) ||
           (src3 != nullptr && src3->RequiresGpuReadback());
}

bool IsOpaqueAlphaBlendCopyEnabled() {
    static const bool enabled = []() {
        const char *value = std::getenv("AETHERKIRI_GODOT_GPU_OPAQUE_COPY");
        return value == nullptr || value[0] == '\0' || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool IsGpuCopyTrianglesEnabled() {
    static const bool enabled = []() {
        const char *value = std::getenv("AETHERKIRI_GODOT_GPU_COPY_TRIANGLES");
        return value == nullptr || value[0] == '\0' || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool IsGpuBlendTrianglesEnabled() {
    static const bool enabled = []() {
        const char *value =
            std::getenv("AETHERKIRI_GODOT_GPU_BLEND_TRIANGLES");
        return value == nullptr || value[0] == '\0' ||
               std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool RectAbsSizeMatches(const tTVPRect &dst, const tTVPRect &src) {
    return dst.get_width() > 0 && dst.get_height() > 0 &&
           std::abs(src.get_width()) == dst.get_width() &&
           std::abs(src.get_height()) == dst.get_height();
}

bool RectNeedsAreaDownsample(const tTVPRect &dst, const tTVPRect &src) {
    return dst.get_width() > 0 && dst.get_height() > 0 &&
           (dst.get_width() < std::abs(src.get_width()) ||
            dst.get_height() < std::abs(src.get_height()));
}

bool RectNeedsAlphaAreaDownsample(const tTVPRect &dst, const tTVPRect &src,
                                  const SDLTexture2D *texture) {
    return texture != nullptr && texture->HasKnownTransparency() &&
           RectNeedsAreaDownsample(dst, src);
}

bool TrianglesNeedStrongAreaDownsample(uint32_t triangle_count,
                                       const tTVPPointD *dst_points,
                                       const tTVPPointD *src_points) {
    if (triangle_count == 0 || dst_points == nullptr || src_points == nullptr) {
        return false;
    }
    // Comparing source/destination axis-aligned bounds misclassifies a
    // rotated surface as downsampled whenever one rotated bound gets shorter.
    // That sent the common slanted E-mote presentation through the software
    // rasterizer and forced two full GPU readbacks per frame.  Compare the
    // corresponding triangle edges instead.  The GPU shader does
    // alpha-premultiplied bilinear sampling, so reserve the area-filtered CPU
    // path only for a genuinely strong (>2x along an edge) reduction.
    constexpr double kStrongDownsampleScale = 0.5;
    constexpr double kLengthEpsilon = 0.001;
    for(uint32_t triangle = 0; triangle < triangle_count; ++triangle) {
        const uint32_t base = triangle * 3u;
        for(uint32_t edge = 0; edge < 3u; ++edge) {
            const uint32_t next = (edge + 1u) % 3u;
            const auto &src0 = src_points[base + edge];
            const auto &src1 = src_points[base + next];
            const auto &dst0 = dst_points[base + edge];
            const auto &dst1 = dst_points[base + next];
            const double srcDx = src1.x - src0.x;
            const double srcDy = src1.y - src0.y;
            const double dstDx = dst1.x - dst0.x;
            const double dstDy = dst1.y - dst0.y;
            const double srcLengthSquared = srcDx * srcDx + srcDy * srcDy;
            if(srcLengthSquared <= kLengthEpsilon * kLengthEpsilon) {
                continue;
            }
            const double dstLengthSquared = dstDx * dstDx + dstDy * dstDy;
            const double threshold =
                kStrongDownsampleScale * kStrongDownsampleScale *
                srcLengthSquared;
            if(dstLengthSquared + kLengthEpsilon * kLengthEpsilon <
               threshold) {
                return true;
            }
        }
    }
    return false;
}

bool TrianglesNeedAlphaAreaDownsample(uint32_t triangle_count,
                                      const tTVPPointD *dst_points,
                                      const tTVPPointD *src_points,
                                      const SDLTexture2D *texture) {
    return texture != nullptr && texture->HasKnownTransparency() &&
           TrianglesNeedStrongAreaDownsample(
               triangle_count, dst_points, src_points);
}

bool RectBoundsInsideTexture(const tTVPRect &rc, const SDLTexture2D *texture) {
    if (texture == nullptr) return false;
    const tjs_int left = std::min(rc.left, rc.right);
    const tjs_int right = std::max(rc.left, rc.right);
    const tjs_int top = std::min(rc.top, rc.bottom);
    const tjs_int bottom = std::max(rc.top, rc.bottom);
    return left >= 0 && top >= 0 &&
           right <= static_cast<tjs_int>(texture->GetWidth()) &&
           bottom <= static_cast<tjs_int>(texture->GetHeight()) &&
           left < right && top < bottom;
}

bool IsFullTextureRect(const tTVPRect &rc, int width, int height) {
    return rc.left <= 0 && rc.top <= 0 && rc.right >= width && rc.bottom >= height;
}

bool ScanOpaqueRgba(const void *pixel, int pitch, int width, int height) {
    if (pixel == nullptr || width <= 0 || height <= 0) return false;
    const auto *bytes = static_cast<const uint8_t *>(pixel);
    const int stride = pitch > 0 ? pitch : width * 4;
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = bytes + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            if (row[x * 4 + 3] != 0xff) return false;
        }
    }
    return true;
}

} // namespace

iTVPRenderManager *TVPGetSoftwareRenderManager();

SDLRenderMethod::SDLRenderMethod(iTVPRenderMethod *delegate)
    : delegate_(delegate) {}

int SDLRenderMethod::EnumParameterID(const char *name) {
    const int id = delegate_ != nullptr ? delegate_->EnumParameterID(name) : -1;
    if(name != nullptr && std::strcmp(name, "phase") == 0) phase_id_ = id;
    if(name != nullptr && std::strcmp(name, "vague") == 0) vague_id_ = id;
    return id;
}
void SDLRenderMethod::SetParameterUInt(int id, unsigned int Value) {
    if (delegate_) delegate_->SetParameterUInt(id, Value);
}
void SDLRenderMethod::SetParameterInt(int id, int Value) {
    if(id == phase_id_) phase_ = Value;
    if(id == vague_id_) vague_ = Value;
    if (delegate_) delegate_->SetParameterInt(id, Value);
}
void SDLRenderMethod::SetParameterPtr(int id, const void *Value) {
    if (delegate_) delegate_->SetParameterPtr(id, Value);
}
void SDLRenderMethod::SetParameterFloat(int id, float Value) {
    if (delegate_) delegate_->SetParameterFloat(id, Value);
}
void SDLRenderMethod::SetParameterColor4B(int id, unsigned int clr) {
    color_ = clr;
    if (delegate_) delegate_->SetParameterColor4B(id, clr);
}
void SDLRenderMethod::SetParameterOpa(int id, int Value) {
    opacity_ = Value;
    if (delegate_) delegate_->SetParameterOpa(id, Value);
}
void SDLRenderMethod::SetParameterFloatArray(int id, float *Value, int nElem) {
    if (delegate_) delegate_->SetParameterFloatArray(id, Value, nElem);
}
iTVPRenderMethod *SDLRenderMethod::SetBlendFuncSeparate(
    int func, int srcRGB, int dstRGB, int srcAlpha, int dstAlpha) {
    if (delegate_) {
        delegate_->SetBlendFuncSeparate(func, srcRGB, dstRGB, srcAlpha, dstAlpha);
    }
    return this;
}
bool SDLRenderMethod::IsBlendTarget() {
    return delegate_ == nullptr || delegate_->IsBlendTarget();
}

iTVPRenderManager *SDLRenderManager::SoftwareDelegate() {
    if (software_delegate_ == nullptr) {
        software_delegate_ = TVPGetSoftwareRenderManager();
    }
    return software_delegate_;
}

SDLTexture2D::SDLTexture2D(const void *pixel, int pitch, unsigned int w,
                               unsigned int h, TVPTextureFormat::e format,
                               int create_flags)
    : iTVPTexture2D(static_cast<tjs_int>(w), static_cast<tjs_int>(h)),
      format_(format),
      pitch_(pitch > 0 ? pitch : static_cast<int>(w) * BytesPerPixel(format)),
      discard_unwritten_on_partial_update_(
          (create_flags & RENDER_CREATE_TEXTURE_FLAG_NO_COMPRESS) != 0) {
    pixels_.resize(static_cast<size_t>(pitch_) * h);
    if (pixel != nullptr) {
        const int src_pitch = pitch > 0 ? pitch : pitch_;
        const auto *src = static_cast<const uint8_t *>(pixel);
        for (unsigned int y = 0; y < h; ++y) {
            std::memcpy(pixels_.data() + static_cast<size_t>(y) * pitch_,
                        src + static_cast<size_t>(y) * src_pitch,
                        static_cast<size_t>(std::min(pitch_, src_pitch)));
        }
        SetOpacityFromPixels(pixels_.data(), pitch_);
    } else {
        MarkTransparentKnown();
    }
    MarkCpuDirty();
}

SDLTexture2D::~SDLTexture2D() { ReleaseGpuHandle(); }

void SDLTexture2D::EnsureCpuStorage() {
    const size_t required = static_cast<size_t>(pitch_) * Height;
    if (pixels_.size() != required) {
        pixels_.assign(required, 0);
    }
}

void SDLTexture2D::DiscardCpuStorage() {
    if (pixels_.empty()) return;
    std::vector<uint8_t>().swap(pixels_);
}

void SDLTexture2D::SetOpacityFromPixels(const void *pixel, int pitch) {
    if (format_ != TVPTextureFormat::RGBA) {
        MarkOpaqueKnown();
        return;
    }
    if (pixel == nullptr || Width <= 0 || Height <= 0) {
        MarkOpacityUnknown();
        return;
    }
    const int stride = pitch > 0 ? pitch : pitch_;
    const auto *bytes = static_cast<const uint8_t *>(pixel);
    for (int y = 0; y < Height; ++y) {
        const uint8_t *row = bytes + static_cast<size_t>(y) * stride;
        for (int x = 0; x < Width; ++x) {
            if (row[x * 4 + 3] != 0xff) {
                MarkTransparentKnown();
                return;
            }
        }
    }
    MarkOpaqueKnown();
}

void SDLTexture2D::MarkOpacityUnknown() {
    opacity_known_ = false;
    opaque_ = false;
}

void SDLTexture2D::MarkTransparentKnown() {
    opacity_known_ = true;
    opaque_ = false;
}

void SDLTexture2D::MarkOpaqueKnown() {
    opacity_known_ = true;
    opaque_ = true;
}

void SDLTexture2D::CreateGpuHandle(const void *pixel, int pitch) {
    SDL_Renderer *renderer = TVPGetSdlRenderer();
    if (renderer == nullptr) return;
    const void *src = pixel != nullptr ? pixel :
        (pixels_.empty() ? nullptr : pixels_.data());
    uint32_t stride = static_cast<uint32_t>(
        pixel != nullptr && pitch > 0 ? pitch : pitch_);
    std::vector<uint32_t> expanded_gray;
    if(format_ == TVPTextureFormat::Gray) {
        if(src == nullptr) return;
        expanded_gray.resize(static_cast<size_t>(Width) * Height);
        const auto *gray = static_cast<const uint8_t *>(src);
        for(int y = 0; y < Height; ++y) {
            for(int x = 0; x < Width; ++x) {
                const uint32_t value = gray[static_cast<size_t>(y) * stride + x];
                expanded_gray[static_cast<size_t>(y) * Width + x] =
                    value | (value << 8) | (value << 16) | 0xff000000u;
            }
        }
        src = expanded_gray.data();
        stride = static_cast<uint32_t>(Width) * 4u;
    } else if(format_ != TVPTextureFormat::RGBA) {
        return;
    }

    // Engine CPU pixels are R,G,B,A in memory (little-endian), which matches
    // SDL_PIXELFORMAT_ABGR8888 (see AGENTS.md). Streaming textures let the
    // composited frame be written/read back directly with SDL_LockTexture.
    SDL_Texture *tex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
        static_cast<int>(Width), static_cast<int>(Height));
    if (tex == nullptr) {
        return;
    }
    // SDL3 defaults to BLEND: alpha=0 regions would show the host's
    // RenderClear through. The frame is fully composited, so present with
    // NONE (same as the software path).
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    void *dst = nullptr;
    int tex_pitch = 0;
    if (SDL_LockTexture(tex, nullptr, &dst, &tex_pitch)) {
        std::memset(dst, 0, static_cast<size_t>(tex_pitch) * Height);
        if (src != nullptr) {
            const uint32_t copy_bytes = std::min<uint32_t>(
                static_cast<uint32_t>(tex_pitch), stride);
            const auto *src_bytes = static_cast<const uint8_t *>(src);
            auto *dst_bytes = static_cast<uint8_t *>(dst);
            for (int y = 0; y < Height; ++y) {
                std::memcpy(dst_bytes + static_cast<size_t>(y) * tex_pitch,
                            src_bytes + static_cast<size_t>(y) * stride,
                            copy_bytes);
            }
        }
        SDL_UnlockTexture(tex);
    }
    gpu_handle_ = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tex));
    gpu_dirty_ = false;
    cpu_dirty_ = false;
    if(format_ == TVPTextureFormat::RGBA) DiscardCpuStorage();
}

bool SDLTexture2D::EnsureGpuHandle() {
    if (gpu_handle_ == 0) {
        CreateGpuHandle(nullptr, 0);
    } else if (cpu_dirty_) {
        return UploadCpuToGpu(false);
    }
    return gpu_handle_ != 0;
}

namespace {
// Synchronous SDL readback slots: begin reads the streaming texture into a
// request-owned buffer; poll copies it out; discard frees it.
std::mutex g_sdl_readback_mutex;
uint64_t g_sdl_next_readback_id = 1;
std::unordered_map<uint64_t, std::vector<uint8_t>> g_sdl_readbacks;
}  // namespace

uint64_t SDLTexture2D::BeginGpuReadback() const {
    SDL_Texture *tex = reinterpret_cast<SDL_Texture *>(
        static_cast<uintptr_t>(gpu_handle_));
    if (tex == nullptr) {
        return 0;
    }
    void *src = nullptr;
    int tex_pitch = 0;
    if (!SDL_LockTexture(tex, nullptr, &src, &tex_pitch)) {
        return 0;
    }
    std::vector<uint8_t> data(static_cast<size_t>(tex_pitch) * Height);
    std::memcpy(data.data(), src, data.size());
    SDL_UnlockTexture(tex);
    std::lock_guard<std::mutex> lock(g_sdl_readback_mutex);
    const uint64_t id = g_sdl_next_readback_id++;
    if (id == 0) g_sdl_next_readback_id = 1;
    g_sdl_readbacks[id] = std::move(data);
    return id;
}

bool SDLTexture2D::PollGpuReadback(
    uint64_t request, void *out_pixels, size_t out_pixels_size,
    uint32_t stride_bytes, bool *ready) const {
    if (ready != nullptr) {
        *ready = false;
    }
    if (request == 0 || out_pixels == nullptr) {
        return false;
    }
    std::vector<uint8_t> data;
    {
        std::lock_guard<std::mutex> lock(g_sdl_readback_mutex);
        const auto it = g_sdl_readbacks.find(request);
        if (it == g_sdl_readbacks.end()) {
            return false;
        }
        data = it->second;
    }
    if (ready != nullptr) {
        *ready = true;
    }
    const uint32_t tight_stride = static_cast<uint32_t>(Width) * 4u;
    const uint32_t dst_stride = stride_bytes != 0 ? stride_bytes : tight_stride;
    if (out_pixels_size < static_cast<size_t>(dst_stride) * Height) {
        return false;
    }
    auto *dst = static_cast<uint8_t *>(out_pixels);
    for (int y = 0; y < Height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * dst_stride,
                    data.data() + static_cast<size_t>(y) * tight_stride,
                    tight_stride);
    }
    return true;
}

void SDLTexture2D::DiscardGpuReadback(uint64_t request) const {
    if (request == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_sdl_readback_mutex);
    g_sdl_readbacks.erase(request);
}

void SDLTexture2D::ReleaseGpuHandle() {
    if (gpu_handle_ == 0) return;
    TVPQueueSdlTextureRelease(reinterpret_cast<SDL_Texture *>(
        static_cast<uintptr_t>(gpu_handle_)));
    gpu_handle_ = 0;
    gpu_dirty_ = false;
    cpu_dirty_ = false;
}

void SDLTexture2D::EnsureCpuReadable() {
    if (cpu_dirty_) {
        EnsureCpuStorage();
        return;
    }
    if (gpu_handle_ == 0) {
        EnsureCpuStorage();
        return;
    }
    if (!gpu_dirty_ && !pixels_.empty()) return;
    EnsureCpuStorage();
    SDL_Texture *tex = reinterpret_cast<SDL_Texture *>(
        static_cast<uintptr_t>(gpu_handle_));
    if (tex == nullptr) {
        return;
    }
    void *src = nullptr;
    int tex_pitch = 0;
    if (!SDL_LockTexture(tex, nullptr, &src, &tex_pitch)) {
        return;
    }
    if (format_ == TVPTextureFormat::Gray) {
        const uint32_t rgba_pitch = static_cast<uint32_t>(Width) * 4u;
        const auto *source = static_cast<const uint8_t *>(src);
        auto *destination = pixels_.data();
        for (int y = 0; y < Height; ++y) {
            const auto *row =
                source + static_cast<size_t>(y) * tex_pitch;
            for (int x = 0; x < Width; ++x) {
                destination[static_cast<size_t>(y) * pitch_ + x] =
                    row[static_cast<size_t>(x) * 4];
            }
        }
        gpu_dirty_ = false;
    } else if (format_ == TVPTextureFormat::RGBA) {
        const uint32_t copy_bytes = std::min<uint32_t>(
            static_cast<uint32_t>(tex_pitch), static_cast<uint32_t>(pitch_));
        const auto *source = static_cast<const uint8_t *>(src);
        auto *destination = pixels_.data();
        for (int y = 0; y < Height; ++y) {
            std::memcpy(destination + static_cast<size_t>(y) * pitch_,
                        source + static_cast<size_t>(y) * tex_pitch,
                        copy_bytes);
        }
        gpu_dirty_ = false;
    }
    SDL_UnlockTexture(tex);
}

const void *SDLTexture2D::GetScanLineForRead(tjs_uint l) {
    EnsureCpuReadable();
    if (l >= static_cast<tjs_uint>(Height) || pixels_.empty()) return nullptr;
    return pixels_.data() + static_cast<size_t>(l) * pitch_;
}

void *SDLTexture2D::GetScanLineForWrite(tjs_uint l) {
    EnsureCpuReadable();
    if (l >= static_cast<tjs_uint>(Height) || pixels_.empty()) return nullptr;
    MarkOpacityUnknown();
    MarkCpuDirty();
    return pixels_.data() + static_cast<size_t>(l) * pitch_;
}

void SDLTexture2D::Update(const void *pixel, TVPTextureFormat::e format,
                            int pitch, const tTVPRect &rc) {
    if (pixel == nullptr) return;
    const int new_bpp = BytesPerPixel(format);
    if(format != format_) {
        ReleaseGpuHandle();
        format_ = format;
        pitch_ = Width * new_bpp;
        pixels_.assign(static_cast<size_t>(pitch_) * Height, 0);
    }
    const bool full_rect = IsFullTextureRect(rc, Width, Height);
    const bool replace_transient_scratch =
        !full_rect && discard_unwritten_on_partial_update_ &&
        rc.left == 0 && rc.top == 0;
    if (!full_rect && !replace_transient_scratch) {
        EnsureCpuReadable();
    } else {
        if (replace_transient_scratch) {
            pixels_.clear();
        }
        EnsureCpuStorage();
    }
    const int src_pitch = pitch > 0 ? pitch : rc.get_width() * new_bpp;
    const bool copied = CopyRect(pixels_.data(), pitch_, Width, Height,
                                 static_cast<const uint8_t *>(pixel), src_pitch,
                                 new_bpp, rc);
    if (!copied) return;
    if (full_rect) {
        SetOpacityFromPixels(pixels_.data(), pitch_);
    } else {
        MarkOpacityUnknown();
    }
    MarkCpuDirty();
}

uint32_t SDLTexture2D::GetPoint(int x, int y) {
    if (x < 0 || y < 0 || x >= Width || y >= Height ||
        (format_ != TVPTextureFormat::Gray &&
         format_ != TVPTextureFormat::RGBA)) {
        return 0;
    }
    EnsureCpuReadable();
    if (pixels_.empty()) return 0;
    const size_t offset = static_cast<size_t>(y) * pitch_;
    if (format_ == TVPTextureFormat::Gray) {
        return pixels_[offset + static_cast<size_t>(x)];
    }
    uint32_t value = 0;
    std::memcpy(&value, pixels_.data() + offset + static_cast<size_t>(x) * 4,
                4);
    return value;
}

void SDLTexture2D::SetPoint(int x, int y, uint32_t clr) {
    if (x < 0 || y < 0 || x >= Width || y >= Height ||
        (format_ != TVPTextureFormat::Gray &&
         format_ != TVPTextureFormat::RGBA)) {
        return;
    }
    EnsureCpuReadable();
    if (pixels_.empty()) return;
    const size_t offset = static_cast<size_t>(y) * pitch_;
    if (format_ == TVPTextureFormat::Gray) {
        pixels_[offset + static_cast<size_t>(x)] = static_cast<uint8_t>(clr);
    } else {
        std::memcpy(pixels_.data() + offset + static_cast<size_t>(x) * 4,
                    &clr, 4);
        MarkOpacityUnknown();
    }
    MarkCpuDirty();
}

void SDLTexture2D::SetSize(unsigned int w, unsigned int h) {
    ReleaseGpuHandle();
    Width = static_cast<tjs_int>(w);
    Height = static_cast<tjs_int>(h);
    pitch_ = static_cast<int>(w) * BytesPerPixel(format_);
    pixels_.assign(static_cast<size_t>(pitch_) * h, 0);
    MarkTransparentKnown();
    MarkCpuDirty();
}

bool SDLTexture2D::ClearGpu(uint32_t rgba, const tTVPRect &rc) {
    SDL_Texture *tex = reinterpret_cast<SDL_Texture *>(
        static_cast<uintptr_t>(gpu_handle_));
    if (tex == nullptr || format_ != TVPTextureFormat::RGBA) return false;
    const uint32_t width = static_cast<uint32_t>(rc.get_width());
    const uint32_t height = static_cast<uint32_t>(rc.get_height());
    if (width == 0 || height == 0) return false;
    SDL_Rect sdl_rect{rc.left, rc.top, static_cast<int>(width),
                      static_cast<int>(height)};
    void *dst = nullptr;
    int tex_pitch = 0;
    if (!SDL_LockTexture(tex, &sdl_rect, &dst, &tex_pitch)) {
        return false;
    }
    // Script-facing AARRGGBB was converted by the caller to the backend's
    // AABBGGRR value; the ABGR8888 texture stores bytes as R,G,B,A on
    // little-endian, i.e. the 32-bit value's memory layout.
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(rgba), static_cast<uint8_t>(rgba >> 8u),
        static_cast<uint8_t>(rgba >> 16u), static_cast<uint8_t>(rgba >> 24u)};
    auto *out = static_cast<uint8_t *>(dst);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = out + static_cast<size_t>(y) * tex_pitch;
        for (uint32_t x = 0; x < width; ++x) {
            std::memcpy(row + static_cast<size_t>(x) * 4u, bytes, 4u);
        }
    }
    SDL_UnlockTexture(tex);
    gpu_dirty_ = true;
    cpu_dirty_ = false;
    if (IsFullTextureRect(rc, Width, Height)) {
        ((rgba >> 24) & 0xffu) == 0xffu ? MarkOpaqueKnown()
                                         : MarkTransparentKnown();
    } else {
        MarkOpacityUnknown();
    }
    return true;
}

bool SDLTexture2D::CopyGpuFrom(SDLTexture2D *src, const tTVPRect &dst_rc,
                                 const tTVPRect &src_rc) {
    // GPU compositing operations were provided by the removed Godot GPU
    // bridge; report failure so callers fall back to the software path.
    (void)src;
    (void)dst_rc;
    (void)src_rc;
    return false;
}

bool SDLTexture2D::CopyTrianglesGpuFrom(SDLTexture2D *src,
                                          uint32_t triangle_count,
                                          const tTVPRect &clip_rc,
                                          const tTVPPointD *dst_points,
                                          const tTVPPointD *src_points) {
    (void)src;
    (void)triangle_count;
    (void)clip_rc;
    (void)dst_points;
    (void)src_points;
    return false;
}

bool SDLTexture2D::BlendTrianglesGpuFrom(
    SDLTexture2D *src, uint32_t triangle_count, const tTVPRect &clip_rc,
    const tTVPPointD *dst_points, const tTVPPointD *src_points, uint32_t mode,
    int opacity) {
    (void)src;
    (void)triangle_count;
    (void)clip_rc;
    (void)dst_points;
    (void)src_points;
    (void)mode;
    (void)opacity;
    return false;
}

bool SDLTexture2D::DrawTrianglesGpuFrom(SDLTexture2D *src,
                                          uint32_t triangle_count,
                                          const tTVPRect &clip_rc,
                                          const tTVPPointD *dst_points,
                                          const tTVPPointD *src_points,
                                          int opacity,
                                          uint32_t blend_mode) {
    (void)src;
    (void)triangle_count;
    (void)clip_rc;
    (void)dst_points;
    (void)src_points;
    (void)opacity;
    (void)blend_mode;
    return false;
}

bool SDLTexture2D::DrawMaskedTrianglesGpuFrom(
    SDLTexture2D *src, SDLTexture2D *mask,
    uint32_t triangle_count, const tTVPRect &clip_rc,
    const tTVPPointD *dst_points, const tTVPPointD *src_points,
    const tTVPPointD *mask_points, int opacity, uint32_t blend_mode,
    bool use_mask_alpha) {
    (void)src;
    (void)mask;
    (void)triangle_count;
    (void)clip_rc;
    (void)dst_points;
    (void)src_points;
    (void)mask_points;
    (void)opacity;
    (void)blend_mode;
    (void)use_mask_alpha;
    return false;
}

bool SDLTexture2D::BlendGpuFrom(SDLTexture2D *src, const tTVPRect &dst_rc,
                                  const tTVPRect &src_rc, uint32_t mode,
                                  int opacity, uint32_t color) {
    (void)src;
    (void)dst_rc;
    (void)src_rc;
    (void)mode;
    (void)opacity;
    (void)color;
    return false;
}

bool SDLTexture2D::BlendGpuFrom2(SDLTexture2D *src1, SDLTexture2D *src2,
                                   const tTVPRect &dst_rc,
                                   const tTVPRect &src1_rc,
                                   const tTVPRect &src2_rc, uint32_t mode,
                                   int opacity, uint32_t color) {
    (void)src1;
    (void)src2;
    (void)dst_rc;
    (void)src1_rc;
    (void)src2_rc;
    (void)mode;
    (void)opacity;
    (void)color;
    return false;
}

bool SDLTexture2D::BlendGpuFrom3(
    SDLTexture2D *src1, SDLTexture2D *src2, SDLTexture2D *src3,
    const tTVPRect &dst_rc, const tTVPRect &src1_rc,
    const tTVPRect &src2_rc, const tTVPRect &src3_rc, uint32_t mode,
    int opacity, uint32_t color) {
    (void)src1;
    (void)src2;
    (void)src3;
    (void)dst_rc;
    (void)src1_rc;
    (void)src2_rc;
    (void)src3_rc;
    (void)mode;
    (void)opacity;
    (void)color;
    return false;
}

bool SDLTexture2D::UploadCpuToGpu(bool flush_pending_gpu_writes) {
    (void)flush_pending_gpu_writes;
    if (!cpu_dirty_) {
        return true;
    }
    if (format_ != TVPTextureFormat::RGBA || pixels_.empty()) {
        return false;
    }
    SDL_Texture *tex = reinterpret_cast<SDL_Texture *>(
        static_cast<uintptr_t>(gpu_handle_));
    if (tex == nullptr) {
        CreateGpuHandle(pixels_.data(), pitch_);
        return gpu_handle_ != 0;
    }
    void *dst = nullptr;
    int tex_pitch = 0;
    if (!SDL_LockTexture(tex, nullptr, &dst, &tex_pitch)) {
        return false;
    }
    const uint32_t copy_bytes = std::min<uint32_t>(
        static_cast<uint32_t>(tex_pitch), static_cast<uint32_t>(pitch_));
    const auto *src = pixels_.data();
    auto *out = static_cast<uint8_t *>(dst);
    for (int y = 0; y < Height; ++y) {
        std::memcpy(out + static_cast<size_t>(y) * tex_pitch,
                    src + static_cast<size_t>(y) * pitch_, copy_bytes);
    }
    SDL_UnlockTexture(tex);
    gpu_dirty_ = false;
    cpu_dirty_ = false;
    DiscardCpuStorage();
    return true;
}

iTVPTexture2D *SDLRenderManager::CreateTexture2D(const void *pixel, int pitch,
                                                   unsigned int w,
                                                   unsigned int h,
                                                   TVPTextureFormat::e format,
                                                   int flags) {
    auto *texture = new SDLTexture2D(pixel, pitch, w, h, format, flags);
    vmem_size_ += static_cast<uint64_t>(texture->GetPitch()) * h;
    g_texture_create_count.fetch_add(1, std::memory_order_relaxed);
    return texture;
}

iTVPTexture2D *SDLRenderManager::CreateTexture2D(tTVPBitmap *bmp) {
    if (bmp == nullptr) {
        return CreateTexture2D(nullptr, 0, 1, 1, TVPTextureFormat::RGBA);
    }
    return CreateTexture2D(bmp->GetScanLine(0), bmp->GetPitch(),
                           bmp->GetWidth(), bmp->GetHeight(),
                           bmp->GetBPP() == 8 ? TVPTextureFormat::Gray
                                               : TVPTextureFormat::RGBA);
}

iTVPTexture2D *SDLRenderManager::CreateTexture2D(TJS::tTJSBinaryStream *) {
    return CreateTexture2D(nullptr, 0, 1, 1, TVPTextureFormat::RGBA);
}

iTVPTexture2D *SDLRenderManager::CreateTexture2D(unsigned int neww,
                                                   unsigned int newh,
                                                   iTVPTexture2D *tex) {
    g_texture_clone_count.fetch_add(1, std::memory_order_relaxed);
    if (tex == nullptr || tex->GetWidth() != static_cast<tjs_int>(neww) ||
        tex->GetHeight() != static_cast<tjs_int>(newh)) {
        g_texture_resize_count.fetch_add(1, std::memory_order_relaxed);
    }
    auto *ret = new SDLTexture2D(nullptr, 0, neww, newh,
                                  tex != nullptr ? tex->GetFormat()
                                                 : TVPTextureFormat::RGBA);
    if (tex != nullptr) {
        const tTVPRect copy_rc(0, 0,
                               std::min<tjs_int>(neww, tex->GetWidth()),
                               std::min<tjs_int>(newh, tex->GetHeight()));
        if (!copy_rc.is_empty()) {
            // Copy-on-write and grow-only motion scratch layers arrive here
            // while their newest pixels still live exclusively on the GPU.
            // Reading scan line zero would synchronously download the entire
            // Metal texture before uploading it into the replacement texture.
            // Keep that clone on the ordered GPU queue whenever both textures
            // use this backend.
            auto *sdl_src = dynamic_cast<SDLTexture2D *>(tex);
            if (sdl_src != nullptr &&
                ret->EnsureGpuHandle() && sdl_src->EnsureGpuHandle() &&
                sdl_src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
                ret->CopyGpuFrom(sdl_src, copy_rc, copy_rc)) {
                return ret;
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

iTVPRenderMethod *SDLRenderManager::GetRenderMethod(const char *name,
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

bool SDLRenderManager::GetRenderStat(unsigned int &drawCount,
                                       uint64_t &vmemsize) {
    unsigned int delegate_draws = 0;
    uint64_t delegate_vmem = 0;
    const bool ok = SoftwareDelegate()->GetRenderStat(delegate_draws, delegate_vmem);
    drawCount = draw_count_ + delegate_draws;
    draw_count_ = 0;
    vmemsize = std::max(vmem_size_, delegate_vmem);
    return ok;
}

bool SDLRenderManager::GetTextureStat(iTVPTexture2D *texture,
                                        uint64_t &vmemsize) {
    return SoftwareDelegate()->GetTextureStat(texture, vmemsize);
}

int SDLRenderManager::EnumParameterID(const char *name) {
    const int id = SoftwareDelegate()->EnumParameterID(name);
    if(name != nullptr && std::strcmp(name, "StretchType") == 0)
        stretch_parameter_id_ = id;
    return id;
}

void SDLRenderManager::SetParameterUInt(int id, unsigned int Value) {
    SoftwareDelegate()->SetParameterUInt(id, Value);
}

void SDLRenderManager::SetParameterInt(int id, int Value) {
    if(id >= 0 && id == stretch_parameter_id_)
        stretch_type_ = Value;
    SoftwareDelegate()->SetParameterInt(id, Value);
}

void SDLRenderManager::SetParameterPtr(int id, const void *Value) {
    SoftwareDelegate()->SetParameterPtr(id, Value);
}

void SDLRenderManager::SetParameterFloat(int id, float Value) {
    SoftwareDelegate()->SetParameterFloat(id, Value);
}

void SDLRenderManager::OperateRect(iTVPRenderMethod *method, iTVPTexture2D *tar,
                                     iTVPTexture2D *reftar,
                                     const tTVPRect &rctar,
                                     const tRenderTexRectArray &textures) {
    ++draw_count_;
    auto *godot_method = dynamic_cast<SDLRenderMethod *>(method);
    iTVPRenderMethod *delegate_method =
        godot_method != nullptr ? godot_method->Delegate() : method;
    const std::string method_name =
        method != nullptr ? method->GetName() : std::string();

    auto *dst = dynamic_cast<SDLTexture2D *>(tar);
    auto *src = textures.size() == 1
        ? dynamic_cast<SDLTexture2D *>(textures[0].first)
        : nullptr;
    auto *src1 = textures.size() == 2
        ? dynamic_cast<SDLTexture2D *>(textures[0].first)
        : nullptr;
    auto *src2 = textures.size() == 2
        ? dynamic_cast<SDLTexture2D *>(textures[1].first)
        : nullptr;
    auto *src3_1 = textures.size() == 3
        ? dynamic_cast<SDLTexture2D *>(textures[0].first)
        : nullptr;
    auto *src3_2 = textures.size() == 3
        ? dynamic_cast<SDLTexture2D *>(textures[1].first)
        : nullptr;
    auto *src3_3 = textures.size() == 3
        ? dynamic_cast<SDLTexture2D *>(textures[2].first)
        : nullptr;
    const bool nearest_scaled = textures.size() == 1 &&
        !RectAbsSizeMatches(rctar, textures[0].second) &&
        (stretch_type_ & stTypeMask) == stNearest;

    if (method_name == "Copy" && dst != nullptr && src != nullptr &&
        IsGpuRectFastPathEnabled("Copy") &&
        ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst, src) &&
        dst->EnsureGpuHandle() && src->EnsureGpuHandle() &&
        src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled())) {
        const tTVPRect &src_rc = textures[0].second;
        if (!RectBoundsInsideTexture(src_rc, src)) {
            CountCopyFallbackReason("copy_src_out_of_bounds");
            CountMethodFallback(method);
            SoftwareDelegate()->OperateRect(delegate_method, tar, reftar, rctar, textures);
            if (dst != nullptr) {
                dst->MarkCpuDirty();
            }
            return;
        }
        if (src_rc.get_width() == rctar.get_width() &&
            src_rc.get_height() == rctar.get_height() &&
            dst->CopyGpuFrom(src, rctar, src_rc)) {
            CountGpuFastPath(method_name);
            return;
        }
        if(nearest_scaled) {
            CountCopyFallbackReason("scaled_copy_nearest");
            CountMethodFallback(method);
            SoftwareDelegate()->OperateRect(delegate_method, tar, reftar,
                                            rctar, textures);
            dst->MarkCpuDirty();
            return;
        }
        const bool needs_area_downsample =
            RectNeedsAlphaAreaDownsample(rctar, src_rc, src);
        if (!needs_area_downsample && !src_rc.is_empty() &&
            IsGpuCopyTrianglesEnabled()) {
            const tTVPPointD dst_pt[6] = {
                {static_cast<double>(rctar.left), static_cast<double>(rctar.top)},
                {static_cast<double>(rctar.right), static_cast<double>(rctar.top)},
                {static_cast<double>(rctar.left), static_cast<double>(rctar.bottom)},
                {static_cast<double>(rctar.right), static_cast<double>(rctar.top)},
                {static_cast<double>(rctar.left), static_cast<double>(rctar.bottom)},
                {static_cast<double>(rctar.right), static_cast<double>(rctar.bottom)},
            };
            const tTVPPointD src_pt[6] = {
                {static_cast<double>(src_rc.left), static_cast<double>(src_rc.top)},
                {static_cast<double>(src_rc.right), static_cast<double>(src_rc.top)},
                {static_cast<double>(src_rc.left), static_cast<double>(src_rc.bottom)},
                {static_cast<double>(src_rc.right), static_cast<double>(src_rc.top)},
                {static_cast<double>(src_rc.left), static_cast<double>(src_rc.bottom)},
                {static_cast<double>(src_rc.right), static_cast<double>(src_rc.bottom)},
            };
            if (dst->CopyTrianglesGpuFrom(src, 2, rctar, dst_pt, src_pt)) {
                CountGpuFastPath(method_name);
                return;
            }
        }
        if (!RectAbsSizeMatches(rctar, src_rc)) {
            CountCopyFallbackReason(needs_area_downsample
                                        ? "scaled_copy_area_downsample"
                                        : IsGpuCopyTrianglesEnabled()
                                              ? "scaled_copy_bridge_failed"
                                              : "scaled_copy_cpu");
            CountMethodFallback(method);
            SoftwareDelegate()->OperateRect(delegate_method, tar, reftar, rctar, textures);
            if (dst != nullptr) {
                dst->MarkCpuDirty();
            }
            return;
        }
        const double sx0 = src_rc.get_width() < 0 ? src_rc.left - 1 : src_rc.left;
        const double sx1 = src_rc.get_width() < 0 ? src_rc.right - 1 : src_rc.right;
        const double sy0 = src_rc.get_height() < 0 ? src_rc.top - 1 : src_rc.top;
        const double sy1 = src_rc.get_height() < 0 ? src_rc.bottom - 1 : src_rc.bottom;
        const tTVPPointD dst_pt[6] = {
            {static_cast<double>(rctar.left), static_cast<double>(rctar.top)},
            {static_cast<double>(rctar.right), static_cast<double>(rctar.top)},
            {static_cast<double>(rctar.left), static_cast<double>(rctar.bottom)},
            {static_cast<double>(rctar.right), static_cast<double>(rctar.top)},
            {static_cast<double>(rctar.left), static_cast<double>(rctar.bottom)},
            {static_cast<double>(rctar.right), static_cast<double>(rctar.bottom)},
        };
        const tTVPPointD src_pt[6] = {
            {sx0, sy0},
            {sx1, sy0},
            {sx0, sy1},
            {sx1, sy0},
            {sx0, sy1},
            {sx1, sy1},
        };
        if (dst->CopyTrianglesGpuFrom(src, 2, rctar, dst_pt, src_pt)) {
            CountGpuFastPath(method_name);
            return;
        }
    }

    if (method_name == "CopyColor" && dst != nullptr && src != nullptr &&
        IsGpuRectFastPathEnabled("CopyColor") &&
        ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst, src) &&
        RectAbsSizeMatches(rctar, textures[0].second) &&
        RectBoundsInsideTexture(textures[0].second, src) &&
        dst->EnsureGpuHandle() && src->EnsureGpuHandle() &&
        src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
        dst->BlendGpuFrom(src, rctar, textures[0].second,
                          TVP_GODOT_GPU_BLEND_COPY_COLOR, 255, 0)) {
        CountGpuFastPath(method_name);
        return;
    }

    if (method_name == "AlphaBlend" && dst != nullptr && src != nullptr &&
        !nearest_scaled &&
        IsGpuRectFastPathEnabled("AlphaBlend") &&
        ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst, src) &&
        RectBoundsInsideTexture(textures[0].second, src) &&
        dst->EnsureGpuHandle() && src->EnsureGpuHandle() &&
        src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
        dst->BlendGpuFrom(src, rctar, textures[0].second,
                          TVP_GODOT_GPU_BLEND_ALPHA,
                          godot_method != nullptr ? godot_method->Opacity() : 255,
                          0)) {
        CountGpuFastPath(method_name);
        return;
    }

    if (method_name == "AlphaBlend_d" && dst != nullptr && src != nullptr &&
        !nearest_scaled &&
        IsGpuRectFastPathEnabled("AlphaBlend_d") &&
        ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst, src) &&
        RectBoundsInsideTexture(textures[0].second, src) &&
        dst->EnsureGpuHandle() && src->EnsureGpuHandle() &&
        src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
        dst->BlendGpuFrom(src, rctar, textures[0].second,
                          TVP_GODOT_GPU_BLEND_ALPHA_D,
                          godot_method != nullptr ? godot_method->Opacity() : 255,
                          0)) {
        CountGpuFastPath(method_name);
        return;
    }

    if (method_name == "ConstAlphaBlend_d" && dst != nullptr && src != nullptr &&
        IsGpuRectFastPathEnabled("ConstAlphaBlend_d") &&
        ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst, src) &&
        RectAbsSizeMatches(rctar, textures[0].second) &&
        RectBoundsInsideTexture(textures[0].second, src) &&
        dst->EnsureGpuHandle() && src->EnsureGpuHandle() &&
        src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
        dst->BlendGpuFrom(src, rctar, textures[0].second,
                          TVP_GODOT_GPU_BLEND_CONST_ALPHA_D,
                          godot_method != nullptr ? godot_method->Opacity() : 255,
                          0)) {
        CountGpuFastPath(method_name);
        return;
    }

    if (method_name == "PsScreenBlend" && dst != nullptr && src != nullptr &&
        IsGpuRectFastPathEnabled("PsScreenBlend") &&
        ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst, src) &&
        RectAbsSizeMatches(rctar, textures[0].second) &&
        RectBoundsInsideTexture(textures[0].second, src) &&
        dst->EnsureGpuHandle() && src->EnsureGpuHandle() &&
        src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
        dst->BlendGpuFrom(src, rctar, textures[0].second,
                          TVP_GODOT_GPU_BLEND_PS_SCREEN,
                          godot_method != nullptr ? godot_method->Opacity() : 255,
                          0)) {
        CountGpuFastPath(method_name);
        return;
    }

    if (method_name == "PsAddBlend" && dst != nullptr && src != nullptr &&
        IsGpuRectFastPathEnabled("PsAddBlend") &&
        ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst, src) &&
        RectAbsSizeMatches(rctar, textures[0].second) &&
        RectBoundsInsideTexture(textures[0].second, src) &&
        dst->EnsureGpuHandle() && src->EnsureGpuHandle() &&
        src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
        dst->BlendGpuFrom(src, rctar, textures[0].second,
                          TVP_GODOT_GPU_BLEND_PS_ADD,
                          godot_method != nullptr ? godot_method->Opacity() : 255,
                          0)) {
        CountGpuFastPath(method_name);
        return;
    }

    if (method_name == "PsSubBlend" && dst != nullptr && src != nullptr &&
        IsGpuRectFastPathEnabled("PsSubBlend") &&
        ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst, src) &&
        RectAbsSizeMatches(rctar, textures[0].second) &&
        RectBoundsInsideTexture(textures[0].second, src) &&
        dst->EnsureGpuHandle() && src->EnsureGpuHandle() &&
        src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
        dst->BlendGpuFrom(src, rctar, textures[0].second,
                          TVP_GODOT_GPU_BLEND_PS_SUBTRACT,
                          godot_method != nullptr ? godot_method->Opacity() : 255,
                          0)) {
        CountGpuFastPath(method_name);
        return;
    }

    if (method_name == "PsMulBlend" && dst != nullptr && src != nullptr &&
        IsGpuRectFastPathEnabled("PsMulBlend") &&
        ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst, src) &&
        RectAbsSizeMatches(rctar, textures[0].second) &&
        RectBoundsInsideTexture(textures[0].second, src) &&
        dst->EnsureGpuHandle() && src->EnsureGpuHandle() &&
        src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
        dst->BlendGpuFrom(src, rctar, textures[0].second,
                          TVP_GODOT_GPU_BLEND_PS_MULTIPLY,
                          godot_method != nullptr ? godot_method->Opacity() : 255,
                          0)) {
        CountGpuFastPath(method_name);
        return;
    }

    if ((method_name == "AlphaBlend_a" ||
        method_name == "PerspectiveAlphaBlend_a") &&
        dst != nullptr && src != nullptr && !nearest_scaled &&
        IsGpuRectFastPathEnabled("AlphaBlend_a") &&
        ShouldUseGpuRectFastPath(rctar, "AlphaBlend_a", dst, src) &&
        dst->EnsureGpuHandle() && src->EnsureGpuHandle() &&
        src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled())) {
        const tTVPRect &src_rc = textures[0].second;
        const int opacity = godot_method != nullptr ? godot_method->Opacity() : 255;
        if (!RectBoundsInsideTexture(src_rc, src)) {
            CountCopyFallbackReason("alpha_blend_a_src_out_of_bounds");
            CountMethodFallback(method);
            SoftwareDelegate()->OperateRect(delegate_method, tar, reftar, rctar, textures);
            if (dst != nullptr) {
                dst->MarkCpuDirty();
            }
            return;
        }
        if (IsOpaqueAlphaBlendCopyEnabled() && opacity == 255 && src->IsOpaque()) {
            if (src_rc.get_width() == rctar.get_width() &&
                src_rc.get_height() == rctar.get_height() &&
                dst->CopyGpuFrom(src, rctar, src_rc)) {
                CountGpuFastPath(method_name + ":CopyOpaque");
                return;
            }
            const bool needs_area_downsample =
                RectNeedsAlphaAreaDownsample(rctar, src_rc, src);
            if (!needs_area_downsample && !src_rc.is_empty() &&
                IsGpuCopyTrianglesEnabled()) {
                const tTVPPointD dst_pt[6] = {
                    {static_cast<double>(rctar.left), static_cast<double>(rctar.top)},
                    {static_cast<double>(rctar.right), static_cast<double>(rctar.top)},
                    {static_cast<double>(rctar.left), static_cast<double>(rctar.bottom)},
                    {static_cast<double>(rctar.right), static_cast<double>(rctar.top)},
                    {static_cast<double>(rctar.left), static_cast<double>(rctar.bottom)},
                    {static_cast<double>(rctar.right), static_cast<double>(rctar.bottom)},
                };
                const tTVPPointD src_pt[6] = {
                    {static_cast<double>(src_rc.left), static_cast<double>(src_rc.top)},
                    {static_cast<double>(src_rc.right), static_cast<double>(src_rc.top)},
                    {static_cast<double>(src_rc.left), static_cast<double>(src_rc.bottom)},
                    {static_cast<double>(src_rc.right), static_cast<double>(src_rc.top)},
                    {static_cast<double>(src_rc.left), static_cast<double>(src_rc.bottom)},
                    {static_cast<double>(src_rc.right), static_cast<double>(src_rc.bottom)},
                };
                if (dst->CopyTrianglesGpuFrom(src, 2, rctar, dst_pt, src_pt)) {
                    CountGpuFastPath(method_name + ":CopyOpaque");
                    return;
                }
            }
        }
        // The Godot bridge performs scaled AlphaBlend_a with premultiplied
        // bilinear sampling. Keeping area downscales on the CPU forces a full
        // GPU readback/upload between every transparent motion layer, which is
        // especially expensive for the 0.75x SD animations used by Yuzu games.
        if (dst->BlendGpuFrom(src, rctar, src_rc,
                          TVP_GODOT_GPU_BLEND_ALPHA_BLEND_A,
                          opacity, 0)) {
            CountGpuFastPath(method_name);
            return;
        }
    }

    if (method_name == "FillARGB" && dst != nullptr &&
        IsGpuRectFastPathEnabled("FillARGB") &&
        ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst) &&
        dst->EnsureGpuHandle() &&
        dst->ClearGpu(godot_method != nullptr ? godot_method->Color() : 0,
                      rctar)) {
        CountGpuFastPath(method_name);
        return;
    }

    if (method_name == "FillMask" && dst != nullptr &&
        IsGpuRectFastPathEnabled("FillMask") &&
        ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst, dst) &&
        dst->EnsureGpuHandle() &&
        dst->BlendGpuFrom(dst, rctar, rctar,
                          TVP_GODOT_GPU_BLEND_FILL_MASK,
                          godot_method != nullptr ? godot_method->Opacity() : 255,
                          0)) {
        CountGpuFastPath(method_name);
        return;
    }

    if (method_name == "RemoveConstOpacity" && dst != nullptr &&
        IsGpuRectFastPathEnabled("RemoveConstOpacity") &&
        ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst, dst) &&
        dst->EnsureGpuHandle() &&
        dst->BlendGpuFrom(dst, rctar, rctar,
                          TVP_GODOT_GPU_BLEND_REMOVE_CONST_OPACITY,
                          godot_method != nullptr ? godot_method->Opacity() : 255,
                          0)) {
        CountGpuFastPath(method_name);
        return;
    }

    if ((method_name == "ConstAlphaBlend_SD" ||
         method_name == "ConstAlphaBlend_SD_d") &&
        dst != nullptr && src1 != nullptr && src2 != nullptr &&
        IsGpuRectFastPathEnabled(method_name.c_str()) &&
        ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst, src1, src2) &&
        RectBoundsInsideTexture(textures[0].second, src1) &&
        RectBoundsInsideTexture(textures[1].second, src2) &&
        dst->EnsureGpuHandle() && src1->EnsureGpuHandle() &&
        src2->EnsureGpuHandle() &&
        src1->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
        src2->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
        dst->BlendGpuFrom2(
            src1, src2, rctar, textures[0].second, textures[1].second,
            method_name == "ConstAlphaBlend_SD_d"
                ? TVP_GODOT_GPU_BLEND_CONST_ALPHA_SD_D
                : TVP_GODOT_GPU_BLEND_CONST_ALPHA_SD,
            godot_method != nullptr ? godot_method->Opacity() : 255, 0)) {
        CountGpuFastPath(method_name);
        return;
    }

    if((method_name == "UnivTransBlend" ||
        method_name == "UnivTransBlend_d" ||
        method_name == "UnivTransBlend_a") &&
       dst != nullptr && src3_1 != nullptr && src3_2 != nullptr &&
       src3_3 != nullptr &&
       IsGpuRectFastPathEnabled(method_name.c_str()) &&
       ShouldUseGpuRectFastPath(rctar, method_name.c_str(), dst, src3_1,
                                src3_2, src3_3) &&
       RectBoundsInsideTexture(textures[0].second, src3_1) &&
       RectBoundsInsideTexture(textures[1].second, src3_2) &&
       RectBoundsInsideTexture(textures[2].second, src3_3) &&
       dst->EnsureGpuHandle() && src3_1->EnsureGpuHandle() &&
       src3_2->EnsureGpuHandle() && src3_3->EnsureGpuHandle() &&
       src3_1->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
       src3_2->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
       src3_3->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
       dst->BlendGpuFrom3(
           src3_1, src3_2, src3_3, rctar, textures[0].second,
           textures[1].second, textures[2].second,
           method_name == "UnivTransBlend_d"
               ? TVP_GODOT_GPU_BLEND_UNIVERSAL_D
               : method_name == "UnivTransBlend_a"
                     ? TVP_GODOT_GPU_BLEND_UNIVERSAL_A
                     : TVP_GODOT_GPU_BLEND_UNIVERSAL,
           godot_method != nullptr ? godot_method->Phase() : 0,
           static_cast<uint32_t>(godot_method != nullptr
                                     ? godot_method->Vague()
                                     : 0))) {
        CountGpuFastPath(method_name);
        return;
    }

    if (TraceGpuFallback() && method_name == "Copy") {
        std::fprintf(stderr,
                     "godot_gpu_fallback method=Copy tex_count=%zu dst=%p src=%p "
                     "dst_handle=%llu src_handle=%llu target=(%d,%d,%d,%d) "
                     "src_rect=(%d,%d,%d,%d) large=%d enabled=%d\n",
                     textures.size(), static_cast<void *>(dst), static_cast<void *>(src),
                     static_cast<unsigned long long>(dst != nullptr ? dst->GetSdlTextureHandle() : 0),
                     static_cast<unsigned long long>(src != nullptr ? src->GetSdlTextureHandle() : 0),
                     rctar.left, rctar.top, rctar.right, rctar.bottom,
                     textures.size() == 1 ? textures[0].second.left : 0,
                     textures.size() == 1 ? textures[0].second.top : 0,
                     textures.size() == 1 ? textures[0].second.right : 0,
                     textures.size() == 1 ? textures[0].second.bottom : 0,
                     IsGpuRectLargeEnoughForMethod(rctar, method_name.c_str()) ? 1 : 0,
                     IsGpuRectFastPathEnabled("Copy") ? 1 : 0);
    }
    if (DetailedRenderStats() && method_name == "Copy") {
        if (textures.size() != 1) {
            CountCopyFallbackReason("texture_count");
        } else if (dst == nullptr) {
            CountCopyFallbackReason("dst_not_godot");
        } else if (src == nullptr) {
            CountCopyFallbackReason("src_not_godot");
        } else if (!IsGpuRectFastPathEnabled("Copy")) {
            CountCopyFallbackReason("disabled");
        } else if (!IsGpuRectLargeEnoughForMethod(rctar, method_name.c_str())) {
            CountCopyFallbackReason("small_rect");
        } else if (rctar.get_width() != textures[0].second.get_width() ||
                   rctar.get_height() != textures[0].second.get_height() ||
                   textures[0].second.get_width() <= 0 ||
                   textures[0].second.get_height() <= 0) {
            std::ostringstream reason;
            reason << "mismatch_d" << rctar.get_width() << "x" << rctar.get_height()
                   << "_s" << textures[0].second.get_width() << "x"
                   << textures[0].second.get_height()
                   << "_src" << textures[0].second.left << "," << textures[0].second.top
                   << "," << textures[0].second.right << "," << textures[0].second.bottom;
            CountCopyFallbackReason(reason.str());
        } else if (!dst->HasSdlTextureHandle()) {
            CountCopyFallbackReason("dst_no_gpu_handle");
        } else if (!src->HasSdlTextureHandle()) {
            CountCopyFallbackReason("src_no_gpu_handle");
        } else {
            CountCopyFallbackReason("bridge_copy_failed");
        }
    }

    CountMethodFallback(method);
    SoftwareDelegate()->OperateRect(delegate_method, tar, reftar, rctar, textures);
    if (dst != nullptr) {
        dst->MarkCpuDirty();
    }
}

void SDLRenderManager::OperateTriangles(iTVPRenderMethod *method, int nTriangles,
                                          iTVPTexture2D *target,
                                          iTVPTexture2D *reftar,
                                          const tTVPRect &rcclip,
                                          const tTVPPointD *pttar,
                                          const tRenderTexQuadArray &textures) {
    ++draw_count_;
    const std::string method_name =
        method != nullptr ? method->GetName() : std::string();
    auto *godot_method = dynamic_cast<SDLRenderMethod *>(method);
    auto *dst = dynamic_cast<SDLTexture2D *>(target);
    auto *src = textures.size() == 1
        ? dynamic_cast<SDLTexture2D *>(textures[0].first)
        : nullptr;
    if (method_name == "Copy") {
        if (dst != nullptr && src != nullptr &&
            IsGpuRectFastPathEnabled("Copy") &&
            IsGpuCopyTrianglesEnabled() &&
            !TrianglesNeedAlphaAreaDownsample(static_cast<uint32_t>(nTriangles),
                                              pttar, textures[0].second, src) &&
            dst->EnsureGpuHandle() && src->EnsureGpuHandle() &&
            src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
            dst->CopyTrianglesGpuFrom(src, static_cast<uint32_t>(nTriangles),
                                      rcclip, pttar, textures[0].second)) {
            CountGpuFastPath(method_name);
            return;
        }
        CountCopyFallbackReason(IsGpuCopyTrianglesEnabled()
                                    ? "triangles"
                                    : "triangles_cpu");
    }

    uint32_t blend_mode = 0;
    const char *fast_path_name = nullptr;
    if (method_name == "AlphaBlend") {
        blend_mode = TVP_GODOT_GPU_BLEND_ALPHA;
        fast_path_name = "AlphaBlend";
    } else if (method_name == "AlphaBlend_d") {
        blend_mode = TVP_GODOT_GPU_BLEND_ALPHA_D;
        fast_path_name = "AlphaBlend_d";
    } else if (method_name == "AlphaBlend_a" ||
               method_name == "PerspectiveAlphaBlend_a") {
        blend_mode = TVP_GODOT_GPU_BLEND_ALPHA_BLEND_A;
        fast_path_name = "AlphaBlend_a";
    } else if (method_name == "CopyColor") {
        blend_mode = TVP_GODOT_GPU_BLEND_COPY_COLOR;
        fast_path_name = "CopyColor";
    } else if (method_name == "ConstAlphaBlend_d") {
        blend_mode = TVP_GODOT_GPU_BLEND_CONST_ALPHA_D;
        fast_path_name = "ConstAlphaBlend_d";
    } else if (method_name == "PsScreenBlend") {
        blend_mode = TVP_GODOT_GPU_BLEND_PS_SCREEN;
        fast_path_name = "PsScreenBlend";
    } else if (method_name == "PsAddBlend") {
        blend_mode = TVP_GODOT_GPU_BLEND_PS_ADD;
        fast_path_name = "PsAddBlend";
    } else if (method_name == "PsSubBlend") {
        blend_mode = TVP_GODOT_GPU_BLEND_PS_SUBTRACT;
        fast_path_name = "PsSubBlend";
    } else if (method_name == "PsMulBlend") {
        blend_mode = TVP_GODOT_GPU_BLEND_PS_MULTIPLY;
        fast_path_name = "PsMulBlend";
    }
    if (fast_path_name != nullptr && nTriangles > 0 && nTriangles <= 64 &&
        dst != nullptr &&
        src != nullptr && IsGpuRectFastPathEnabled(fast_path_name) &&
        IsGpuBlendTrianglesEnabled() &&
        ShouldUseGpuRectFastPath(rcclip, fast_path_name, dst, src) &&
        dst->EnsureGpuHandle() && src->EnsureGpuHandle() &&
        src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) &&
        dst->BlendTrianglesGpuFrom(
            src, static_cast<uint32_t>(nTriangles), rcclip, pttar,
            textures[0].second, blend_mode,
            godot_method != nullptr ? godot_method->Opacity() : 255)) {
        CountGpuFastPath(method_name + ":Triangles");
        return;
    }
    CountMethodFallback(method);
    SoftwareDelegate()->OperateTriangles(
        godot_method != nullptr ? godot_method->Delegate() : method,
        nTriangles, target, reftar, rcclip, pttar, textures);
    if (dst != nullptr) {
        dst->MarkCpuDirty();
    }
}

void SDLRenderManager::OperatePerspective(iTVPRenderMethod *method, int nQuads,
                                            iTVPTexture2D *target,
                                            iTVPTexture2D *reftar,
                                            const tTVPRect &rcclip,
                                            const tTVPPointD *pttar,
                                            const tRenderTexQuadArray &textures) {
    ++draw_count_;
    const std::string method_name =
        method != nullptr ? method->GetName() : std::string();
    if (method_name == "Copy") {
        CountCopyFallbackReason("perspective");
    }
    CountMethodFallback(method);
    auto *godot_method = dynamic_cast<SDLRenderMethod *>(method);
    SoftwareDelegate()->OperatePerspective(
        godot_method != nullptr ? godot_method->Delegate() : method,
        nQuads, target, reftar, rcclip, pttar, textures);
    if (auto *dst = dynamic_cast<SDLTexture2D *>(target)) {
        dst->MarkCpuDirty();
    }
}

bool TVPSdlClearMotionScratchInPlace(
    iTVPBaseBitmap *bitmap, const tTVPRect &rect, uint32_t argb) {
    if(bitmap == nullptr || !bitmap->Is32BPP() || rect.is_empty()) {
        return false;
    }
    // Layer.assignImages shares the source texture. D3DEmote immediately
    // reuses its full-window work layer for the next character, so clearing a
    // still-shared texture in place also clears the character that received
    // the preceding frame. The scratch is fully repainted; detach without
    // copying instead of mutating an assignImages destination.
    bitmap->IndependNoCopy();
    auto *texture =
        dynamic_cast<SDLTexture2D *>(bitmap->GetTexture());
    if(texture == nullptr || !texture->EnsureGpuHandle()) {
        return false;
    }
    // iTVPBaseBitmap::Fill converts script-facing AARRGGBB to the backend's
    // AABBGGRR byte layout before invoking FillARGB.
    const uint32_t abgr =
        (argb & 0xff00ff00u) |
        ((argb & 0x000000ffu) << 16u) |
        ((argb & 0x00ff0000u) >> 16u);
    return texture->ClearGpu(abgr, rect);
}

bool TVPSdlCompositeAlphaUnionMask(
    iTVPBaseBitmap *dst_bitmap, iTVPBaseBitmap *src_bitmap,
    iTVPBaseBitmap *mask_scratch_bitmap,
    iTVPBaseBitmap *const *mask_bitmaps,
    const tTVPRect *mask_dst_rects,
    const tTVPRect *mask_src_rects,
    size_t mask_count,
    bool use_mask_alpha,
    int width,
    int height) {
    if(dst_bitmap == nullptr || src_bitmap == nullptr ||
       mask_scratch_bitmap == nullptr || mask_bitmaps == nullptr ||
       mask_dst_rects == nullptr || mask_src_rects == nullptr ||
       mask_count == 0 || dst_bitmap == src_bitmap ||
       dst_bitmap == mask_scratch_bitmap ||
       src_bitmap == mask_scratch_bitmap) {
        return false;
    }

    if(width <= 0) {
        width = static_cast<int>(dst_bitmap->GetWidth());
    }
    if(height <= 0) {
        height = static_cast<int>(dst_bitmap->GetHeight());
    }
    const tTVPRect full_dst(0, 0, width, height);
    const tTVPRect full_src(0, 0, width, height);
    const tTVPRect full_mask(0, 0, width, height);
    if(full_dst.is_empty() || full_src.is_empty() || full_mask.is_empty() ||
       full_dst.right > static_cast<tjs_int>(dst_bitmap->GetWidth()) ||
       full_dst.bottom > static_cast<tjs_int>(dst_bitmap->GetHeight()) ||
       full_src.right > static_cast<tjs_int>(src_bitmap->GetWidth()) ||
       full_src.bottom > static_cast<tjs_int>(src_bitmap->GetHeight()) ||
       full_mask.right > static_cast<tjs_int>(mask_scratch_bitmap->GetWidth()) ||
       full_mask.bottom > static_cast<tjs_int>(mask_scratch_bitmap->GetHeight())) {
        return false;
    }

    auto *dst = dynamic_cast<SDLTexture2D *>(
        dst_bitmap->GetTextureForRender(true, &full_dst));
    auto *src = dynamic_cast<SDLTexture2D *>(src_bitmap->GetTexture());
    auto *mask_scratch = dynamic_cast<SDLTexture2D *>(
        mask_scratch_bitmap->GetTextureForRender(true, &full_mask));
    if(dst == nullptr || src == nullptr || mask_scratch == nullptr ||
       !dst->EnsureGpuHandle() || !src->EnsureGpuHandle() ||
       !mask_scratch->EnsureGpuHandle() ||
       !src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) ||
       !dst->ClearGpu(0x00000000u, full_dst) ||
       !mask_scratch->ClearGpu(0x00000000u, full_mask)) {
        return false;
    }

    for(size_t i = 0; i < mask_count; ++i) {
        if(mask_bitmaps[i] == nullptr || mask_dst_rects[i].is_empty() ||
           mask_src_rects[i].is_empty() ||
           mask_dst_rects[i].get_width() != mask_src_rects[i].get_width() ||
           mask_dst_rects[i].get_height() != mask_src_rects[i].get_height()) {
            continue;
        }
        auto *mask = dynamic_cast<SDLTexture2D *>(
            mask_bitmaps[i]->GetTexture());
        if(mask == nullptr || !mask->EnsureGpuHandle() ||
           !mask->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) ||
           !mask_scratch->BlendGpuFrom(
               mask, mask_dst_rects[i], mask_src_rects[i],
               // AlphaBlend holds the destination alpha in KiriKiri.  The
               // union scratch starts transparent, so use the destination-
               // alpha variant to accumulate src-over alpha just like the
               // native op-5 mask builder.
               TVP_GODOT_GPU_BLEND_ALPHA_D, 255, 0)) {
            return false;
        }
    }

    const tTVPPointD points[6] = {
        {0.0, 0.0},
        {static_cast<double>(full_dst.right), 0.0},
        {0.0, static_cast<double>(full_dst.bottom)},
        {static_cast<double>(full_dst.right), 0.0},
        {0.0, static_cast<double>(full_dst.bottom)},
        {static_cast<double>(full_dst.right),
         static_cast<double>(full_dst.bottom)},
    };
    return dst->DrawMaskedTrianglesGpuFrom(
        src, mask_scratch, 2, full_dst, points, points, points, 255, 0,
        use_mask_alpha);
}

bool TVPSdlApplyAlphaUnionMask(
    iTVPBaseBitmap *dst_bitmap, iTVPBaseBitmap *mask_scratch_bitmap,
    iTVPBaseBitmap *const *mask_bitmaps,
    const tTVPRect *mask_dst_rects,
    const tTVPRect *mask_src_rects,
    size_t mask_count,
    bool threshold_mask_mode,
    int item_flags,
    int width,
    int height) {
    if(dst_bitmap == nullptr || mask_scratch_bitmap == nullptr ||
       mask_bitmaps == nullptr || mask_dst_rects == nullptr ||
       mask_src_rects == nullptr || mask_count == 0 ||
       dst_bitmap == mask_scratch_bitmap) {
        return false;
    }

    if(width <= 0) {
        width = static_cast<int>(dst_bitmap->GetWidth());
    }
    if(height <= 0) {
        height = static_cast<int>(dst_bitmap->GetHeight());
    }
    const tTVPRect full_rect(0, 0, width, height);
    if(full_rect.is_empty() ||
       full_rect.right > static_cast<tjs_int>(dst_bitmap->GetWidth()) ||
       full_rect.bottom > static_cast<tjs_int>(dst_bitmap->GetHeight()) ||
       full_rect.right >
           static_cast<tjs_int>(mask_scratch_bitmap->GetWidth()) ||
       full_rect.bottom >
           static_cast<tjs_int>(mask_scratch_bitmap->GetHeight())) {
        return false;
    }

    auto *dst = dynamic_cast<SDLTexture2D *>(
        dst_bitmap->GetTextureForRender(true, &full_rect));
    auto *mask_scratch = dynamic_cast<SDLTexture2D *>(
        mask_scratch_bitmap->GetTextureForRender(true, &full_rect));
    if(dst == nullptr || mask_scratch == nullptr ||
       !dst->EnsureGpuHandle() || !mask_scratch->EnsureGpuHandle() ||
       !dst->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) ||
       !mask_scratch->ClearGpu(0x00000000u, full_rect)) {
        return false;
    }

    for(size_t i = 0; i < mask_count; ++i) {
        if(mask_bitmaps[i] == nullptr ||
           mask_bitmaps[i] == mask_scratch_bitmap ||
           mask_dst_rects[i].is_empty() ||
           mask_src_rects[i].is_empty() ||
           mask_dst_rects[i].get_width() !=
               mask_src_rects[i].get_width() ||
           mask_dst_rects[i].get_height() !=
               mask_src_rects[i].get_height()) {
            return false;
        }
        auto *mask = dynamic_cast<SDLTexture2D *>(
            mask_bitmaps[i]->GetTexture());
        if(mask == nullptr || !mask->EnsureGpuHandle() ||
           !mask->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) ||
           !mask_scratch->BlendGpuFrom(
               mask, mask_dst_rects[i], mask_src_rects[i],
               TVP_GODOT_GPU_BLEND_ALPHA_D, 255, 0)) {
            return false;
        }
    }

    const uint32_t color =
        static_cast<uint32_t>(item_flags & 0xff) |
        (static_cast<uint32_t>(threshold_mask_mode ? 1u : 0u) << 8);
    if(!dst->BlendGpuFrom(
           mask_scratch, full_rect, full_rect,
           TVP_GODOT_GPU_BLEND_APPLY_ALPHA_MASK, 64, color)) {
        return false;
    }
    CountGpuFastPath("ApplyAlphaUnionMask");
    return true;
}

bool TVPSdlApplyAlphaMask(
    iTVPBaseBitmap *dst_bitmap, iTVPBaseBitmap *src_bitmap,
    const tTVPRect &dst_rect, const tTVPRect &src_rect,
    int threshold, bool threshold_mask_mode, int item_flags) {
    if(dst_bitmap == nullptr || src_bitmap == nullptr ||
       dst_bitmap == src_bitmap || dst_rect.is_empty() || src_rect.is_empty() ||
       dst_rect.get_width() != src_rect.get_width() ||
       dst_rect.get_height() != src_rect.get_height()) {
        return false;
    }
    auto *dst = dynamic_cast<SDLTexture2D *>(
        dst_bitmap->GetTextureForRender(true, &dst_rect));
    auto *src = dynamic_cast<SDLTexture2D *>(src_bitmap->GetTexture());
    if(dst == nullptr || src == nullptr ||
       !RectBoundsInsideTexture(dst_rect, dst) ||
       !RectBoundsInsideTexture(src_rect, src) ||
       !dst->EnsureGpuHandle() || !src->EnsureGpuHandle() ||
       !dst->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) ||
       !src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled())) {
        return false;
    }
    const uint32_t color =
        static_cast<uint32_t>(item_flags & 0xff) |
        (static_cast<uint32_t>(threshold_mask_mode ? 1u : 0u) << 8);
    if(!dst->BlendGpuFrom(
           src, dst_rect, src_rect,
           TVP_GODOT_GPU_BLEND_APPLY_ALPHA_MASK,
           std::clamp(threshold, 0, 255), color)) {
        return false;
    }
    CountGpuFastPath("ApplyAlphaMask");
    return true;
}

bool TVPSdlBlendAlphaDWithMask(
    iTVPBaseBitmap *dst_bitmap, iTVPBaseBitmap *src_bitmap,
    iTVPBaseBitmap *mask_bitmap, const tTVPRect &dst_rect,
    const tTVPRect &src_rect, const tTVPRect &mask_rect, int opacity,
    bool threshold_mask_mode) {
    if(dst_bitmap == nullptr || src_bitmap == nullptr ||
       mask_bitmap == nullptr || dst_bitmap == src_bitmap ||
       dst_bitmap == mask_bitmap || src_bitmap == mask_bitmap ||
       dst_rect.is_empty() || src_rect.is_empty() || mask_rect.is_empty() ||
       dst_rect.get_width() != src_rect.get_width() ||
       dst_rect.get_height() != src_rect.get_height() ||
       dst_rect.get_width() != mask_rect.get_width() ||
       dst_rect.get_height() != mask_rect.get_height()) {
        return false;
    }
    auto *dst = dynamic_cast<SDLTexture2D *>(
        dst_bitmap->GetTextureForRender(true, &dst_rect));
    auto *src = dynamic_cast<SDLTexture2D *>(src_bitmap->GetTexture());
    auto *mask = dynamic_cast<SDLTexture2D *>(mask_bitmap->GetTexture());
    if(dst == nullptr || src == nullptr || mask == nullptr ||
       !RectBoundsInsideTexture(dst_rect, dst) ||
       !RectBoundsInsideTexture(src_rect, src) ||
       !RectBoundsInsideTexture(mask_rect, mask) ||
       !dst->EnsureGpuHandle() || !src->EnsureGpuHandle() ||
       !mask->EnsureGpuHandle() ||
       !dst->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) ||
       !src->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled()) ||
       !mask->UploadCpuToGpu(!DeferredSdlGpuDrainEnabled())) {
        return false;
    }
    const uint32_t mode = threshold_mask_mode
        ? TVP_GODOT_GPU_BLEND_ALPHA_D_MASK_THRESHOLD
        : TVP_GODOT_GPU_BLEND_ALPHA_D_MASK_MULTIPLY;
    if(!dst->BlendGpuFrom2(
           src, mask, dst_rect, src_rect, mask_rect, mode,
           std::clamp(opacity, 0, 255), 0)) {
        return false;
    }
    CountGpuFastPath("AlphaBlend_d+Mask");
    return true;
}

std::string TVPGetSDLRenderManagerFallbackStats() {
    std::vector<std::pair<std::string, uint64_t>> entries;
    uint64_t texture_creates = 0;
    uint64_t texture_clones = 0;
    uint64_t texture_resizes = 0;
    uint64_t fallbacks = 0;
    uint64_t gpu_fastpaths = 0;
    std::vector<std::pair<std::string, uint64_t>> gpu_entries;
    std::vector<std::pair<std::string, uint64_t>> copy_fallback_entries;
    if (DetailedRenderStats()) {
        std::lock_guard<std::mutex> lock(g_method_stats_mutex);
        entries.reserve(g_method_stats.size());
        for (const auto &entry : g_method_stats) {
            entries.push_back(entry);
        }
        gpu_entries.reserve(g_gpu_method_stats.size());
        for (const auto &entry : g_gpu_method_stats) {
            gpu_entries.push_back(entry);
        }
        copy_fallback_entries.reserve(g_copy_fallback_stats.size());
        for (const auto &entry : g_copy_fallback_stats) {
            copy_fallback_entries.push_back(entry);
        }
    }
    texture_creates = g_texture_create_count.load(std::memory_order_relaxed);
    texture_clones = g_texture_clone_count.load(std::memory_order_relaxed);
    texture_resizes = g_texture_resize_count.load(std::memory_order_relaxed);
    fallbacks = g_software_fallback_count.load(std::memory_order_relaxed);
    gpu_fastpaths = g_gpu_fastpath_count.load(std::memory_order_relaxed);
    std::sort(entries.begin(), entries.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    std::sort(gpu_entries.begin(), gpu_entries.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    std::sort(copy_fallback_entries.begin(), copy_fallback_entries.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });

    std::ostringstream out;
    out << " fallback_ops=" << fallbacks
        << " gpu_ops=" << gpu_fastpaths
        << " gpu_min_area=" << GpuRectMinArea()
        << " gpu_alpha_min_area=" << GpuRectMinAreaForMethod("AlphaBlend")
        << " texture_creates=" << texture_creates
        << " texture_clones=" << texture_clones
        << " texture_resizes=" << texture_resizes
        << " hot=[";
    const size_t limit = std::min<size_t>(entries.size(), 8);
    for (size_t i = 0; i < limit; ++i) {
        if (i != 0) out << ",";
        out << entries[i].first << ":" << entries[i].second;
    }
    out << "] gpu_hot=[";
    const size_t gpu_limit = std::min<size_t>(gpu_entries.size(), 8);
    for (size_t i = 0; i < gpu_limit; ++i) {
        if (i != 0) out << ",";
        out << gpu_entries[i].first << ":" << gpu_entries[i].second;
    }
    out << "] copy_fallback=[";
    const size_t copy_limit = std::min<size_t>(copy_fallback_entries.size(), 8);
    for (size_t i = 0; i < copy_limit; ++i) {
        if (i != 0) out << ",";
        out << copy_fallback_entries[i].first << ":" << copy_fallback_entries[i].second;
    }
    out << "]";
    return out.str();
}

namespace {
iTVPRenderManager *CreateSDLRenderManager() { return new SDLRenderManager(); }

class SDLRenderManagerAutoRegister {
public:
    SDLRenderManagerAutoRegister() {
        TVPRegisterRenderManager("godot_native", CreateSDLRenderManager);
        TVPRegisterRenderManager("gpu_bridge", CreateSDLRenderManager);
        TVPRegisterRenderManager("debug_cpu", CreateSDLRenderManager);
    }
} godot_render_manager_auto_register;
} // namespace

void TVPForceRegisterSdlRenderManager() {}

void TVPSetSDLRenderManagerGpuFastPathEnabled(bool enabled) {
    g_gpu_fastpath_enabled.store(enabled, std::memory_order_relaxed);
}
