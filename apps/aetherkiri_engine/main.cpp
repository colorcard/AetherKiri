// AetherKiri engine standalone shell (krkrz-style SDL entrypoint).
//
// Runs a game directly with no UI: owns the window, input forwarding, the
// present loop, and drives the engine through the C ABI. Rendering happens
// in-engine: with --render-backend gpu_bridge the engine creates its own
// SDL textures on the injected renderer and the shell presents the published
// frame handle zero-copy; with software it reads frames back and uploads a
// streaming texture.
#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#if defined(__linux__)
#include <unistd.h>
#endif

#include "engine_api.h"
#include "engine_options.h"
#include "host_input.h"
#include "scenario.h"

namespace {

constexpr uint32_t kDefaultSurfaceWidth = 640;
constexpr uint32_t kDefaultSurfaceHeight = 480;

struct ShellState {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_GPUDevice *gpu_device = nullptr;
    engine_handle_t engine = nullptr;

    std::string game_path;
    bool use_gpu = false;
    bool use_sdl3_gpu = false;
    uint32_t fps_limit = 0;

    std::string screenshot_path;
    int screenshot_after_frames = 0;
    int frame_count_since_start = 0;

    std::string scenario_path;
    std::string scenario_output = "out/compat/scenario";
    std::unique_ptr<ScenarioRunner> scenario;
    const engine_visual_diagnostics_v1_t *visual_diagnostics = nullptr;
    const engine_visual_checkpoint_v1_t *visual_checkpoint = nullptr;
    uint64_t checkpoint_token = 0;
    std::string checkpoint_name;
    std::string runtime_logs;
    std::string last_visual_snapshot;
    bool scenario_failure_saved = false;
    bool scenario_failed = false;
    bool scenario_fast_exit = false;
    uint64_t scenario_started_ms = 0;
    uint64_t scenario_start_rss_bytes = 0;
    std::vector<uint32_t> scenario_frame_times_ms;

    bool startup_complete = false;
    bool exit_requested = false;

    uint32_t surface_width = kDefaultSurfaceWidth;
    uint32_t surface_height = kDefaultSurfaceHeight;
    uint32_t window_width = kDefaultSurfaceWidth;
    uint32_t window_height = kDefaultSurfaceHeight;
    bool window_auto_sized = false;

    uint64_t last_ticks = 0;
    uint64_t last_frame_serial = 0;
    uint64_t no_frame_since_ms = 0;

    uint64_t gpu_frame_texture = 0;
    std::vector<uint8_t> frame_pixels;
    uint32_t frame_stride = 0;
    SDL_Texture *screen = nullptr;  // software path
};

ShellState g;

void PrintUsage() {
    fprintf(stderr,
            "usage: aetherkiri_engine --game <path> [options]\n"
            "  --game <path>              game directory (absolute path)\n"
            "  --fps <n>                  frame rate limit (0 = unlimited)\n"
            "  --render-backend <name>    software | gpu_bridge | sdl3_gpu (default software)\n"
            "  --screenshot <path>        save a PPM after N frames\n"
            "  --screenshot-frames <n>    frames before screenshot (default 180)\n"
            "  --scenario <path>          run a version 1 scenario JSON\n"
            "  --scenario-output <path>   artifact directory (default out/compat/scenario)\n");
}

void ParseArgs(int argc, char *argv[]) {
    if (const char *env_path = std::getenv("AETHERKIRI_GAME_PATH");
        env_path != nullptr && *env_path != '\0') {
        g.game_path = env_path;
    }
    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "--game") == 0 || strcmp(argv[i], "-g") == 0) &&
            i + 1 < argc) {
            g.game_path = argv[++i];
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            g.fps_limit =
                static_cast<uint32_t>(std::max(0, atoi(argv[++i])));
        } else if (strcmp(argv[i], "--render-backend") == 0 && i + 1 < argc) {
            const std::string backend = argv[++i];
            if (backend == "software") {
                g.use_gpu = false;
            } else if (backend == "gpu_bridge") {
                g.use_gpu = true;
            } else if (backend == "sdl3_gpu") {
                g.use_gpu = true;
                g.use_sdl3_gpu = true;
            } else {
                fprintf(stderr, "unknown --render-backend '%s'\n",
                        backend.c_str());
            }
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            g.screenshot_path = argv[++i];
            g.screenshot_after_frames = 180;
        } else if (strcmp(argv[i], "--screenshot-frames") == 0 &&
                   i + 1 < argc) {
            g.screenshot_after_frames = std::max(0, atoi(argv[++i]));
        } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            g.scenario_path = argv[++i];
        } else if (strcmp(argv[i], "--scenario-output") == 0 && i + 1 < argc) {
            g.scenario_output = argv[++i];
        } else if (strcmp(argv[i], "--scenario-fast-exit") == 0) {
            g.scenario_fast_exit = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            PrintUsage();
            std::exit(0);
        }
    }
}

bool SavePpm(const std::string &path, const uint8_t *rgba, uint32_t width,
             uint32_t height, uint32_t stride) {
    FILE *file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        fprintf(stderr, "screenshot: cannot open %s\n", path.c_str());
        return false;
    }
    fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t *src = rgba + static_cast<size_t>(row) * stride;
        for (uint32_t col = 0; col < width; ++col) {
            fputc(src[col * 4 + 0], file);
            fputc(src[col * 4 + 1], file);
            fputc(src[col * 4 + 2], file);
        }
    }
    fclose(file);
    fprintf(stderr, "screenshot saved: %s (%ux%u)\n", path.c_str(), width,
            height);
    return true;
}

bool SaveText(const std::filesystem::path &path, const std::string &value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) return false;
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return output.good();
}

uint64_t ReadRssBytes() {
#if defined(__linux__)
    std::ifstream input("/proc/self/statm");
    uint64_t total_pages = 0;
    uint64_t resident_pages = 0;
    if(input >> total_pages >> resident_pages)
        return resident_pages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#endif
    return 0;
}

void SaveScenarioReport(const char *status) {
    if(!g.scenario || g.scenario_started_ms == 0) return;
    std::error_code ec;
    std::filesystem::create_directories(g.scenario_output, ec);
    std::vector<uint32_t> sorted = g.scenario_frame_times_ms;
    std::sort(sorted.begin(), sorted.end());
    const uint32_t p99 = sorted.empty() ? 0 :
        sorted[std::min(sorted.size() - 1,
                        static_cast<size_t>(sorted.size() * .99))];
    const uint32_t maximum = sorted.empty() ? 0 : sorted.back();
    const uint64_t elapsed_ms = SDL_GetTicks() - g.scenario_started_ms;
    const double fps = elapsed_ms == 0 ? 0.0 :
        g.scenario_frame_times_ms.size() * 1000.0 / elapsed_ms;
    const uint64_t end_rss = ReadRssBytes();
    std::ostringstream report;
    report << "{\"schema\":\"aetherkiri.scenario-report.v1\""
           << ",\"status\":\"" << status << "\""
           << ",\"frames\":" << g.scenario_frame_times_ms.size()
           << ",\"elapsed_ms\":" << elapsed_ms
           << ",\"average_fps\":" << fps
           << ",\"p99_frame_time_ms\":" << p99
           << ",\"max_frame_time_ms\":" << maximum
           << ",\"rss_start_bytes\":" << g.scenario_start_rss_bytes
           << ",\"rss_end_bytes\":" << end_rss
           << ",\"rss_growth_bytes\":"
           << (end_rss > g.scenario_start_rss_bytes
                   ? end_rss - g.scenario_start_rss_bytes : 0) << "}\n";
    SaveText(std::filesystem::path(g.scenario_output) / "scenario-report.json",
             report.str());
}

std::string SafeArtifactName(std::string name) {
    for(char &ch : name) {
        const bool safe = (ch >= 'a' && ch <= 'z') ||
                          (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if(!safe) ch = '_';
    }
    return name.empty() ? "checkpoint" : name;
}

ScenarioRunner::CheckpointStatus CaptureCheckpoint(
    const std::string &requested_name, std::string &error) {
    if(g.visual_checkpoint == nullptr) {
        error = "visual checkpoint interface is unavailable";
        return ScenarioRunner::CheckpointStatus::Failed;
    }
    if(g.checkpoint_token == 0) {
        if(g.visual_checkpoint->request_capture(
               g.engine, &g.checkpoint_token) != ENGINE_RESULT_OK) {
            error = std::string("checkpoint request failed: ") +
                    engine_get_last_error(g.engine);
            return ScenarioRunner::CheckpointStatus::Failed;
        }
        g.checkpoint_name = requested_name;
        return ScenarioRunner::CheckpointStatus::Pending;
    }
    if(g.checkpoint_name != requested_name) {
        error = "another checkpoint request is pending";
        return ScenarioRunner::CheckpointStatus::Failed;
    }
    engine_visual_checkpoint_info_v1_t info{};
    info.struct_size = sizeof(info);
    if(g.visual_checkpoint->get_capture(
           g.engine, g.checkpoint_token, &info, nullptr, 0, nullptr, 0) !=
       ENGINE_RESULT_OK) {
        error = std::string("checkpoint query failed: ") +
                engine_get_last_error(g.engine);
        return ScenarioRunner::CheckpointStatus::Failed;
    }
    if(info.status == ENGINE_VISUAL_CHECKPOINT_PENDING)
        return ScenarioRunner::CheckpointStatus::Pending;
    if(info.status != ENGINE_VISUAL_CHECKPOINT_READY || info.rgba_bytes == 0 ||
       info.snapshot_json_bytes == 0) {
        error = "engine failed to capture the requested visual frame";
        return ScenarioRunner::CheckpointStatus::Failed;
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(info.rgba_bytes));
    std::vector<char> snapshot(info.snapshot_json_bytes);
    info.struct_size = sizeof(info);
    if(g.visual_checkpoint->get_capture(
           g.engine, g.checkpoint_token, &info, rgba.data(), rgba.size(),
           snapshot.data(), static_cast<uint32_t>(snapshot.size())) !=
       ENGINE_RESULT_OK) {
        error = std::string("checkpoint read failed: ") +
                engine_get_last_error(g.engine);
        return ScenarioRunner::CheckpointStatus::Failed;
    }
    std::error_code ec;
    std::filesystem::create_directories(g.scenario_output, ec);
    if(ec) {
        error = "cannot create scenario output: " + ec.message();
        return ScenarioRunner::CheckpointStatus::Failed;
    }
    const auto base = std::filesystem::path(g.scenario_output) /
                      SafeArtifactName(requested_name);
    if(!SavePpm(base.string() + ".ppm", rgba.data(), info.width, info.height,
                info.stride_bytes) ||
       !SaveText(base.string() + ".layers.json", snapshot.data()) ||
       !SaveText(base.string() + ".logs.txt", g.runtime_logs)) {
        error = "cannot write checkpoint artifacts";
        return ScenarioRunner::CheckpointStatus::Failed;
    }
    std::ostringstream metadata;
    metadata << "{\"schema\":\"aetherkiri.checkpoint.v1\",\"name\":\""
             << SafeArtifactName(requested_name) << "\",\"frame_serial\":"
             << info.frame_serial << ",\"width\":" << info.width
             << ",\"height\":" << info.height << "}\n";
    if(!SaveText(base.string() + ".json", metadata.str())) {
        error = "cannot write checkpoint metadata";
        return ScenarioRunner::CheckpointStatus::Failed;
    }
    g.checkpoint_token = 0;
    g.checkpoint_name.clear();
    return ScenarioRunner::CheckpointStatus::Completed;
}

void DrainRuntimeLogs() {
    char buffer[8192];
    for(;;) {
        uint32_t written = 0;
        if(engine_drain_runtime_logs(g.engine, buffer, sizeof(buffer), &written) !=
               ENGINE_RESULT_OK || written == 0) break;
        g.runtime_logs.append(buffer, written);
    }
}

bool ReadVisualSnapshot(std::string &snapshot) {
    if(g.visual_diagnostics == nullptr) return false;
    uint32_t required = 0;
    if(g.visual_diagnostics->get_snapshot_json(g.engine, nullptr, 0, &required) !=
           ENGINE_RESULT_OK || required == 0) return false;
    std::vector<char> buffer(required);
    if(g.visual_diagnostics->get_snapshot_json(
           g.engine, buffer.data(), static_cast<uint32_t>(buffer.size()),
           &required) != ENGINE_RESULT_OK) return false;
    snapshot.assign(buffer.data());
    return true;
}

void SaveGpuFramePpm() {
    auto *tex = reinterpret_cast<SDL_Texture *>(
        static_cast<uintptr_t>(g.gpu_frame_texture));
    if (tex == nullptr) {
        fprintf(stderr, "screenshot failed: no GPU frame available\n");
        return;
    }
    void *pixels = nullptr;
    int pitch = 0;
    if (!SDL_LockTexture(tex, nullptr, &pixels, &pitch)) {
        fprintf(stderr, "screenshot failed: SDL_LockTexture: %s\n",
                SDL_GetError());
        return;
    }
    SavePpm(g.screenshot_path, static_cast<const uint8_t *>(pixels),
            static_cast<uint32_t>(tex->w), static_cast<uint32_t>(tex->h),
            static_cast<uint32_t>(pitch));
    SDL_UnlockTexture(tex);
}

bool CreatePresentation() {
    g.window = SDL_CreateWindow("AetherKiri", static_cast<int>(g.window_width),
                                static_cast<int>(g.window_height),
                                SDL_WINDOW_RESIZABLE);
    if (g.window == nullptr) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    if (g.use_sdl3_gpu) {
        g.gpu_device = SDL_CreateGPUDevice(
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, true,
            nullptr);
        if (g.gpu_device == nullptr) {
            fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
            return false;
        }
        if (!SDL_ClaimWindowForGPUDevice(g.gpu_device, g.window)) {
            fprintf(stderr, "SDL_ClaimWindowForGPUDevice failed: %s\n",
                    SDL_GetError());
            SDL_DestroyGPUDevice(g.gpu_device);
            g.gpu_device = nullptr;
            return false;
        }
        fprintf(stderr, "sdl3_gpu: SDL_GPUDevice created\n");
        // Swapchain present: no SDL_Renderer needed. g.renderer stays null;
        // readback fallback (screenshots) uses the CPU frame path.
    } else {
        g.renderer = SDL_CreateRenderer(g.window, nullptr);
        if (g.renderer == nullptr) {
            fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
            return false;
        }
    }
    if (g.use_sdl3_gpu) {
        // Readback fallback only: no SDL streaming screen texture needed.
    } else if (!g.use_gpu) {
        g.screen = SDL_CreateTexture(
            g.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(g.surface_width),
            static_cast<int>(g.surface_height));
        if (g.screen == nullptr) {
            fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetTextureScaleMode(g.screen, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(g.screen, SDL_BLENDMODE_NONE);
    }
    return true;
}

void AutoSizeWindowForSurface(uint32_t width, uint32_t height) {
    if(g.window_auto_sized || g.window == nullptr || width == 0 || height == 0)
        return;
    SDL_DisplayID display = SDL_GetDisplayForWindow(g.window);
    SDL_Rect usable{};
    if(display == 0 || !SDL_GetDisplayUsableBounds(display, &usable)) {
        g.window_auto_sized = true;
        return;
    }
    const float max_w = static_cast<float>(usable.w) * 0.9f;
    const float max_h = static_cast<float>(usable.h) * 0.9f;
    const float scale = std::min(
        1.0f, std::min(max_w / static_cast<float>(width),
                       max_h / static_cast<float>(height)));
    g.window_width = std::max(1u, static_cast<uint32_t>(width * scale));
    g.window_height = std::max(1u, static_cast<uint32_t>(height * scale));
    SDL_SetWindowSize(g.window, static_cast<int>(g.window_width),
                      static_cast<int>(g.window_height));
    SDL_SetWindowPosition(g.window, SDL_WINDOWPOS_CENTERED,
                          SDL_WINDOWPOS_CENTERED);
    g.window_auto_sized = true;
}

bool StartupEngine() {
    engine_create_desc_t desc;
    std::memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.api_version = ENGINE_API_VERSION;
    desc.writable_path_utf8 = "/tmp/aetherkiri-engine-writable";
    desc.cache_path_utf8 = "/tmp/aetherkiri-engine-cache";
    if (engine_create(&desc, &g.engine) != ENGINE_RESULT_OK) {
        fprintf(stderr, "engine_create failed\n");
        return false;
    }
    if(!g.scenario_path.empty()) {
        const void *raw = nullptr;
        if(engine_query_interface(
               g.engine, ENGINE_INTERFACE_VISUAL_DIAGNOSTICS_V1,
               ENGINE_VISUAL_DIAGNOSTICS_INTERFACE_VERSION_1, &raw) !=
               ENGINE_RESULT_OK) {
            fprintf(stderr, "visual diagnostics interface unavailable: %s\n",
                    engine_get_last_error(g.engine));
            return false;
        }
        g.visual_diagnostics =
            static_cast<const engine_visual_diagnostics_v1_t *>(raw);
        raw = nullptr;
        if(engine_query_interface(
               g.engine, ENGINE_INTERFACE_VISUAL_CHECKPOINT_V1,
               ENGINE_VISUAL_CHECKPOINT_INTERFACE_VERSION_1, &raw) !=
               ENGINE_RESULT_OK) {
            fprintf(stderr, "visual checkpoint interface unavailable: %s\n",
                    engine_get_last_error(g.engine));
            return false;
        }
        g.visual_checkpoint =
            static_cast<const engine_visual_checkpoint_v1_t *>(raw);
    }

    engine_option_t option;
    std::memset(&option, 0, sizeof(option));
    option.key_utf8 = ENGINE_OPTION_RENDERER;
    option.value_utf8 = g.use_sdl3_gpu
                            ? "sdl3_gpu"
                            : (g.use_gpu ? ENGINE_RENDERER_GPU_BRIDGE
                                         : ENGINE_RENDERER_SOFTWARE);
    engine_set_option(g.engine, &option);

    if (g.use_sdl3_gpu) {
        if (engine_set_sdl_gpu_device(g.engine, g.gpu_device) !=
            ENGINE_RESULT_OK) {
            fprintf(stderr, "engine_set_sdl_gpu_device failed\n");
            return false;
        }
        fprintf(stderr, "sdl3_gpu: engine_set_sdl_gpu_device OK\n");
    } else if (g.use_gpu) {
        // Inject the renderer: the engine creates/paints its own SDL
        // textures on it; the shell keeps presentation.
        if (engine_set_sdl_renderer(g.engine, g.renderer) != ENGINE_RESULT_OK) {
            fprintf(stderr, "engine_set_sdl_renderer failed\n");
            return false;
        }
    }
    engine_set_surface_size(g.engine, g.surface_width, g.surface_height);

    std::string path = g.game_path;
    {
        std::error_code ec;
        const auto absolute = std::filesystem::absolute(path, ec);
        if (!ec) {
            path = absolute.lexically_normal().string();
        }
    }
    const engine_result_t open_result =
        engine_open_game_async(g.engine, path.c_str(), nullptr);
    if (open_result != ENGINE_RESULT_OK) {
        fprintf(stderr, "engine_open_game_async failed: %s\n",
                engine_get_last_error(g.engine));
        return false;
    }
    fprintf(stderr, "[aetherkiri_engine] opening game: %s\n", path.c_str());
    return true;
}

void RunStartup() {
    uint32_t startup_state = ENGINE_STARTUP_STATE_IDLE;
    if (engine_get_startup_state(g.engine, &startup_state) != ENGINE_RESULT_OK) {
        return;
    }
    if (startup_state == ENGINE_STARTUP_STATE_SUCCEEDED) {
        g.startup_complete = true;
        fprintf(stderr, "[aetherkiri_engine] game started\n");
    } else if (startup_state == ENGINE_STARTUP_STATE_FAILED) {
        fprintf(stderr, "[aetherkiri_engine] engine startup failed\n");
        g.exit_requested = true;
    }
}

void UploadSoftwareFrame(const engine_frame_desc_t &frame_desc) {
    if(g.renderer == nullptr) {
        return;
    }
    if(g.screen == nullptr || g.screen->w != static_cast<int>(frame_desc.width) ||
       g.screen->h != static_cast<int>(frame_desc.height)) {
        if(g.screen != nullptr) SDL_DestroyTexture(g.screen);
        g.screen = SDL_CreateTexture(
            g.renderer, SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_STREAMING, static_cast<int>(frame_desc.width),
            static_cast<int>(frame_desc.height));
        if(g.screen == nullptr) {
            fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
            return;
        }
        SDL_SetTextureScaleMode(g.screen, SDL_SCALEMODE_NEAREST);
        SDL_SetTextureBlendMode(g.screen, SDL_BLENDMODE_NONE);
    }
    const size_t frame_bytes =
        static_cast<size_t>(frame_desc.stride_bytes) * frame_desc.height;
    if (frame_bytes == 0) {
        return;
    }
    if (g.frame_pixels.size() != frame_bytes) {
        g.frame_pixels.resize(frame_bytes);
        g.frame_stride = frame_desc.stride_bytes;
    }
    if (engine_read_frame_rgba(g.engine, g.frame_pixels.data(), frame_bytes) !=
        ENGINE_RESULT_OK) {
        return;
    }
    void *pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(g.screen, nullptr, &pixels, &pitch)) {
        const uint32_t copy_pitch = std::min<uint32_t>(
            static_cast<uint32_t>(pitch), g.frame_stride);
        const auto *src = g.frame_pixels.data();
        auto *dst = static_cast<uint8_t *>(pixels);
        for (uint32_t row = 0; row < frame_desc.height; ++row) {
            std::memcpy(dst + static_cast<size_t>(row) * pitch,
                        src + static_cast<size_t>(row) * g.frame_stride,
                        copy_pitch);
        }
        SDL_UnlockTexture(g.screen);
    }
}

void TickShell(uint32_t delta_ms) {
    engine_tick(g.engine, delta_ms);
    if(g.use_sdl3_gpu) {
        // Finish every compositor command before the published texture is
        // consumed by the swapchain command buffer below.
        engine_submit_sdl_gpu_frame(g.engine);
    }

    engine_frame_desc_t frame_desc;
    std::memset(&frame_desc, 0, sizeof(frame_desc));
    frame_desc.struct_size = sizeof(frame_desc);
    if (engine_get_frame_desc(g.engine, &frame_desc) != ENGINE_RESULT_OK) {
        return;
    }
    if(frame_desc.width > 0 && frame_desc.height > 0 &&
       (frame_desc.width != g.surface_width ||
        frame_desc.height != g.surface_height)) {
        // The game may select its logical resolution during startup. Keep the
        // host surface contract synchronized so C ABI pointer coordinates are
        // not scaled a second time by the draw device.
        if(engine_set_surface_size(g.engine, frame_desc.width,
                                   frame_desc.height) == ENGINE_RESULT_OK) {
            g.surface_width = frame_desc.width;
            g.surface_height = frame_desc.height;
        }
    }

    // Frame-stop detection: the engine stops producing frames once its
    // application terminates (TVPTerminateAsync, e.g. script win.close());
    // there is no host notification, so exit when the serial stops advancing.
    const uint64_t stall_now = SDL_GetTicks();
    if (frame_desc.frame_serial != g.last_frame_serial) {
        g.last_frame_serial = frame_desc.frame_serial;
        g.no_frame_since_ms = 0;
    } else if (g.no_frame_since_ms == 0) {
        g.no_frame_since_ms = stall_now;
    } else if (stall_now - g.no_frame_since_ms >= 3000) {
        fprintf(stderr,
                "[aetherkiri_engine] engine stopped producing frames; exiting\n");
        g.exit_requested = true;
        return;
    }

    if (g.use_gpu && !g.use_sdl3_gpu) {
        uint64_t texture = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t serial = 0;
        if (engine_get_gpu_frame_texture(g.engine, &texture, &width, &height,
                                         &serial) == ENGINE_RESULT_OK &&
            texture != 0) {
            g.gpu_frame_texture = texture;
            g.surface_width = width;
            g.surface_height = height;
        } else {
            g.gpu_frame_texture = 0;
        }
    } else if (g.use_sdl3_gpu) {
        // sdl3_gpu: try the zero-copy swapchain present path (engine publishes
        // the composited SDL_GPUTexture); fall back to CPU readback if the
        // frame is not yet GPU-backed.
        uint64_t texture = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t serial = 0;
        if (engine_get_sdl_gpu_frame_texture(g.engine, &texture, &width,
                                             &height, &serial) ==
                ENGINE_RESULT_OK &&
            texture != 0) {
            g.gpu_frame_texture = texture;
            g.surface_width = width;
            g.surface_height = height;
            g.frame_pixels.clear();
        } else {
            g.gpu_frame_texture = 0;
            UploadSoftwareFrame(frame_desc);
            g.surface_width = frame_desc.width;
            g.surface_height = frame_desc.height;
        }
    } else {
        // Software present via CPU readback.
        UploadSoftwareFrame(frame_desc);
        g.surface_width = frame_desc.width;
        g.surface_height = frame_desc.height;
    }
    AutoSizeWindowForSurface(g.surface_width, g.surface_height);

    DrainRuntimeLogs();
    if(g.scenario) {
        if(g.scenario_started_ms == 0) {
            g.scenario_started_ms = SDL_GetTicks();
            g.scenario_start_rss_bytes = ReadRssBytes();
        }
        g.scenario_frame_times_ms.push_back(delta_ms);
        ReadVisualSnapshot(g.last_visual_snapshot);
        g.scenario->Tick(g.engine, frame_desc.frame_serial, SDL_GetTicks(),
                         g.runtime_logs, g.last_visual_snapshot,
                         CaptureCheckpoint);
        if(g.scenario->failed()) {
            fprintf(stderr, "[aetherkiri_engine] scenario failed: %s\n",
                    g.scenario->failure().c_str());
            if(!g.scenario_failure_saved) {
                std::string capture_error;
                const auto capture_status =
                    CaptureCheckpoint("failure", capture_error);
                if(capture_status == ScenarioRunner::CheckpointStatus::Pending)
                    return;
                if(capture_status == ScenarioRunner::CheckpointStatus::Failed)
                    fprintf(stderr, "failure checkpoint failed: %s\n",
                            capture_error.c_str());
                SaveText(std::filesystem::path(g.scenario_output) /
                             "failure.txt", g.scenario->failure() + "\n");
                g.scenario_failure_saved = true;
            }
            g.exit_requested = true;
            g.scenario_failed = true;
            SaveScenarioReport("failed");
            if(g.scenario_fast_exit) {
                std::fflush(nullptr);
                std::_Exit(EXIT_FAILURE);
            }
        } else if(g.scenario->finished()) {
            fprintf(stderr, "[aetherkiri_engine] scenario completed\n");
            g.exit_requested = true;
            SaveScenarioReport("passed");
            if(g.scenario_fast_exit) {
                std::fflush(nullptr);
                std::_Exit(EXIT_SUCCESS);
            }
        }
    }

    if (!g.screenshot_path.empty()) {
        ++g.frame_count_since_start;
        if (g.frame_count_since_start >= g.screenshot_after_frames) {
            if ((g.use_gpu && !g.use_sdl3_gpu) && g.gpu_frame_texture != 0) {
                SaveGpuFramePpm();
            } else {
                // sdl3_gpu / software: screenshot via CPU readback.
                engine_frame_desc_t shot_desc;
                std::memset(&shot_desc, 0, sizeof(shot_desc));
                shot_desc.struct_size = sizeof(shot_desc);
                if (engine_get_frame_desc(g.engine, &shot_desc) ==
                        ENGINE_RESULT_OK &&
                    shot_desc.width > 0 && shot_desc.height > 0) {
                    std::vector<uint8_t> px(static_cast<size_t>(
                                                shot_desc.stride_bytes) *
                                            shot_desc.height);
                    if (engine_read_frame_rgba(g.engine, px.data(),
                                               px.size()) == ENGINE_RESULT_OK) {
                        SavePpm(g.screenshot_path, px.data(), shot_desc.width,
                                shot_desc.height, shot_desc.stride_bytes);
                    }
                }
            }
            g.screenshot_path.clear();
            g.exit_requested = true;
        }
    }
}

void PresentShell() {
    if (g.use_sdl3_gpu && g.gpu_device != nullptr &&
        g.gpu_frame_texture != 0) {
        // Zero-copy swapchain present: blit the engine's composited
        // SDL_GPUTexture into the swapchain and submit.
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g.gpu_device);
        if (cmd == nullptr) {
            fprintf(stderr, "AcquireGPUCommandBuffer: %s\n", SDL_GetError());
            return;
        }
        SDL_GPUTexture *swap = nullptr;
        Uint32 sw = 0, sh = 0;
        if (!SDL_AcquireGPUSwapchainTexture(cmd, g.window, &swap, &sw, &sh)) {
            fprintf(stderr, "AcquireGPUSwapchainTexture: %s\n", SDL_GetError());
            SDL_SubmitGPUCommandBuffer(cmd);
            return;
        }
        if (swap != nullptr) {
            auto *frame = reinterpret_cast<SDL_GPUTexture *>(
                static_cast<uintptr_t>(g.gpu_frame_texture));
            SDL_GPUBlitInfo blit{};
            blit.source.texture = frame;
            blit.source.mip_level = 0;
            blit.source.layer_or_depth_plane = 0;
            blit.source.x = 0;
            blit.source.y = 0;
            blit.source.w = static_cast<Uint32>(g.surface_width);
            blit.source.h = static_cast<Uint32>(g.surface_height);
            blit.destination.texture = swap;
            blit.destination.mip_level = 0;
            blit.destination.layer_or_depth_plane = 0;
            blit.destination.x = 0;
            const PresentationTransform viewport =
                CalculatePresentationTransform(g.surface_width,
                                               g.surface_height,
                                               static_cast<float>(sw),
                                               static_cast<float>(sh));
            blit.destination.x = static_cast<Uint32>(viewport.offset_x);
            blit.destination.y = static_cast<Uint32>(viewport.offset_y);
            blit.destination.w = static_cast<Uint32>(viewport.width);
            blit.destination.h = static_cast<Uint32>(viewport.height);
            blit.load_op = SDL_GPU_LOADOP_CLEAR;
            blit.clear_color = (SDL_FColor){0, 0, 0, 1};
            blit.flip_mode = SDL_FLIP_NONE;
            blit.filter = SDL_GPU_FILTER_NEAREST;
            blit.cycle = false;
            SDL_BlitGPUTexture(cmd, &blit);
        }
        if (!SDL_SubmitGPUCommandBuffer(cmd)) {
            fprintf(stderr, "SubmitGPUCommandBuffer: %s\n", SDL_GetError());
        } else {
            static bool logged_swapchain_present = false;
            if (!logged_swapchain_present) {
                fprintf(stderr,
                        "sdl3_gpu: presenting native frame via swapchain\n");
                logged_swapchain_present = true;
            }
        }
        if (g.engine != nullptr) {
            engine_flush_released_textures(g.engine);
        }
        return;
    }
    if (g.renderer == nullptr) {
        return;
    }
    SDL_SetRenderDrawColor(g.renderer, 0, 0, 0, 255);
    SDL_RenderClear(g.renderer);

    SDL_Texture *frame = nullptr;
    if (g.use_gpu && !g.use_sdl3_gpu) {
        frame = reinterpret_cast<SDL_Texture *>(
            static_cast<uintptr_t>(g.gpu_frame_texture));
    } else if (!g.use_sdl3_gpu) {
        frame = g.screen;
    }
    // sdl3_gpu without a swapchain frame: nothing to present this tick.
    if (g.startup_complete && frame != nullptr && g.renderer != nullptr) {
        const PresentationTransform viewport = CalculatePresentationTransform(
            g.surface_width, g.surface_height,
            static_cast<float>(g.window_width),
            static_cast<float>(g.window_height));
        SDL_FRect dst{viewport.offset_x, viewport.offset_y, viewport.width,
                      viewport.height};
        SDL_RenderTexture(g.renderer, frame, nullptr, &dst);
        SDL_RenderPresent(g.renderer);
    } else if (g.renderer != nullptr) {
        SDL_RenderPresent(g.renderer);
    }

    // Destroy textures the engine released this frame only after the present
    // above no longer references them.
    if (g.engine != nullptr) {
        engine_flush_released_textures(g.engine);
    }
}

}  // namespace

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
    (void)appstate;
    ParseArgs(argc, argv);
    if (g.game_path.empty()) {
        PrintUsage();
        return SDL_APP_FAILURE;
    }
    if(!g.scenario_path.empty()) {
        ScenarioProfile profile;
        std::string error;
        if(!LoadScenarioProfile(g.scenario_path, profile, error)) {
            fprintf(stderr, "scenario parse failed: %s\n", error.c_str());
            return SDL_APP_FAILURE;
        }
        g.scenario = std::make_unique<ScenarioRunner>(std::move(profile));
    }
    if (!CreatePresentation()) {
        return SDL_APP_FAILURE;
    }
    if (!StartupEngine()) {
        return SDL_APP_FAILURE;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    (void)appstate;
    if (event == nullptr) {
        return SDL_APP_CONTINUE;
    }
    if (event->type == SDL_EVENT_QUIT) {
        g.exit_requested = true;
        return SDL_APP_SUCCESS;
    }
    if (event->type == SDL_EVENT_WINDOW_RESIZED) {
        g.window_width = event->window.data1;
        g.window_height = event->window.data2;
        return SDL_APP_CONTINUE;
    }
    if (event->type == SDL_EVENT_KEY_DOWN &&
        event->key.key == SDLK_ESCAPE && !g.startup_complete) {
        g.exit_requested = true;
        return SDL_APP_SUCCESS;
    }
    if (g.engine != nullptr && g.startup_complete) {
        const PresentationTransform viewport = CalculatePresentationTransform(
            g.surface_width, g.surface_height,
            static_cast<float>(g.window_width),
            static_cast<float>(g.window_height));
        ForwardSdlEventToEngine(g.engine, event, viewport);
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    (void)appstate;

    if (g.exit_requested) {
        return g.scenario_failed ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
    }

    const uint32_t now = SDL_GetTicks();
    const uint32_t delta_ms = static_cast<uint32_t>(
        std::min<uint64_t>(now - g.last_ticks, 1000u));
    g.last_ticks = now;

    if (!g.startup_complete) {
        if (g.engine != nullptr) {
            engine_tick(g.engine, delta_ms);
            RunStartup();
        }
        return SDL_APP_CONTINUE;
    }

    TickShell(delta_ms);
    PresentShell();

    if (g.fps_limit > 0) {
        const uint32_t frame_ms = 1000u / g.fps_limit;
        const uint32_t elapsed = SDL_GetTicks() - g.last_ticks;
        if (elapsed < frame_ms) {
            SDL_Delay(frame_ms - elapsed);
        }
    }
    if(g.exit_requested)
        return g.scenario_failed ? SDL_APP_FAILURE : SDL_APP_SUCCESS;
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    (void)appstate;
    (void)result;
    if (g.engine != nullptr) {
        engine_flush_released_textures(g.engine);
        engine_destroy(g.engine);
        g.engine = nullptr;
    }
    if (g.gpu_device != nullptr) {
        // Detach the GPU device before tearing down the SDL_Renderer used for
        // present so the engine's GPU resources are released while the device
        // is still alive.
        SDL_WaitForGPUIdle(g.gpu_device);
        if (g.window != nullptr) {
            SDL_ReleaseWindowFromGPUDevice(g.gpu_device, g.window);
        }
        SDL_DestroyGPUDevice(g.gpu_device);
        g.gpu_device = nullptr;
    }
    if (g.screen != nullptr) {
        SDL_DestroyTexture(g.screen);
        g.screen = nullptr;
    }
    if (g.renderer != nullptr) {
        SDL_DestroyRenderer(g.renderer);
        g.renderer = nullptr;
    }
    if (g.window != nullptr) {
        SDL_DestroyWindow(g.window);
        g.window = nullptr;
    }
}
