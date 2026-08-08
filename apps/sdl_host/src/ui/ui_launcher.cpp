// ImGui launcher panel: game selection, engine settings, start/stop/restart.

#include "ui_state.h"

#include <imgui.h>

extern FileDialogRequest g_file_dialog;

namespace {

void LauncherHeader(UiState &state) {
    ImGui::SeparatorText(u8"引擎设置");
    ImGui::SetNextItemWidth(240);
    const char *backends[] = {"software", "gpu_bridge", "sdl3_gpu"};
    int backend_idx =
        state.settings.render_backend == "gpu_bridge"
            ? 1
            : (state.settings.render_backend == "sdl3_gpu" ? 2 : 0);
    ImGui::Combo(u8"渲染后端", &backend_idx, backends,
                 IM_ARRAYSIZE(backends));
    state.settings.render_backend =
        backend_idx == 0 ? "software" : (backend_idx == 1 ? "gpu_bridge" : "sdl3_gpu");

    ImGui::SetNextItemWidth(120);
    int fps = static_cast<int>(state.settings.fps_limit);
    ImGui::InputInt(u8"帧率上限 (0=无限)", &fps);
    state.settings.fps_limit = fps < 0 ? 0 : static_cast<uint32_t>(fps);

    ImGui::SetNextItemWidth(200);
    static const char *profiles[] = {"off", "baseline", "input", "render",
                                     "storage", "script", "audio",
                                     "video", "plugin", "system", "full"};
    static int profile_idx = 0;
    ImGui::Combo(u8"诊断 Profile", &profile_idx, profiles,
                 IM_ARRAYSIZE(profiles));
    state.settings.diagnostics_profile =
        profile_idx == 0 ? "" : profiles[profile_idx];

    ImGui::SetNextItemWidth(120);
    int slow = static_cast<int>(state.settings.slow_frame_threshold_ms);
    ImGui::InputInt(u8"慢帧阈值 (ms, 0=关)", &slow);
    state.settings.slow_frame_threshold_ms =
        slow < 0 ? 0 : static_cast<uint32_t>(slow);

    ImGui::SeparatorText(u8"游戏路径");
    ImGui::SetNextItemWidth(320);
    char path_buf[512];
    snprintf(path_buf, sizeof(path_buf), "%s",
             state.settings.game_path.c_str());
    if (ImGui::InputText(u8"游戏目录/XP3", path_buf, sizeof(path_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        state.settings.game_path = path_buf;
    }
    ImGui::SameLine();
    if (ImGui::Button(u8"浏览目录...")) {
        g_file_dialog.folder = true;
        g_file_dialog.active = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(u8"浏览文件...")) {
        g_file_dialog.folder = false;
        g_file_dialog.active = true;
    }
    if (!g_file_dialog.result.empty()) {
        state.settings.game_path = g_file_dialog.result;
        g_file_dialog.result.clear();
    }

    ImGui::SeparatorText(u8"字体");
    ImGui::SetNextItemWidth(320);
    char font_buf[256];
    snprintf(font_buf, sizeof(font_buf), "%s",
             state.settings.font_path.c_str());
    if (ImGui::InputText(u8"字体路径 (ttc/ttf)", font_buf, sizeof(font_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        state.settings.font_path = font_buf;
    }
}

}  // namespace

void UiLauncherPaint(UiState &state) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin(u8"AetherKiri SDL 宿主", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::Text(u8"AetherKiri SDL 宿主 — 启动器");
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
                       state.status_text.c_str());
    ImGui::Spacing();

    LauncherHeader(state);

    ImGui::Separator();
    const bool can_start = !state.settings.game_path.empty();
    if (state.game_state == GameState::NoGame) {
        if (ImGui::Button(u8"启动游戏", ImVec2(120, 0)) && can_start) {
            state.want_start = true;
            state.status_text = u8"启动中...";
        }
    } else if (state.game_state == GameState::Running) {
        if (ImGui::Button(u8"停止", ImVec2(120, 0))) {
            state.want_quit = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(u8"重启", ImVec2(120, 0))) {
            state.want_restart = true;
        }
    } else if (state.game_state == GameState::Launching) {
        ImGui::Text(u8"引擎启动中...");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), u8"引擎错误: %s",
                           state.status_text.c_str());
        if (ImGui::Button(u8"返回", ImVec2(120, 0))) {
            state.game_state = GameState::NoGame;
        }
    }

    if (ImGui::IsItemHovered() && !can_start) {
        ImGui::SetTooltip(u8"请先输入游戏路径或浏览选择");
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader(u8"调试辅助")) {
        ImGui::Checkbox(u8"显示 ImGui Demo 窗口", &state.show_demo_window);
        ImGui::Checkbox(u8"显示调试面板", &state.show_overlay);
        ImGui::TextWrapped(
            u8"快捷键: F12 切换调试面板, Ctrl+Q 退出, Esc 退出游戏");
    }

    ImGui::End();
}
