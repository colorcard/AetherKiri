// AetherKiri SDL3 host: minimal rendering/playback layer that drives the
// engine through the C ABI (bridge/engine_api). Owns the window, input
// forwarding, and the presentation path (CPU readback or zero-copy GPU
// frame via the SDL_Renderer GPU bridge).

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "engine_api.h"
#include "engine_options.h"
#include "host_input.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "ui/ui_state.h"

// UI state shared with the ui_* modules (declared at file scope so their
// extern declarations link; visible to everything below).
UiState g_ui_state;
OverlayStats g_overlay_stats;
FileDialogRequest g_file_dialog;

namespace {

constexpr uint32_t kDefaultSurfaceWidth = 1280;
constexpr uint32_t kDefaultSurfaceHeight = 720;
constexpr uint32_t kDefaultFpsLimit = 60;

struct HostState {
    engine_handle_t engine = nullptr;
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *screen = nullptr;
    SDL_Texture *pixel_buffer = nullptr;

    uint32_t surface_width = kDefaultSurfaceWidth;
    uint32_t surface_height = kDefaultSurfaceHeight;
    uint32_t window_width = kDefaultSurfaceWidth;
    uint32_t window_height = kDefaultSurfaceHeight;

    uint32_t frame_stride = 0;
    std::vector<uint8_t> frame_pixels;
    bool frame_ready = false;

    // Zero-copy GPU presentation (render_backend = gpu_bridge/sdl3_gpu):
    // the engine publishes an SDL_Texture* via the GPU bridge and the host
    // presents it directly instead of CPU readback + texture upload.
    bool use_gpu_present = false;
    uint64_t gpu_frame_texture = 0;

    uint64_t last_ticks = 0;
    double fps = 0.0;
    int frame_count = 0;
    uint64_t fps_timer_start = 0;

    bool running = true;
    bool startup_complete = false;
    bool exit_requested = false;
    std::string screenshot_path;  // filled from CLI below
    int screenshot_after_frames = 0;
    int frame_count_since_start = 0;
    // Engine termination (e.g. window close from script) stops frame
    // production without any host notification; exit after a stall.
    uint64_t last_frame_serial = 0;
    uint64_t no_frame_since_ms = 0;
    uint64_t last_log_poll_ms = 0;
    // Structured diagnostics (--diagnostics [profile]).
    bool diagnostics_enabled = false;
    uint64_t diagnostic_mask = 0;
    uint64_t last_diag_poll_ms = 0;
    uint64_t last_title_stats_ms = 0;
    char memory_stats_text[96] = "";
    // Slow-frame analysis (default on, 20 ms; 0 disables).
    uint32_t slow_frame_threshold_ms = 20;
    uint64_t last_slow_frame_ms = 0;
    uint32_t suppressed_slow_frames = 0;
    // Benchmark (--benchmark <seconds>; 0 disables).
    uint32_t benchmark_seconds = 0;
    uint64_t benchmark_start_ms = 0;
    uint64_t benchmark_frames = 0;
    std::vector<double> benchmark_tick_us;
    std::vector<double> benchmark_readback_us;
    std::vector<double> benchmark_upload_us;
    std::vector<double> benchmark_total_us;
};

void SavePpm(const std::string &path, const uint8_t *rgba,
             uint32_t width, uint32_t height, uint32_t stride) {
    FILE *file = fopen(path.c_str(), "wb");
    if(file == nullptr)
        return;
    fprintf(file, "P6\n%u %u\n255\n", width, height);
    for(uint32_t row = 0; row < height; ++row) {
        const uint8_t *src = rgba + static_cast<size_t>(row) * stride;
        for(uint32_t col = 0; col < width; ++col) {
            fputc(src[col * 4 + 0], file);
            fputc(src[col * 4 + 1], file);
            fputc(src[col * 4 + 2], file);
        }
    }
    fclose(file);
    fprintf(stderr, "screenshot saved: %s (%ux%u)\n", path.c_str(), width,
            height);
}

// Reads the published GPU frame back into a PPM. The engine does not keep a
// CPU copy of GPU-published frames, so the host locks the SDL texture
// directly (pixel memory is R,G,B,A on little-endian, same as SavePpm).
void SaveGpuFramePpm(HostState &state, const std::string &path) {
    auto *tex = reinterpret_cast<SDL_Texture *>(
        static_cast<uintptr_t>(state.gpu_frame_texture));
    if(tex == nullptr) {
        fprintf(stderr, "screenshot failed: no GPU frame available\n");
        return;
    }
    void *pixels = nullptr;
    int pitch = 0;
    if(!SDL_LockTexture(tex, nullptr, &pixels, &pitch)) {
        fprintf(stderr, "screenshot failed: SDL_LockTexture: %s\n",
                SDL_GetError());
        return;
    }
    SavePpm(path, static_cast<const uint8_t *>(pixels),
            static_cast<uint32_t>(tex->w), static_cast<uint32_t>(tex->h),
            static_cast<uint32_t>(pitch));
    SDL_UnlockTexture(tex);
}

const char *StartupStateName(uint32_t state) {
    switch(state) {
        case ENGINE_STARTUP_STATE_IDLE:
            return "idle";
        case ENGINE_STARTUP_STATE_RUNNING:
            return "running";
        case ENGINE_STARTUP_STATE_SUCCEEDED:
            return "succeeded";
        case ENGINE_STARTUP_STATE_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

std::string WritableBaseDir() {
    const char *override_dir = std::getenv("AETHERKIRI_SDL_DATA_DIR");
    if(override_dir != nullptr && *override_dir != '\0')
        return override_dir;
    const char *xdg_data = std::getenv("XDG_DATA_HOME");
    if(xdg_data != nullptr && *xdg_data != '\0')
        return std::string(xdg_data) + "/aetherkiri-sdl";
    const char *home = std::getenv("HOME");
    if(home != nullptr && *home != '\0')
        return std::string(home) + "/.local/share/aetherkiri-sdl";
    return "aetherkiri-sdl";
}

void EnsureDirectory(const std::string &path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
}

void UpdateWindowTitle(HostState &state) {
    char title[192];
    if(state.startup_complete) {
        snprintf(title, sizeof(title),
                 "AetherKiri SDL Host - %ux%u @ %.1f fps%s",
                 state.surface_width, state.surface_height, state.fps,
                 state.memory_stats_text);
    } else {
        snprintf(title, sizeof(title), "AetherKiri SDL Host - starting...");
    }
    SDL_SetWindowTitle(state.window, title);
}

bool CreatePresentation(HostState &state) {
    state.renderer =
        SDL_CreateRenderer(state.window, nullptr);
    if(state.renderer == nullptr) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    state.screen = SDL_CreateTexture(
        state.renderer, SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING, state.surface_width,
        state.surface_height);
    if(state.screen == nullptr) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(state.screen, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(state.screen, SDL_BLENDMODE_NONE);
    return true;
}

void DestroyPresentation(HostState &state) {
    if(state.screen != nullptr) {
        SDL_DestroyTexture(state.screen);
        state.screen = nullptr;
    }
    if(state.renderer != nullptr) {
        SDL_DestroyRenderer(state.renderer);
        state.renderer = nullptr;
    }
}

// Presents the latest engine frame to the window (aspect-correct, centered;
// the window keeps a fixed aspect ratio so the frame is never stretched).
// Clears the renderer and draws the game texture (ImGui is layered on top
// before the final SDL_RenderPresent in the host loop).
void PresentGameTexture(HostState &state) {
    if(state.renderer == nullptr)
        return;
    SDL_SetRenderDrawColor(state.renderer, 0, 0, 0, 255);
    SDL_RenderClear(state.renderer);
    if(!state.frame_ready)
        return;
    SDL_Texture *frame = nullptr;
    if(state.use_gpu_present) {
        // Zero-copy: the engine's GPU bridge publishes SDL_Texture* handles.
        frame = reinterpret_cast<SDL_Texture *>(
            static_cast<uintptr_t>(state.gpu_frame_texture));
    } else {
        frame = state.screen;
    }
    if(frame == nullptr)
        return;
    int frame_w = 0;
    int frame_h = 0;
    float tex_w = 0.0f;
    float tex_h = 0.0f;
    if(SDL_GetTextureSize(frame, &tex_w, &tex_h) || tex_w <= 0.0f ||
       tex_h <= 0.0f) {
        return;
    }
    frame_w = static_cast<int>(tex_w);
    frame_h = static_cast<int>(tex_h);
    const float win_w = static_cast<float>(state.window_width);
    const float win_h = static_cast<float>(state.window_height);
    const float src_aspect =
        static_cast<float>(frame_w) / static_cast<float>(frame_h);
    const float win_aspect = win_w / win_h;
    // Fit the frame inside the window preserving its aspect (letterbox);
    // a no-op when the window aspect matches the frame.
    SDL_FRect dst;
    if(src_aspect > win_aspect) {
        dst.w = win_w;
        dst.h = win_w / src_aspect;
    } else {
        dst.h = win_h;
        dst.w = win_h * src_aspect;
    }
    dst.x = (win_w - dst.w) * 0.5f;
    dst.y = (win_h - dst.h) * 0.5f;
    SDL_RenderTexture(state.renderer, frame, nullptr, &dst);
}

// Keeps the window aspect ratio in sync with the engine's surface (the
// game's logical resolution, which may differ from the default window).
// SDL_SetWindowAspectRatio also resizes the window immediately to satisfy
// the new constraint, so the frame fills the window exactly.
void MatchWindowToSurfaceAspect(HostState &state) {
    if(state.window == nullptr || state.surface_width == 0 ||
       state.surface_height == 0) {
        return;
    }
    const uint32_t flags = SDL_GetWindowFlags(state.window);
    if((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) != 0) {
        return;
    }
    const float surface_aspect =
        static_cast<float>(state.surface_width) /
        static_cast<float>(state.surface_height);
    SDL_SetWindowAspectRatio(state.window, surface_aspect, surface_aspect);
    int win_w = 0;
    int win_h = 0;
    SDL_GetWindowSize(state.window, &win_w, &win_h);
    fprintf(stderr, "[host] window aspect locked to game: %dx%d (game %ux%u)\n",
            win_w, win_h, state.surface_width, state.surface_height);
}

// Copies the engine frame into the streaming texture.
void UploadEngineFrame(HostState &state) {
    if(state.screen == nullptr)
        return;
    if(state.frame_pixels.empty())
        return;
    void *pixels = nullptr;
    int pitch = 0;
    if(SDL_LockTexture(state.screen, nullptr, &pixels, &pitch)) {
        const uint32_t copy_pitch =
            std::min<uint32_t>(static_cast<uint32_t>(pitch),
                               state.frame_stride);
        const uint8_t *src = state.frame_pixels.data();
        const uint8_t *first_px = src;
        uint8_t *dst = static_cast<uint8_t *>(pixels);
        for(uint32_t row = 0; row < state.surface_height; ++row) {
            memcpy(dst, src, copy_pitch);
            dst += pitch;
            src += state.frame_stride;
        }
        SDL_UnlockTexture(state.screen);
        state.frame_ready = true;
        static int upload_log_counter = 0;
        if((upload_log_counter++ % 300) == 0) {
            fprintf(stderr,
                    "[upload] pitch=%d stride=%u %ux%u firstpx=%02x%02x%02x%02x\n",
                    pitch, state.frame_stride, state.surface_width,
                    state.surface_height, first_px[0], first_px[1],
                    first_px[2], first_px[3]);
        }
    } else {
        fprintf(stderr, "[upload] LockTexture failed: %s\n", SDL_GetError());
    }
}
void TickEngine(HostState &state) {
    if(state.engine == nullptr)
        return;
    const uint64_t now = SDL_GetTicks();
    const uint64_t elapsed = now - state.last_ticks;
    state.last_ticks = now;
    const uint32_t delta_ms =
        elapsed > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(elapsed);

    const auto tick_start = std::chrono::steady_clock::now();
    engine_tick(state.engine, delta_ms);
    const auto tick_end = std::chrono::steady_clock::now();

    engine_frame_desc_t frame_desc;
    memset(&frame_desc, 0, sizeof(frame_desc));
    frame_desc.struct_size = sizeof(frame_desc);
    if(engine_get_frame_desc(state.engine, &frame_desc) != ENGINE_RESULT_OK)
        return;

    const size_t frame_bytes =
        static_cast<size_t>(frame_desc.stride_bytes) * frame_desc.height;
    if(frame_bytes == 0)
        return;

    // The engine renders at the game's logical resolution, which may differ
    // from the surface size requested at startup. Follow it: re-declare the
    // surface size so the engine's input coordinate space (viewport rect)
    // matches the frame we present, then rebuild the streaming texture.
    if(frame_desc.width != state.surface_width ||
       frame_desc.height != state.surface_height) {
        state.surface_width = frame_desc.width;
        state.surface_height = frame_desc.height;
        engine_set_surface_size(state.engine, frame_desc.width,
                                frame_desc.height);
        MatchWindowToSurfaceAspect(state);
        if(state.screen != nullptr) {
            SDL_DestroyTexture(state.screen);
            state.screen = nullptr;
        }
        state.screen = SDL_CreateTexture(
            state.renderer, SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_STREAMING, state.surface_width,
            state.surface_height);
        if(state.screen != nullptr) {
            SDL_SetTextureScaleMode(state.screen, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(state.screen, SDL_BLENDMODE_NONE);
        } else {
            fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
            state.exit_requested = true;
            return;
        }
    }

    if(frame_bytes != state.frame_pixels.size()) {
        state.frame_pixels.resize(frame_bytes);
        state.frame_stride = frame_desc.stride_bytes;
    }
    const auto readback_start = std::chrono::steady_clock::now();
    const bool frame_read =
        engine_read_frame_rgba(state.engine, state.frame_pixels.data(),
                               frame_bytes) == ENGINE_RESULT_OK;
    const auto readback_end = std::chrono::steady_clock::now();
    if(state.use_gpu_present) {
        // Zero-copy path: grab the engine's published GPU frame handle and
        // present it directly; no readback, no texture upload.
        uint64_t gpu_texture = 0;
        uint32_t gpu_w = 0;
        uint32_t gpu_h = 0;
        uint64_t gpu_serial = 0;
        const engine_result_t gpu_result =
            engine_get_gpu_frame_texture(state.engine, &gpu_texture, &gpu_w,
                                         &gpu_h, &gpu_serial);
        static int gpu_err_log = 0;
        if(gpu_result != ENGINE_RESULT_OK && (gpu_err_log++ % 300) == 0) {
            fprintf(stderr, "[host] gpu frame fetch failed: result=%d err='%s'\n",
                    static_cast<int>(gpu_result),
                    engine_get_last_error(state.engine));
        }
        if(gpu_result == ENGINE_RESULT_OK && gpu_texture != 0) {
            state.gpu_frame_texture = gpu_texture;
            state.frame_ready = true;
        } else {
            state.gpu_frame_texture = 0;
            state.frame_ready = false;
        }
        if(!state.screenshot_path.empty()) {
            ++state.frame_count_since_start;
            if(state.frame_count_since_start >= state.screenshot_after_frames) {
                SaveGpuFramePpm(state, state.screenshot_path);
                state.screenshot_path.clear();
                state.exit_requested = true;
            }
        }
        // Reuse the readback timing slot for the GPU frame fetch so the
        // overlay/benchmark/slow-frame machinery keeps working unchanged.
        const auto gpu_end = std::chrono::steady_clock::now();
        const auto us = [](const auto &a, const auto &b) {
            return std::chrono::duration<double, std::micro>(b - a).count();
        };
        const double tick_us = us(tick_start, tick_end);
        const double fetch_us = us(readback_start, gpu_end);
        const double total_us = us(tick_start, gpu_end);
        g_overlay_stats.tick_ms = tick_us / 1000.0;
        g_overlay_stats.readback_ms = fetch_us / 1000.0;
        g_overlay_stats.upload_ms = 0.0;
        g_overlay_stats.total_ms = total_us / 1000.0;
        g_overlay_stats.stats_valid = true;
        if(state.benchmark_seconds > 0) {
            state.benchmark_frames++;
            state.benchmark_tick_us.push_back(tick_us);
            state.benchmark_readback_us.push_back(fetch_us);
            state.benchmark_upload_us.push_back(0.0);
            state.benchmark_total_us.push_back(total_us);
            constexpr size_t kMaxBenchmarkSamples = 100000;
            if(state.benchmark_total_us.size() > kMaxBenchmarkSamples) {
                state.benchmark_tick_us.erase(
                    state.benchmark_tick_us.begin(),
                    state.benchmark_tick_us.begin() + 10000);
                state.benchmark_readback_us.erase(
                    state.benchmark_readback_us.begin(),
                    state.benchmark_readback_us.begin() + 10000);
                state.benchmark_upload_us.erase(
                    state.benchmark_upload_us.begin(),
                    state.benchmark_upload_us.begin() + 10000);
                state.benchmark_total_us.erase(
                    state.benchmark_total_us.begin(),
                    state.benchmark_total_us.begin() + 10000);
            }
        }
        if(state.slow_frame_threshold_ms > 0 &&
           total_us >= state.slow_frame_threshold_ms * 1000.0) {
            const uint64_t slow_now = SDL_GetTicks();
            if(state.last_slow_frame_ms == 0 ||
               slow_now - state.last_slow_frame_ms >= 1000) {
                fprintf(stderr,
                        "[host] slow frame total=%.1fms tick=%.1f gpu_fetch=%.1f "
                        "(suppressed=%u)\n",
                        total_us / 1000.0, tick_us / 1000.0,
                        fetch_us / 1000.0, state.suppressed_slow_frames);
                state.suppressed_slow_frames = 0;
            } else {
                state.suppressed_slow_frames++;
            }
        }

        // Frame-stop detection: same rule as the CPU path — exit when the
        // engine's published frame serial stops advancing (script
        // TVPTerminateAsync, e.g. win.close()).
        const uint64_t gpu_stall_now = SDL_GetTicks();
        if(frame_desc.frame_serial != state.last_frame_serial) {
            state.last_frame_serial = frame_desc.frame_serial;
            state.no_frame_since_ms = 0;
        } else if(state.no_frame_since_ms == 0) {
            state.no_frame_since_ms = gpu_stall_now;
        } else if(gpu_stall_now - state.no_frame_since_ms >= 3000) {
            fprintf(stderr, "[host] engine stopped producing frames; exiting\n");
            state.exit_requested = true;
        }
        return;
    }
    if(frame_read) {
        const auto upload_start = std::chrono::steady_clock::now();
        UploadEngineFrame(state);
        const auto upload_end = std::chrono::steady_clock::now();
        if(!state.screenshot_path.empty()) {
            ++state.frame_count_since_start;
            if(state.frame_count_since_start >= state.screenshot_after_frames) {
                SavePpm(state.screenshot_path, state.frame_pixels.data(),
                        frame_desc.width, frame_desc.height,
                        frame_desc.stride_bytes);
                state.screenshot_path.clear();
                state.exit_requested = true;
            }
        }

        // Frame timing: host presentation path (engine_tick internals are
        // already profiled engine-side).
        const auto frame_end = std::chrono::steady_clock::now();
        const auto us = [](const auto &a, const auto &b) {
            return std::chrono::duration<double, std::micro>(b - a).count();
        };
        const double tick_us = us(tick_start, tick_end);
        const double readback_us = us(readback_start, readback_end);
        const double upload_us = us(upload_start, upload_end);
        const double total_us = us(tick_start, frame_end);

        // Publish to the ImGui overlay.
        g_overlay_stats.tick_ms = tick_us / 1000.0;
        g_overlay_stats.readback_ms = readback_us / 1000.0;
        g_overlay_stats.upload_ms = upload_us / 1000.0;
        g_overlay_stats.total_ms = total_us / 1000.0;
        g_overlay_stats.stats_valid = true;

        if(state.benchmark_seconds > 0) {
            state.benchmark_frames++;
            state.benchmark_tick_us.push_back(tick_us);
            state.benchmark_readback_us.push_back(readback_us);
            state.benchmark_upload_us.push_back(upload_us);
            state.benchmark_total_us.push_back(total_us);
            constexpr size_t kMaxBenchmarkSamples = 100000;
            if(state.benchmark_total_us.size() > kMaxBenchmarkSamples) {
                state.benchmark_tick_us.erase(
                    state.benchmark_tick_us.begin(),
                    state.benchmark_tick_us.begin() + 10000);
                state.benchmark_readback_us.erase(
                    state.benchmark_readback_us.begin(),
                    state.benchmark_readback_us.begin() + 10000);
                state.benchmark_upload_us.erase(
                    state.benchmark_upload_us.begin(),
                    state.benchmark_upload_us.begin() + 10000);
                state.benchmark_total_us.erase(
                    state.benchmark_total_us.begin(),
                    state.benchmark_total_us.begin() + 10000);
            }
        }

        if(state.slow_frame_threshold_ms > 0 &&
           total_us >= state.slow_frame_threshold_ms * 1000.0) {
            const uint64_t slow_now = SDL_GetTicks();
            if(state.last_slow_frame_ms == 0 ||
               slow_now - state.last_slow_frame_ms >= 1000) {
                fprintf(stderr,
                        "[host] slow frame total=%.1fms tick=%.1f "
                        "readback=%.1f upload=%.1f (suppressed=%u)\n",
                        total_us / 1000.0, tick_us / 1000.0,
                        readback_us / 1000.0, upload_us / 1000.0,
                        static_cast<unsigned>(state.suppressed_slow_frames));
                state.last_slow_frame_ms = slow_now;
                state.suppressed_slow_frames = 0;
            } else {
                state.suppressed_slow_frames++;
            }
        }
    }

    // The engine stops producing frames once its application terminates
    // (TVPTerminateAsync, e.g. script win.close()); there is no host
    // notification, so exit when the frame serial stops advancing.
    const uint64_t stall_now = SDL_GetTicks();
    if(frame_desc.frame_serial != state.last_frame_serial) {
        state.last_frame_serial = frame_desc.frame_serial;
        state.no_frame_since_ms = 0;
    } else if(state.no_frame_since_ms == 0) {
        state.no_frame_since_ms = stall_now;
    } else if(stall_now - state.no_frame_since_ms >= 3000) {
        fprintf(stderr, "[host] engine stopped producing frames; exiting\n");
        state.exit_requested = true;
    }
}

// Sorted percentile helper over a copy of the sample buffer.
double Percentile(std::vector<double> samples, double p) {
    if(samples.empty())
        return 0.0;
    std::sort(samples.begin(), samples.end());
    const size_t idx = std::min<size_t>(
        samples.size() - 1, static_cast<size_t>(p * samples.size()));
    return samples[idx];
}

void PrintBenchmarkSummary(HostState &state) {
    const uint64_t now = SDL_GetTicks();
    const double duration_s =
        static_cast<double>(now - state.benchmark_start_ms) / 1000.0;
    if(duration_s <= 0.0 || state.benchmark_total_us.empty()) {
        fprintf(stderr, "benchmark: no frames collected\n");
        return;
    }
    const double avg_fps =
        static_cast<double>(state.benchmark_frames) / duration_s;
    fprintf(stderr,
            "benchmark: duration=%.1fs frames=%llu avg_fps=%.1f\n",
            duration_s,
            static_cast<unsigned long long>(state.benchmark_frames),
            avg_fps);
    const auto print_series = [](const char *name,
                                 const std::vector<double> &samples) {
        double sum = 0.0;
        for(double v : samples)
            sum += v;
        fprintf(stderr,
                "  %s_ms avg=%.2f p50=%.2f p95=%.2f p99=%.2f max=%.2f\n",
                name, sum / samples.size() / 1000.0,
                Percentile(samples, 0.50) / 1000.0,
                Percentile(samples, 0.95) / 1000.0,
                Percentile(samples, 0.99) / 1000.0,
                *std::max_element(samples.begin(), samples.end()) / 1000.0);
    };
    print_series("frame_total", state.benchmark_total_us);
    print_series("tick", state.benchmark_tick_us);
    print_series("readback", state.benchmark_readback_us);
    print_series("upload", state.benchmark_upload_us);
}

void HandleWindowEvent(HostState &state, const SDL_Event *event) {
    if(event->type == SDL_EVENT_WINDOW_RESIZED) {
        state.window_width = event->window.data1;
        state.window_height = event->window.data2;
    }
}

void PollInput(HostState &state) {
    SDL_Event event;
    while(SDL_PollEvent(&event)) {
        switch(event.type) {
            case SDL_EVENT_QUIT:
                state.exit_requested = true;
                return;
            case SDL_EVENT_WINDOW_RESIZED:
                HandleWindowEvent(state, &event);
                break;
            case SDL_EVENT_KEY_DOWN: {
                if(event.key.key == SDLK_F12) {
                    g_ui_state.show_overlay = !g_ui_state.show_overlay;
                }
                if(event.key.key == SDLK_ESCAPE && !state.startup_complete) {
                    state.exit_requested = true;
                    return;
                }
                if(event.key.key == SDLK_Q &&
                   (event.key.mod & SDL_KMOD_CTRL) != 0) {
                    state.exit_requested = true;
                    return;
                }
                break;
            }
            default:
                break;
        }

        // ImGui consumes events over its panels; only forward the rest.
        ImGui_ImplSDL3_ProcessEvent(&event);
        ImGuiIO &io = ImGui::GetIO();
        const bool capture_mouse =
            (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
             event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
             event.type == SDL_EVENT_MOUSE_MOTION ||
             event.type == SDL_EVENT_MOUSE_WHEEL) &&
            io.WantCaptureMouse;
        const bool capture_key =
            (event.type == SDL_EVENT_KEY_DOWN ||
             event.type == SDL_EVENT_KEY_UP ||
             event.type == SDL_EVENT_TEXT_INPUT) &&
            io.WantCaptureKeyboard;
        if(state.engine != nullptr && state.startup_complete &&
           !capture_mouse && !capture_key) {
            const float scale_x =
                static_cast<float>(state.surface_width) /
                static_cast<float>(state.window_width);
            const float scale_y =
                static_cast<float>(state.surface_height) /
                static_cast<float>(state.window_height);
            ForwardSdlEventToEngine(state.engine, &event, scale_x, scale_y);
        }
    }
}

void DrainStartupLogs(HostState &state) {
    if(state.engine == nullptr)
        return;
    char buffer[4096];
    uint32_t written = 0;
    while(engine_drain_startup_logs(state.engine, buffer, sizeof(buffer),
                                    &written) == ENGINE_RESULT_OK &&
          written > 0) {
        fwrite(buffer, 1, written, stderr);
    }
}

void DrainRuntimeLogs(HostState &state) {
    if(state.engine == nullptr)
        return;
    // Poll the engine's runtime log queue at a low cadence; forward new
    // lines to stderr and into the overlay log buffer. The queue cursor
    // advances inside the engine, so lines stay deduplicated across calls.
    const uint64_t now = SDL_GetTicks();
    if(state.last_log_poll_ms != 0 && now - state.last_log_poll_ms < 100)
        return;
    state.last_log_poll_ms = now;

    char buffer[8192];
    uint32_t written = 0;
    while(engine_drain_runtime_logs(state.engine, buffer, sizeof(buffer),
                                    &written) == ENGINE_RESULT_OK &&
          written > 0) {
        fwrite(buffer, 1, written, stderr);
        g_overlay_stats.log_buffer.append(buffer, written);
        constexpr size_t kMaxOverlayLogs = 1 << 20;  // 1 MiB ring
        if(g_overlay_stats.log_buffer.size() > kMaxOverlayLogs) {
            g_overlay_stats.log_buffer.erase(0, kMaxOverlayLogs / 2);
        }
        written = 0;
    }
}

// Diagnostic profile -> category mask, mirroring the Godot host's
// PROFILE_MASK table (category bits: lifecycle=1 input=2 render=4
// storage=8 script=16 audio=32 video=64 plugin=128 memory=256 system=512).
uint64_t DiagnosticMaskForProfile(const std::string &profile) {
    if(profile == "input")
        return (1ull << 0) | (1ull << 1) | (1ull << 2);
    if(profile == "render")
        return (1ull << 0) | (1ull << 2) | (1ull << 8);
    if(profile == "storage")
        return (1ull << 0) | (1ull << 2) | (1ull << 3) | (1ull << 8);
    if(profile == "script")
        return (1ull << 0) | (1ull << 2) | (1ull << 4);
    if(profile == "audio")
        return (1ull << 0) | (1ull << 2) | (1ull << 5);
    if(profile == "video")
        return (1ull << 0) | (1ull << 6) | (1ull << 2);
    if(profile == "plugin")
        return (1ull << 0) | (1ull << 2) | (1ull << 7);
    if(profile == "system")
        return (1ull << 0) | (1ull << 2) | (1ull << 9);
    if(profile == "full")
        return (1ull << 10) - 1;
    return (1ull << 0) | (1ull << 2);  // baseline
}

void EnableDiagnostics(HostState &state, const std::string &profile) {
    if(state.engine == nullptr)
        return;
    state.diagnostics_enabled = true;
    state.diagnostic_mask = DiagnosticMaskForProfile(profile);

    engine_diagnostic_config_t config;
    memset(&config, 0, sizeof(config));
    config.struct_size = sizeof(config);
    config.enabled = 1;
    config.category_mask = state.diagnostic_mask;
    config.slow_frame_threshold_us = 20000;
    config.max_events = 2000;
    // Called before SDL_Init, so SDL_GetTicks is not usable yet.
    static std::string session_id =
        "sdl-host-" + std::to_string(std::chrono::steady_clock::now()
                                         .time_since_epoch()
                                         .count());
    config.session_id_utf8 = session_id.c_str();
    if(engine_set_diagnostic_config(state.engine, &config) !=
       ENGINE_RESULT_OK) {
        fprintf(stderr, "[host] diagnostic config failed: %s\n",
                engine_get_last_error(state.engine));
    }
}

void DrainDiagnosticEvents(HostState &state) {
    if(!state.diagnostics_enabled || state.engine == nullptr)
        return;
    const uint64_t now = SDL_GetTicks();
    if(state.last_diag_poll_ms != 0 && now - state.last_diag_poll_ms < 1000)
        return;
    state.last_diag_poll_ms = now;

    char buffer[8192];
    uint32_t written = 0;
    while(engine_drain_diagnostic_events(state.engine, buffer, sizeof(buffer),
                                         &written) == ENGINE_RESULT_OK &&
          written > 0) {
        fwrite(buffer, 1, written, stderr);
        g_overlay_stats.diag_buffer.append(buffer, written);
        constexpr size_t kMaxOverlayDiag = 512 * 1024;
        if(g_overlay_stats.diag_buffer.size() > kMaxOverlayDiag) {
            g_overlay_stats.diag_buffer.erase(0, kMaxOverlayDiag / 2);
        }
        written = 0;
    }
}

// Refreshes the memory-stats suffix shown in the window title once per
// second (best-effort; silently ignored when the call fails).
void RefreshMemoryStatsText(HostState &state) {
    const uint64_t now = SDL_GetTicks();
    if(state.last_title_stats_ms != 0 && now - state.last_title_stats_ms < 1000)
        return;
    state.last_title_stats_ms = now;

    engine_memory_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    if(engine_get_memory_stats(state.engine, &stats) != ENGINE_RESULT_OK)
        return;
    snprintf(state.memory_stats_text, sizeof(state.memory_stats_text),
             " | rss %lluM gfx %lluM xp3 %lluM",
             static_cast<unsigned long long>(stats.process_resident_bytes /
                                             1024 / 1024),
             static_cast<unsigned long long>(stats.graphic_cache_bytes / 1024 /
                                            1024),
             static_cast<unsigned long long>(stats.xp3_segment_cache_bytes /
                                             1024 / 1024));
}

void RunStartup(HostState &state) {
    uint32_t startup_state = ENGINE_STARTUP_STATE_IDLE;
    if(engine_get_startup_state(state.engine, &startup_state) !=
       ENGINE_RESULT_OK) {
        return;
    }
    DrainStartupLogs(state);
    if(startup_state == ENGINE_STARTUP_STATE_SUCCEEDED) {
        state.startup_complete = true;
        if(state.benchmark_seconds > 0 && state.benchmark_start_ms == 0)
            state.benchmark_start_ms = SDL_GetTicks();
        UpdateWindowTitle(state);
        return;
    }
    if(startup_state == ENGINE_STARTUP_STATE_FAILED) {
        fprintf(stderr, "engine startup failed\n");
        DrainStartupLogs(state);
        g_ui_state.game_state = GameState::Error;
        g_ui_state.status_text = u8"游戏启动失败（详见日志）";
        state.startup_complete = false;
    }
}

void UpdateFps(HostState &state) {
    ++state.frame_count;
    const uint64_t now = SDL_GetTicks();
    if(state.fps_timer_start == 0)
        state.fps_timer_start = now;
    const uint64_t window_ms = now - state.fps_timer_start;
    if(window_ms >= 1000) {
        state.fps = static_cast<double>(state.frame_count) * 1000.0 /
                    static_cast<double>(window_ms);
        state.frame_count = 0;
        state.fps_timer_start = now;
        UpdateWindowTitle(state);
    }
}

// Forward declarations for ImGui integration and engine-lifecycle helpers
// (defined later in this anonymous namespace).
bool InitImGui(SDL_Window *window, SDL_Renderer *renderer,
               const std::string &font_path);
void ShutdownImGui();
void BeginImGuiFrame();
void EndImGuiFrame(SDL_Renderer *renderer);
void PaintUi(UiState &state, bool game_running);
void PollFileDialog(SDL_Window *window);
void StopEngine(HostState &state);
bool StartEngine(HostState &state, const std::string &base_dir,
                 const std::vector<std::pair<std::string, std::string>>
                     &extra_options);
void LaunchGame(HostState &state);

int RunHost(const std::string &game_path, uint32_t fps_limit,
            const std::string &screenshot_path, int screenshot_after_frames,
            const std::string &diagnostics_profile,
            uint32_t slow_frame_threshold_ms, uint32_t benchmark_seconds,
            const std::vector<std::pair<std::string, std::string>>
                &extra_options) {
    HostState state;
    state.screenshot_path = screenshot_path;
    state.screenshot_after_frames = screenshot_after_frames;
    state.slow_frame_threshold_ms = slow_frame_threshold_ms;
    state.benchmark_seconds = benchmark_seconds;

    const std::string base_dir = WritableBaseDir();
    EnsureDirectory(base_dir);

    // Crash artifacts (stack/log/frame) land under the writable data dir so
    // they survive terminal loss and are easy to find.
    const std::string crash_dir = base_dir + "/crashes";
    EnsureDirectory(crash_dir);
    setenv("AETHERKIRI_CRASH_DIR", crash_dir.c_str(), 1);

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    state.window = SDL_CreateWindow(
        "AetherKiri SDL Host", state.window_width, state.window_height,
        SDL_WINDOW_RESIZABLE);
    if(state.window == nullptr) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    // Lock the window to the default surface aspect so user resizing can
    // never distort the picture (SDL3 re-constrains on live resize).
    const float default_aspect =
        static_cast<float>(kDefaultSurfaceWidth) /
        static_cast<float>(kDefaultSurfaceHeight);
    SDL_SetWindowAspectRatio(state.window, default_aspect, default_aspect);
    if(!CreatePresentation(state)) {
        SDL_DestroyWindow(state.window);
        SDL_Quit();
        return 1;
    }

    if(!InitImGui(state.window, state.renderer,
                  g_ui_state.settings.font_path)) {
        DestroyPresentation(state);
        SDL_DestroyWindow(state.window);
        SDL_Quit();
        return 1;
    }

    // Seed UI settings from CLI arguments (still honored for scripting).
    if(!game_path.empty()) {
        g_ui_state.settings.game_path = game_path;
    }
    if(fps_limit != 0)
        g_ui_state.settings.fps_limit = fps_limit;
    if(!diagnostics_profile.empty())
        g_ui_state.settings.diagnostics_profile = diagnostics_profile;
    state.slow_frame_threshold_ms = slow_frame_threshold_ms;

    // Without --game the host opens in launcher mode (NoGame).
    const bool launch_immediately = !game_path.empty();
    if(launch_immediately) {
        if(!StartEngine(state, base_dir, extra_options)) {
            g_ui_state.game_state = GameState::Error;
        } else {
            LaunchGame(state);
        }
    } else {
        g_ui_state.status_text = u8"请选择游戏后启动";
    }

    SDL_StartTextInput(state.window);

    state.last_ticks = SDL_GetTicks();
    uint32_t frame_ms =
        g_ui_state.settings.fps_limit > 0
            ? (1000u / g_ui_state.settings.fps_limit)
            : 0;

    while(state.running && !state.exit_requested) {
        PollInput(state);
        if(state.exit_requested)
            break;
        PollFileDialog(state.window);

        // UI-driven engine actions.
        if(g_ui_state.want_start) {
            g_ui_state.want_start = false;
            if(StartEngine(state, base_dir, extra_options))
                LaunchGame(state);
        }
        if(g_ui_state.want_restart) {
            g_ui_state.want_restart = false;
            if(StartEngine(state, base_dir, extra_options))
                LaunchGame(state);
        }
        if(g_ui_state.want_quit) {
            g_ui_state.want_quit = false;
            StopEngine(state);
            state.screenshot_path.clear();
            continue;
        }
        if(state.engine != nullptr && g_ui_state.want_pause) {
            g_ui_state.want_pause = false;
            engine_pause(state.engine);
        }
        if(state.engine != nullptr && g_ui_state.want_resume) {
            g_ui_state.want_resume = false;
            engine_resume(state.engine);
        }
        if(state.engine != nullptr && g_ui_state.want_screenshot) {
            g_ui_state.want_screenshot = false;
            if(!state.frame_pixels.empty()) {
                static int shot_seq = 0;
                const std::string path =
                    base_dir + "/shot-" + std::to_string(++shot_seq) + ".ppm";
                SavePpm(path, state.frame_pixels.data(),
                        state.surface_width, state.surface_height,
                        state.frame_stride);
                g_ui_state.status_text = u8"截图已保存: " + path;
            }
        }

        const bool game_running =
            state.engine != nullptr &&
            (g_ui_state.game_state == GameState::Launching ||
             g_ui_state.game_state == GameState::Running);

        if(game_running) {
            RunStartup(state);

            if(state.startup_complete) {
                g_ui_state.game_state = GameState::Running;
                TickEngine(state);
            }
            DrainRuntimeLogs(state);
            DrainDiagnosticEvents(state);
            RefreshMemoryStatsText(state);
        }

        // Fill overlay stats from the last frame's timing.
        if(state.startup_complete && state.engine != nullptr) {
            g_overlay_stats.stats_valid = true;
            g_overlay_stats.surface_w = state.surface_width;
            g_overlay_stats.surface_h = state.surface_height;
            engine_memory_stats_t mem;
            memset(&mem, 0, sizeof(mem));
            mem.struct_size = sizeof(mem);
            if(engine_get_memory_stats(state.engine, &mem) ==
               ENGINE_RESULT_OK) {
                g_overlay_stats.mem_valid = true;
                g_overlay_stats.rss_bytes = mem.process_resident_bytes;
                g_overlay_stats.gfx_cache_bytes = mem.graphic_cache_bytes;
                g_overlay_stats.xp3_cache_bytes =
                    mem.xp3_segment_cache_bytes;
                g_overlay_stats.psb_cache_bytes = mem.psb_cache_bytes;
            }
        } else {
            g_overlay_stats.stats_valid = false;
        }

        // Draw the scene (game texture or launcher background), then layer
        // ImGui on top, then present once.
        if(game_running) {
            PresentGameTexture(state);
            UpdateFps(state);
        } else {
            SDL_SetRenderDrawColor(state.renderer, 24, 24, 32, 255);
            SDL_RenderClear(state.renderer);
        }

        BeginImGuiFrame();
        PaintUi(g_ui_state, game_running);
        if(game_running && state.startup_complete) {
            g_overlay_stats.fps = state.fps;
        }
        EndImGuiFrame(state.renderer);

        SDL_RenderPresent(state.renderer);

        // Destroy textures the engine released this frame only after the
        // present above no longer references them.
        if(state.engine != nullptr) {
            engine_flush_released_textures(state.engine);
        }

        if(state.benchmark_seconds > 0 && state.benchmark_start_ms != 0 &&
           SDL_GetTicks() - state.benchmark_start_ms >=
               static_cast<uint64_t>(state.benchmark_seconds) * 1000) {
            PrintBenchmarkSummary(state);
            state.exit_requested = true;
        }

        if(frame_ms > 0) {
            const uint64_t frame_start = state.last_ticks;
            const uint64_t elapsed = SDL_GetTicks() - frame_start;
            if(elapsed < frame_ms)
                SDL_Delay(static_cast<uint32_t>(frame_ms - elapsed));
        }
    }

    if(state.benchmark_seconds > 0 && state.benchmark_start_ms != 0 &&
       !state.exit_requested) {
        PrintBenchmarkSummary(state);
    }

    StopEngine(state);
    ShutdownImGui();
    DestroyPresentation(state);
    SDL_DestroyWindow(state.window);
    SDL_Quit();
    return 0;
}

}  // namespace
namespace {

// ---------------------------------------------------------------------------
// ImGui integration
// ---------------------------------------------------------------------------
bool InitImGui(SDL_Window *window, SDL_Renderer *renderer,
               const std::string &font_path) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // no persistence for now
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Chinese UI: load the engine's CJK font when available; fall back to
    // the built-in ASCII font otherwise.
    ImFont *font = nullptr;
    if(!font_path.empty()) {
        ImFontConfig cfg;
        cfg.MergeMode = false;
        cfg.GlyphRanges = nullptr;
        static const ImWchar cjk_ranges[] = {
            0x0020, 0x00FF,  // basic ASCII + Latin-1
            0x3000, 0x30FF,  // CJK punctuation, kana
            0x4E00, 0x9FFF,  // CJK unified ideographs
            0xFF00, 0xFFEF,  // fullwidth forms
            0,
        };
        cfg.GlyphRanges = cjk_ranges;
        font = ImGui::GetIO().Fonts->AddFontFromFileTTF(
            font_path.c_str(), 19.0f, &cfg);
    }
    if(font == nullptr) {
        ImGui::GetIO().Fonts->AddFontDefault();
        g_ui_state.status_text =
            u8"中文界面字体缺失（NotoSansCJK-Regular.ttc），已回退英文界面";
    }

    if(!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
        fprintf(stderr, "ImGui_ImplSDL3_InitForSDLRenderer failed\n");
        ImGui::DestroyContext();
        return false;
    }
    if(!ImGui_ImplSDLRenderer3_Init(renderer)) {
        fprintf(stderr, "ImGui_ImplSDLRenderer3_Init failed\n");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    return true;
}

void ShutdownImGui() {
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void BeginImGuiFrame() {
    ImGui_ImplSDL3_NewFrame();
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui::NewFrame();
}

void EndImGuiFrame(SDL_Renderer *renderer) {
    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}

// Paints whichever panel the state machine needs (launcher vs overlay).
void PaintUi(UiState &state, bool game_running) {
    if(!game_running) {
        UiLauncherPaint(state);
    } else if(state.show_overlay) {
        UiOverlayPaint(state);
    }
    if(state.show_demo_window) {
        ImGui::ShowDemoWindow(&state.show_demo_window);
    }
}

// Handles the native file/folder dialogs (SDL3 async API). Polled each frame.
void PollFileDialog(SDL_Window *window) {
    static bool dialog_open = false;
    if(g_file_dialog.active && !dialog_open) {
        dialog_open = true;
        auto callback = [](void *userdata, const char *const *files, int) {
            auto *state = static_cast<UiState *>(userdata);
            if(files != nullptr && files[0] != nullptr) {
                state->settings.game_path = files[0];
            }
            g_file_dialog.active = false;
        };
        if(g_file_dialog.folder) {
            SDL_ShowOpenFolderDialog(callback, &g_ui_state, window, nullptr,
                                     false);
        } else {
            SDL_ShowOpenFileDialog(callback, &g_ui_state, window, nullptr, 0,
                                   nullptr, false);
        }
    }
    if(!g_file_dialog.active && dialog_open)
        dialog_open = false;
}

// ---------------------------------------------------------------------------
// Engine lifecycle helpers (UI-driven start/stop/restart)
// ---------------------------------------------------------------------------
void StopEngine(HostState &state) {
    if(state.engine != nullptr) {
        engine_destroy(state.engine);
        state.engine = nullptr;
    }
    state.startup_complete = false;
    state.frame_ready = false;
    state.frame_pixels.clear();
    state.gpu_frame_texture = 0;
    state.surface_width = kDefaultSurfaceWidth;
    state.surface_height = kDefaultSurfaceHeight;
    state.last_frame_serial = 0;
    state.no_frame_since_ms = 0;
    state.screenshot_path.clear();
    g_ui_state.game_state = GameState::NoGame;
}

bool StartEngine(HostState &state, const std::string &base_dir,
                 const std::vector<std::pair<std::string, std::string>>
                     &extra_options) {
    if(state.engine != nullptr)
        StopEngine(state);

    engine_create_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.api_version = ENGINE_API_VERSION;
    desc.writable_path_utf8 = base_dir.c_str();
    desc.cache_path_utf8 = base_dir.c_str();
    if(engine_create(&desc, &state.engine) != ENGINE_RESULT_OK) {
        fprintf(stderr, "engine_create failed\n");
        g_ui_state.status_text = u8"引擎创建失败";
        return false;
    }

    engine_option_t option;
    memset(&option, 0, sizeof(option));
    option.key_utf8 = ENGINE_OPTION_RENDERER;
    const bool gpu_backend =
        g_ui_state.settings.render_backend == "gpu_bridge" ||
        g_ui_state.settings.render_backend == "sdl3_gpu";
    option.value_utf8 =
        gpu_backend ? ENGINE_RENDERER_GPU_BRIDGE : ENGINE_RENDERER_SOFTWARE;
    engine_set_option(state.engine, &option);
    state.use_gpu_present = gpu_backend;
    if(gpu_backend) {
        // The engine's built-in SDL3 render backend creates its own
        // textures on the injected renderer; the host keeps presentation.
        if(engine_set_sdl_renderer(state.engine, state.renderer) !=
           ENGINE_RESULT_OK) {
            fprintf(stderr, "[host] engine_set_sdl_renderer failed; "
                            "falling back to software path\n");
            state.use_gpu_present = false;
        } else {
            fprintf(stderr, "[host] render backend: gpu_bridge (in-engine "
                            "SDL textures, zero-copy present)\n");
        }
    }

    for(const auto &kv : extra_options) {
        engine_option_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.key_utf8 = kv.first.c_str();
        opt.value_utf8 = kv.second.c_str();
        engine_set_option(state.engine, &opt);
    }
    if(const char *trace = std::getenv("AETHERKIRI_INPUT_TRACE");
       trace != nullptr && strcmp(trace, "1") == 0) {
        engine_option_t input_trace;
        memset(&input_trace, 0, sizeof(input_trace));
        input_trace.key_utf8 = "input_trace";
        input_trace.value_utf8 = "1";
        engine_set_option(state.engine, &input_trace);
    }
    if(!g_ui_state.settings.diagnostics_profile.empty()) {
        EnableDiagnostics(state, g_ui_state.settings.diagnostics_profile);
    }
    engine_set_surface_size(state.engine, state.surface_width,
                            state.surface_height);
    return true;
}

void LaunchGame(HostState &state) {
    if(state.engine == nullptr ||
       g_ui_state.settings.game_path.empty())
        return;
    // The engine resolves game paths against its own project root; UI paths
    // may be relative (typed or picked), so absolutize before opening.
    std::string path = g_ui_state.settings.game_path;
    {
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(path, ec);
        if(!ec)
            path = absolute.lexically_normal().string();
    }
    const engine_result_t open_result = engine_open_game_async(
        state.engine, path.c_str(), nullptr);
    if(open_result != ENGINE_RESULT_OK) {
        fprintf(stderr, "engine_open_game_async failed: %s\n",
                engine_get_last_error(state.engine));
        g_ui_state.status_text = u8"游戏启动失败: " +
                                 std::string(engine_get_last_error(state.engine));
        g_ui_state.game_state = GameState::Error;
        return;
    }
    g_ui_state.game_state = GameState::Launching;
    g_ui_state.status_text = u8"游戏启动中...";
}


}  // namespace (ui helpers)
int main(int argc, char *argv[]) {
    std::string game_path;
    if(const char *env_path = std::getenv("AETHERKIRI_GAME_PATH");
       env_path != nullptr && *env_path != '\0') {
        game_path = env_path;
    }

    uint32_t fps_limit = kDefaultFpsLimit;
    std::string screenshot_path;
    int screenshot_after_frames = 0;
    std::string diagnostics_profile;
    uint32_t slow_frame_threshold_ms = 20;
    uint32_t benchmark_seconds = 0;
    std::vector<std::pair<std::string, std::string>> extra_options;
    for(int i = 1; i < argc; ++i) {
        if((strcmp(argv[i], "--game") == 0 || strcmp(argv[i], "-g") == 0) &&
           i + 1 < argc) {
            game_path = argv[++i];
        } else if(strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            fps_limit = static_cast<uint32_t>(
                std::max(0, atoi(argv[++i])));
        } else if(strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
            screenshot_after_frames = 180;
        } else if(strcmp(argv[i], "--screenshot-frames") == 0 &&
                  i + 1 < argc) {
            screenshot_after_frames = std::max(0, atoi(argv[++i]));
        } else if(strcmp(argv[i], "--diagnostics") == 0) {
            // Optional profile argument; default to baseline.
            if(i + 1 < argc && argv[i + 1][0] != '-')
                diagnostics_profile = argv[++i];
            else
                diagnostics_profile = "baseline";
        } else if(strcmp(argv[i], "--slow-frame-threshold-ms") == 0 &&
                  i + 1 < argc) {
            slow_frame_threshold_ms = static_cast<uint32_t>(
                std::max(0, atoi(argv[++i])));
        } else if(strcmp(argv[i], "--benchmark") == 0 && i + 1 < argc) {
            benchmark_seconds = static_cast<uint32_t>(
                std::max(0, atoi(argv[++i])));
        } else if(strcmp(argv[i], "--option") == 0 && i + 1 < argc) {
            const std::string kv = argv[++i];
            const size_t eq = kv.find('=');
            if(eq != std::string::npos && eq > 0) {
                extra_options.emplace_back(kv.substr(0, eq),
                                          kv.substr(eq + 1));
            } else {
                fprintf(stderr, "ignoring malformed --option '%s' "
                                "(expected key=value)\n",
                        kv.c_str());
            }
        } else if(strcmp(argv[i], "--render-backend") == 0 && i + 1 < argc) {
            const std::string backend = argv[++i];
            if(backend == "software" || backend == "gpu_bridge" ||
               backend == "sdl3_gpu") {
                g_ui_state.settings.render_backend = backend;
            } else {
                fprintf(stderr,
                        "ignoring unknown --render-backend '%s' "
                        "(expected software|gpu_bridge|sdl3_gpu)\n",
                        backend.c_str());
            }
        } else if(strcmp(argv[i], "--trace") == 0) {
            extra_options.emplace_back("trace_log", "1");
        } else if(strcmp(argv[i], "--plugin-trace") == 0) {
            extra_options.emplace_back("plugin_trace", "1");
        } else if(strcmp(argv[i], "--export-scripts") == 0) {
            extra_options.emplace_back("export_scripts", "1");
        } else if(strcmp(argv[i], "--no-mock") == 0) {
            extra_options.emplace_back("mock_enabled", "0");
        } else if(strcmp(argv[i], "--console-log-file") == 0) {
            extra_options.emplace_back("console_log_file", "1");
        } else if(strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                    "Usage: aetherkiri_ui [--game <path>] [--fps <n>]\n"
                    "  [--screenshot <path>] [--screenshot-frames <n>]\n"
                    "  [--diagnostics [profile]]  structured event stream\n"
                    "  [--slow-frame-threshold-ms <n>]  default 20 (0=off)\n"
                    "  [--benchmark <seconds>]  timing summary then exit\n"
                    "  [--option key=value]       any engine option\n"
                    "  [--trace] [--plugin-trace] [--export-scripts]\n"
                    "  [--no-mock] [--console-log-file]\n"
                    "  AETHERKIRI_GAME_PATH env var is honored too.\n");
            return 0;
        }
    }

    if(game_path.empty()) {
        // No game path: open in launcher mode (game selected via ImGui).
        fprintf(stderr, "[host] no game path; starting in launcher mode\n");
    }

    // The engine resolves game paths against its own project root; pass an
    // absolute path so relative arguments survive that normalization.
    if(!game_path.empty()) {
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(game_path, ec);
        if(!ec)
            game_path = absolute.lexically_normal().string();
    }

    return RunHost(game_path, fps_limit, screenshot_path,
                   screenshot_after_frames, diagnostics_profile,
                   slow_frame_threshold_ms, benchmark_seconds, extra_options);
}
