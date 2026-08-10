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
#include <string>
#include <vector>

#include "engine_api.h"
#include "engine_options.h"
#include "host_input.h"

namespace {

constexpr uint32_t kDefaultSurfaceWidth = 640;
constexpr uint32_t kDefaultSurfaceHeight = 480;

struct ShellState {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    engine_handle_t engine = nullptr;

    std::string game_path;
    bool use_gpu = false;
    uint32_t fps_limit = 0;

    std::string screenshot_path;
    int screenshot_after_frames = 0;
    int frame_count_since_start = 0;

    bool startup_complete = false;
    bool exit_requested = false;

    uint32_t surface_width = kDefaultSurfaceWidth;
    uint32_t surface_height = kDefaultSurfaceHeight;
    uint32_t window_width = kDefaultSurfaceWidth;
    uint32_t window_height = kDefaultSurfaceHeight;

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
            "  --render-backend <name>    software | gpu_bridge (default software)\n"
            "  --screenshot <path>        save a PPM after N frames\n"
            "  --screenshot-frames <n>    frames before screenshot (default 180)\n");
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
            } else if (backend == "gpu_bridge" || backend == "sdl3_gpu") {
                g.use_gpu = true;
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
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            PrintUsage();
            std::exit(0);
        }
    }
}

void SavePpm(const std::string &path, const uint8_t *rgba, uint32_t width,
             uint32_t height, uint32_t stride) {
    FILE *file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        fprintf(stderr, "screenshot: cannot open %s\n", path.c_str());
        return;
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
                                static_cast<int>(g.window_height), 0);
    if (g.window == nullptr) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    g.renderer = SDL_CreateRenderer(g.window, nullptr);
    if (g.renderer == nullptr) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    if (!g.use_gpu) {
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

    engine_option_t option;
    std::memset(&option, 0, sizeof(option));
    option.key_utf8 = ENGINE_OPTION_RENDERER;
    option.value_utf8 =
        g.use_gpu ? ENGINE_RENDERER_GPU_BRIDGE : ENGINE_RENDERER_SOFTWARE;
    engine_set_option(g.engine, &option);

    if (g.use_gpu) {
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
    if (g.screen == nullptr) {
        return;
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

    engine_frame_desc_t frame_desc;
    std::memset(&frame_desc, 0, sizeof(frame_desc));
    frame_desc.struct_size = sizeof(frame_desc);
    if (engine_get_frame_desc(g.engine, &frame_desc) != ENGINE_RESULT_OK) {
        return;
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

    if (g.use_gpu) {
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
    } else {
        UploadSoftwareFrame(frame_desc);
        g.surface_width = frame_desc.width;
        g.surface_height = frame_desc.height;
    }

    if (!g.screenshot_path.empty()) {
        ++g.frame_count_since_start;
        if (g.frame_count_since_start >= g.screenshot_after_frames) {
            if (g.use_gpu) {
                SaveGpuFramePpm();
            } else if (!g.frame_pixels.empty()) {
                SavePpm(g.screenshot_path, g.frame_pixels.data(),
                        g.surface_width, g.surface_height, g.frame_stride);
            }
            g.screenshot_path.clear();
            g.exit_requested = true;
        }
    }
}

void PresentShell() {
    SDL_SetRenderDrawColor(g.renderer, 0, 0, 0, 255);
    SDL_RenderClear(g.renderer);

    SDL_Texture *frame = nullptr;
    if (g.use_gpu) {
        frame = reinterpret_cast<SDL_Texture *>(
            static_cast<uintptr_t>(g.gpu_frame_texture));
    } else {
        frame = g.screen;
    }
    if (g.startup_complete && frame != nullptr) {
        SDL_FRect dst{0.0f, 0.0f, static_cast<float>(g.window_width),
                      static_cast<float>(g.window_height)};
        SDL_RenderTexture(g.renderer, frame, nullptr, &dst);
    }
    SDL_RenderPresent(g.renderer);

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
        const float scale_x =
            static_cast<float>(g.surface_width) /
            static_cast<float>(std::max(g.window_width, 1u));
        const float scale_y =
            static_cast<float>(g.surface_height) /
            static_cast<float>(std::max(g.window_height, 1u));
        ForwardSdlEventToEngine(g.engine, event, scale_x, scale_y);
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    (void)appstate;

    if (g.exit_requested) {
        return SDL_APP_SUCCESS;
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
    return g.exit_requested ? SDL_APP_SUCCESS : SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    (void)appstate;
    (void)result;
    if (g.engine != nullptr) {
        engine_flush_released_textures(g.engine);
        engine_destroy(g.engine);
        g.engine = nullptr;
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
