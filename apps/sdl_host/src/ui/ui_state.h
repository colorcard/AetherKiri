#pragma once

#include <cstdint>
#include <string>

// Shared UI state between the ImGui launcher and the debug overlay.
// Settings live in memory for now (no persistence).

struct UiSettings {
    std::string game_path;
    std::string render_backend = "software";
    uint32_t fps_limit = 0;           // 0 = unlimited
    std::string diagnostics_profile;  // empty = off
    uint32_t slow_frame_threshold_ms = 20;
    std::string font_path = "NotoSansCJK-Regular.ttc";
    int window_width = 1280;
    int window_height = 720;
};

enum class GameState {
    NoGame,
    Launching,
    Running,
    Error,
};

// Live overlay data the host loop fills each frame.
struct OverlayStats {
    double fps = 0.0;
    double tick_ms = 0.0;
    double readback_ms = 0.0;
    double upload_ms = 0.0;
    double total_ms = 0.0;
    bool stats_valid = false;

    uint32_t surface_w = 0;
    uint32_t surface_h = 0;

    // Memory (from engine_get_memory_stats).
    uint64_t rss_bytes = 0;
    uint64_t gfx_cache_bytes = 0;
    uint64_t xp3_cache_bytes = 0;
    uint64_t psb_cache_bytes = 0;
    bool mem_valid = false;

    // Logs / diagnostics ring (filled by the host loop).
    std::string log_buffer;
    std::string diag_buffer;
};

// Native file dialog state (SDL3 async API; polled by the host loop).
struct FileDialogRequest {
    bool active = false;
    bool folder = false;  // true = folder picker, false = file picker
    std::string result;
};

struct UiState {
    UiSettings settings;
    GameState game_state = GameState::NoGame;
    bool show_overlay = true;      // F12 toggles
    bool show_launcher = true;     // launcher panel (NoGame / overlay in game)
    bool show_demo_window = false; // ImGui demo (debug aid)

    // Engine status text shown in the launcher.
    std::string status_text;

    // Overlay: requested actions from the panel (consumed by the host loop).
    bool want_start = false;     // launcher: start the engine + game
    bool want_pause = false;
    bool want_resume = false;
    bool want_restart = false;
    bool want_quit = false;
    bool want_screenshot = false;

    // Diagnostics toggle requested from the panel.
    std::string diagnostics_request;  // "" = none, "set:<profile>", "off"

    // Scrolling log window state.
    bool log_auto_scroll = true;

    // Benchmark summary captured on completion (shown in overlay).
    std::string last_benchmark;
};

// Launcher panel: returns true when the UI consumed the frame (paint only).
void UiLauncherPaint(UiState &state);

// Debug overlay panel (game running).
void UiOverlayPaint(UiState &state);
