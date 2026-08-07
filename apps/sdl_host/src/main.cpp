// AetherKiri SDL3 host: minimal rendering/playback layer that drives the
// engine through the C ABI (bridge/engine_api). Owns the window, input
// forwarding, and the CPU-readback presentation path.

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

// Presents the latest engine frame to the window (stretched).
void PresentFrame(HostState &state) {
    if(state.renderer == nullptr)
        return;
    SDL_SetRenderDrawColor(state.renderer, 0, 0, 0, 255);
    SDL_RenderClear(state.renderer);
    if(state.screen == nullptr || !state.frame_ready)
        return;
    SDL_FRect dst{0.0f, 0.0f, static_cast<float>(state.window_width),
                  static_cast<float>(state.window_height)};
    SDL_RenderTexture(state.renderer, state.screen, nullptr, &dst);
    SDL_RenderPresent(state.renderer);
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

    engine_tick(state.engine, delta_ms);

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
    if(engine_read_frame_rgba(state.engine, state.frame_pixels.data(),
                              frame_bytes) == ENGINE_RESULT_OK) {
        UploadEngineFrame(state);
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
        if(state.engine != nullptr && state.startup_complete) {
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
    // Poll the engine's runtime log queue at a low cadence and forward new
    // lines to stderr. The queue cursor advances inside the engine, so
    // lines stay deduplicated across calls.
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
        UpdateWindowTitle(state);
        return;
    }
    if(startup_state == ENGINE_STARTUP_STATE_FAILED) {
        fprintf(stderr, "engine startup failed\n");
        DrainStartupLogs(state);
        state.exit_requested = true;
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

int RunHost(const std::string &game_path, uint32_t fps_limit,
            const std::string &screenshot_path, int screenshot_after_frames,
            const std::string &diagnostics_profile,
            const std::vector<std::pair<std::string, std::string>>
                &extra_options) {
    HostState state;
    state.screenshot_path = screenshot_path;
    state.screenshot_after_frames = screenshot_after_frames;

    const std::string base_dir = WritableBaseDir();
    EnsureDirectory(base_dir);

    engine_create_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.api_version = ENGINE_API_VERSION;
    desc.writable_path_utf8 = base_dir.c_str();
    desc.cache_path_utf8 = base_dir.c_str();

    if(engine_create(&desc, &state.engine) != ENGINE_RESULT_OK) {
        fprintf(stderr, "engine_create failed\n");
        return 1;
    }

    engine_option_t option;
    memset(&option, 0, sizeof(option));
    option.key_utf8 = ENGINE_OPTION_RENDERER;
    option.value_utf8 = ENGINE_RENDERER_SOFTWARE;
    engine_set_option(state.engine, &option);

    // Extra engine options from the command line (--option key=value and the
    // convenience switches). Applied before game startup so they are visible
    // to the whole session.
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

    if(!diagnostics_profile.empty()) {
        EnableDiagnostics(state, diagnostics_profile);
    }

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        engine_destroy(state.engine);
        return 1;
    }

    state.window = SDL_CreateWindow(
        "AetherKiri SDL Host", state.window_width, state.window_height,
        SDL_WINDOW_RESIZABLE);
    if(state.window == nullptr) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        engine_destroy(state.engine);
        return 1;
    }
    if(!CreatePresentation(state)) {
        SDL_DestroyWindow(state.window);
        SDL_Quit();
        engine_destroy(state.engine);
        return 1;
    }

    // The engine surface stays at the logical game resolution; the window
    // stretches it on display.
    engine_set_surface_size(state.engine, state.surface_width,
                            state.surface_height);

    SDL_StartTextInput(state.window);

    state.last_ticks = SDL_GetTicks();
    const uint32_t frame_ms = fps_limit > 0 ? (1000u / fps_limit) : 0;

    const engine_result_t open_result =
        engine_open_game_async(state.engine, game_path.c_str(), nullptr);
    if(open_result != ENGINE_RESULT_OK) {
        fprintf(stderr, "engine_open_game_async failed: %s\n",
                engine_get_last_error(state.engine));
        state.exit_requested = true;
    }

    while(state.running && !state.exit_requested) {
        PollInput(state);
        if(state.exit_requested)
            break;

        RunStartup(state);

        if(state.startup_complete)
            TickEngine(state);
        DrainRuntimeLogs(state);
        DrainDiagnosticEvents(state);
        RefreshMemoryStatsText(state);

        PresentFrame(state);
        UpdateFps(state);

        if(frame_ms > 0) {
            const uint64_t frame_start = state.last_ticks;
            const uint64_t elapsed = SDL_GetTicks() - frame_start;
            if(elapsed < frame_ms)
                SDL_Delay(static_cast<uint32_t>(frame_ms - elapsed));
        }
    }

    DestroyPresentation(state);
    SDL_DestroyWindow(state.window);
    SDL_Quit();
    engine_destroy(state.engine);
    return 0;
}

}  // namespace

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
                    "Usage: aetherkiri_sdl [--game <path>] [--fps <n>]\n"
                    "  [--screenshot <path>] [--screenshot-frames <n>]\n"
                    "  [--diagnostics [profile]]  structured event stream\n"
                    "  [--option key=value]       any engine option\n"
                    "  [--trace] [--plugin-trace] [--export-scripts]\n"
                    "  [--no-mock] [--console-log-file]\n"
                    "  AETHERKIRI_GAME_PATH env var is honored too.\n");
            return 0;
        }
    }

    if(game_path.empty()) {
        fprintf(stderr,
                "No game path given. Pass --game <path> or set "
                "AETHERKIRI_GAME_PATH.\n");
        return 1;
    }

    // The engine resolves game paths against its own project root; pass an
    // absolute path so relative arguments survive that normalization.
    {
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(game_path, ec);
        if(!ec)
            game_path = absolute.lexically_normal().string();
    }

    return RunHost(game_path, fps_limit, screenshot_path,
                   screenshot_after_frames, diagnostics_profile,
                   extra_options);
}
