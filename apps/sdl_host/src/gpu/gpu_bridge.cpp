#include "gpu_bridge.h"

#include "engine_api.h"
#include "ComplexRect.h"
#include "GodotGpuBridge.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace aetherkiri {
namespace {

SDL_Renderer *g_renderer = nullptr;

// Textures released by the engine are deferred until after the host's next
// present (see GpuBridgeFlushReleasedTextures) so a frame still being
// presented is never destroyed mid-frame.
std::mutex g_released_mutex;
std::vector<SDL_Texture *> g_released_textures;

// ---------------------------------------------------------------------------
// Texture management
// ---------------------------------------------------------------------------

// Engine CPU pixels are R,G,B,A in memory (little-endian), which matches
// SDL_PIXELFORMAT_ABGR8888 (see AGENTS.md: uploading RGBA bytes requires the
// ABGR8888 format on little-endian hosts). All textures are created with that
// format so the streaming upload path is a plain byte copy.
constexpr SDL_PixelFormat kBridgePixelFormat = SDL_PIXELFORMAT_ABGR8888;

SDL_Texture *TextureFromHandle(uint64_t texture) {
    return reinterpret_cast<SDL_Texture *>(static_cast<uintptr_t>(texture));
}

uint64_t HandleFromTexture(SDL_Texture *tex) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(tex));
}

uint32_t RectWidth(const tTVPRect *rc) {
    return rc == nullptr ? 0u
                         : static_cast<uint32_t>(std::max(0, rc->right - rc->left));
}

uint32_t RectHeight(const tTVPRect *rc) {
    return rc == nullptr ? 0u
                         : static_cast<uint32_t>(std::max(0, rc->bottom - rc->top));
}

uint64_t BridgeCreateRgba(uint32_t width, uint32_t height,
                          const void *pixels, uint32_t stride_bytes) {
    if (g_renderer == nullptr || width == 0 || height == 0) {
        return 0;
    }
    SDL_Texture *tex = SDL_CreateTexture(g_renderer, kBridgePixelFormat,
                                         SDL_TEXTUREACCESS_STREAMING, width,
                                         height);
    if (tex == nullptr) {
        fprintf(stderr, "[gpu-bridge] CreateTexture %ux%u failed: %s\n",
                width, height, SDL_GetError());
        return 0;
    }
    // SDL3 defaults to BLEND blend mode: alpha=0 regions of the composited
    // frame would show the RenderClear black through. The engine frame is
    // fully composited, so present it with NONE (same as the software path).
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    // Zero-fill the whole texture first so rows past a padded pitch are
    // transparent black rather than undefined garbage.
    void *dst = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(tex, nullptr, &dst, &pitch)) {
        std::memset(dst, 0, static_cast<size_t>(pitch) * height);
        if (pixels != nullptr && stride_bytes != 0) {
            const auto *src = static_cast<const uint8_t *>(pixels);
            auto *out = static_cast<uint8_t *>(dst);
            const uint32_t copy_bytes =
                std::min<uint32_t>(static_cast<uint32_t>(pitch), stride_bytes);
            for (uint32_t y = 0; y < height; ++y) {
                std::memcpy(out + static_cast<size_t>(y) * pitch,
                            src + static_cast<size_t>(y) * stride_bytes,
                            copy_bytes);
            }
        }
        SDL_UnlockTexture(tex);
    } else {
        fprintf(stderr, "[gpu-bridge] initial LockTexture failed: %s\n",
                SDL_GetError());
    }
    return HandleFromTexture(tex);
}

void BridgeReleaseTexture(uint64_t texture) {
    if (texture == 0) {
        return;
    }
    SDL_Texture *tex = TextureFromHandle(texture);
    std::lock_guard<std::mutex> lock(g_released_mutex);
    g_released_textures.push_back(tex);
}

bool BridgeUpdateRgba(uint64_t texture, const void *pixels,
                      uint32_t stride_bytes, const tTVPRect *rect) {
    SDL_Texture *tex = TextureFromHandle(texture);
    if (tex == nullptr || pixels == nullptr || stride_bytes == 0) {
        return false;
    }
    SDL_Rect sdl_rect{0, 0, 0, 0};
    SDL_Rect *lock_rect = nullptr;
    if (rect != nullptr) {
        sdl_rect.x = rect->left;
        sdl_rect.y = rect->top;
        sdl_rect.w = static_cast<int>(RectWidth(rect));
        sdl_rect.h = static_cast<int>(RectHeight(rect));
        if (sdl_rect.w <= 0 || sdl_rect.h <= 0) {
            return false;
        }
        lock_rect = &sdl_rect;
    }
    void *dst = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(tex, lock_rect, &dst, &pitch)) {
        fprintf(stderr, "[gpu-bridge] update_rgba LockTexture(%d,%d %dx%d) "
                        "failed: %s\n",
                lock_rect == nullptr ? 0 : lock_rect->x,
                lock_rect == nullptr ? 0 : lock_rect->y,
                lock_rect == nullptr ? 0 : lock_rect->w,
                lock_rect == nullptr ? 0 : lock_rect->h, SDL_GetError());
        return false;
    }
    const uint32_t width = static_cast<uint32_t>(
        lock_rect == nullptr ? tex->w : lock_rect->w);
    const uint32_t height = static_cast<uint32_t>(
        lock_rect == nullptr ? tex->h : lock_rect->h);
    const uint32_t copy_bytes = std::min<uint32_t>(
        static_cast<uint32_t>(pitch), stride_bytes);
    const auto *src = static_cast<const uint8_t *>(pixels);
    auto *out = static_cast<uint8_t *>(dst);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(out + static_cast<size_t>(y) * pitch,
                    src + static_cast<size_t>(y) * stride_bytes,
                    copy_bytes);
    }
    SDL_UnlockTexture(tex);
    return true;
}

bool BridgeClearRgba(uint64_t texture, uint32_t rgba, const tTVPRect *rect) {
    SDL_Texture *tex = TextureFromHandle(texture);
    if (tex == nullptr) {
        return false;
    }
    const uint32_t width =
        rect == nullptr ? static_cast<uint32_t>(tex->w) : RectWidth(rect);
    const uint32_t height =
        rect == nullptr ? static_cast<uint32_t>(tex->h) : RectHeight(rect);
    if (width == 0 || height == 0) {
        return false;
    }
    SDL_Rect sdl_rect{0, 0, static_cast<int>(width), static_cast<int>(height)};
    if (rect != nullptr) {
        sdl_rect.x = rect->left;
        sdl_rect.y = rect->top;
    }
    void *dst = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(tex, &sdl_rect, &dst, &pitch)) {
        return false;
    }
    const uint8_t rgba_bytes[4] = {
        static_cast<uint8_t>(rgba >> 24u), static_cast<uint8_t>(rgba >> 16u),
        static_cast<uint8_t>(rgba >> 8u), static_cast<uint8_t>(rgba)};
    auto *out = static_cast<uint8_t *>(dst);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = out + static_cast<size_t>(y) * pitch;
        for (uint32_t x = 0; x < width; ++x) {
            std::memcpy(row + static_cast<size_t>(x) * 4u, rgba_bytes, 4u);
        }
    }
    SDL_UnlockTexture(tex);
    return true;
}

// ---------------------------------------------------------------------------
// Read-back (synchronous; used by screenshots/diagnostics)
// ---------------------------------------------------------------------------

bool ReadTextureRgba(SDL_Texture *tex, void *out_pixels,
                     size_t out_pixels_size, uint32_t stride_bytes) {
    if (tex == nullptr || out_pixels == nullptr) {
        return false;
    }
    const uint32_t width = static_cast<uint32_t>(tex->w);
    const uint32_t height = static_cast<uint32_t>(tex->h);
    const uint32_t tight_stride = width * 4u;
    const uint32_t dst_stride = stride_bytes != 0 ? stride_bytes : tight_stride;
    if (out_pixels_size < static_cast<size_t>(dst_stride) * height) {
        return false;
    }
    void *src = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(tex, nullptr, &src, &pitch)) {
        return false;
    }
    const auto *rows = static_cast<const uint8_t *>(src);
    auto *dst = static_cast<uint8_t *>(out_pixels);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * dst_stride,
                    rows + static_cast<size_t>(y) * pitch, tight_stride);
    }
    SDL_UnlockTexture(tex);
    return true;
}

bool BridgeReadRgba(uint64_t texture, void *out_pixels,
                    size_t out_pixels_size, uint32_t stride_bytes) {
    return ReadTextureRgba(TextureFromHandle(texture), out_pixels,
                           out_pixels_size, stride_bytes);
}

struct ReadbackRequest {
    SDL_Texture *texture;
    std::vector<uint8_t> pixels;
};

std::mutex g_readbacks_mutex;
uint64_t g_next_readback_id = 1;
std::vector<std::pair<uint64_t, ReadbackRequest>> g_readbacks;

uint64_t BridgeBeginReadRgba(uint64_t texture) {
    SDL_Texture *tex = TextureFromHandle(texture);
    if (tex == nullptr) {
        return 0;
    }
    ReadbackRequest req;
    req.texture = tex;
    req.pixels.resize(static_cast<size_t>(tex->w) * tex->h * 4u);
    if (!ReadTextureRgba(tex, req.pixels.data(), req.pixels.size(),
                         static_cast<uint32_t>(tex->w) * 4u)) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_readbacks_mutex);
    const uint64_t id = g_next_readback_id++;
    if (id == 0) {
        g_next_readback_id = 1;
    }
    g_readbacks.emplace_back(id, std::move(req));
    return id;
}

bool BridgePollReadRgba(uint64_t request, void *out_pixels,
                        size_t out_pixels_size, uint32_t stride_bytes,
                        bool *ready) {
    if (ready != nullptr) {
        *ready = false;
    }
    if (request == 0 || out_pixels == nullptr) {
        return false;
    }
    ReadbackRequest req;
    {
        std::lock_guard<std::mutex> lock(g_readbacks_mutex);
        for (auto it = g_readbacks.begin(); it != g_readbacks.end(); ++it) {
            if (it->first == request) {
                req = std::move(it->second);
                g_readbacks.erase(it);
                break;
            }
        }
    }
    if (req.texture == nullptr) {
        return false;
    }
    if (ready != nullptr) {
        *ready = true;
    }
    const uint32_t width = static_cast<uint32_t>(req.texture->w);
    const uint32_t height = static_cast<uint32_t>(req.texture->h);
    const uint32_t tight_stride = width * 4u;
    const uint32_t dst_stride = stride_bytes != 0 ? stride_bytes : tight_stride;
    if (out_pixels_size < static_cast<size_t>(dst_stride) * height) {
        return false;
    }
    auto *dst = static_cast<uint8_t *>(out_pixels);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * dst_stride,
                    req.pixels.data() + static_cast<size_t>(y) * tight_stride,
                    tight_stride);
    }
    return true;
}

void BridgeDiscardReadRgba(uint64_t request) {
    std::lock_guard<std::mutex> lock(g_readbacks_mutex);
    for (auto it = g_readbacks.begin(); it != g_readbacks.end(); ++it) {
        if (it->first == request) {
            g_readbacks.erase(it);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// GPU compositing ops — not used by the current engine render path, so they
// report failure and the engine falls back to its software behavior.
// ---------------------------------------------------------------------------

bool BridgeCopyRect(uint64_t, uint64_t, const tTVPRect *, const tTVPRect *) {
    return false;
}

bool BridgeCopyTriangles(uint64_t, uint64_t, uint32_t, const tTVPRect *,
                         const tTVPPointD *, const tTVPPointD *) {
    return false;
}

bool BridgeDrawTriangles(uint64_t, uint64_t, uint32_t, const tTVPRect *,
                         const tTVPPointD *, const tTVPPointD *, float,
                         uint32_t) {
    return false;
}

bool BridgeDrawMaskedTriangles(uint64_t, uint64_t, uint64_t, uint32_t,
                               const tTVPRect *, const tTVPPointD *,
                               const tTVPPointD *, const tTVPPointD *, float,
                               uint32_t, bool) {
    return false;
}

bool BridgeMosaicRects(uint64_t, const tTVPRect *, uint32_t, uint32_t,
                       uint32_t) {
    return false;
}

bool BridgeBlendRect(uint64_t, uint64_t, const tTVPRect *, const tTVPRect *,
                     uint32_t, int, uint32_t) {
    return false;
}

bool BridgeBlendRect2(uint64_t, uint64_t, uint64_t, const tTVPRect *,
                      const tTVPRect *, const tTVPRect *, uint32_t, int,
                      uint32_t) {
    return false;
}

bool BridgeBlendRect3(uint64_t, uint64_t, uint64_t, uint64_t,
                      const tTVPRect *, const tTVPRect *, const tTVPRect *,
                      const tTVPRect *, uint32_t, int, uint32_t) {
    return false;
}

bool BridgeFlush() { return true; }

uint64_t BridgeBeginBatch() { return 1; }

bool BridgeEndBatch(uint64_t) { return true; }

}  // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void GpuBridgeSetRenderer(SDL_Renderer *renderer) { g_renderer = renderer; }

void GpuBridgeFlushReleasedTextures() {
    std::vector<SDL_Texture *> pending;
    {
        std::lock_guard<std::mutex> lock(g_released_mutex);
        pending.swap(g_released_textures);
    }
    for (SDL_Texture *tex : pending) {
        SDL_DestroyTexture(tex);
    }
}

const TVPGodotGpuBridgeCallbacks *GpuBridgeCallbacks() {
    static const TVPGodotGpuBridgeCallbacks callbacks = {
        .create_rgba = BridgeCreateRgba,
        .release_texture = BridgeReleaseTexture,
        .update_rgba = BridgeUpdateRgba,
        .clear_rgba = BridgeClearRgba,
        .copy_rect = BridgeCopyRect,
        .copy_triangles = BridgeCopyTriangles,
        .draw_triangles = BridgeDrawTriangles,
        .draw_masked_triangles = BridgeDrawMaskedTriangles,
        .mosaic_rects = BridgeMosaicRects,
        .blend_rect = BridgeBlendRect,
        .blend_rect2 = BridgeBlendRect2,
        .blend_rect3 = BridgeBlendRect3,
        .read_rgba = BridgeReadRgba,
        .begin_read_rgba = BridgeBeginReadRgba,
        .poll_read_rgba = BridgePollReadRgba,
        .discard_read_rgba = BridgeDiscardReadRgba,
        .flush = BridgeFlush,
    };
    return &callbacks;
}

const TVPGodotGpuBatchCallbacks *GpuBridgeBatchCallbacks() {
    static const TVPGodotGpuBatchCallbacks callbacks = {
        .struct_size = sizeof(callbacks),
        .abi_version = TVP_GODOT_GPU_BATCH_CALLBACKS_ABI_VERSION,
        .begin_batch = BridgeBeginBatch,
        .end_batch = BridgeEndBatch,
    };
    return &callbacks;
}

}  // namespace aetherkiri
