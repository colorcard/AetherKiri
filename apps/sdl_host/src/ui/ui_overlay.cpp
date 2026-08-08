// ImGui debug overlay: performance, memory, log and control panels shown
// while a game is running. Consumed data comes from the host loop (timing,
// engine_get_memory_stats, drained logs/diagnostics).

#include "ui_state.h"

#include <cinttypes>
#include <imgui.h>

// Live data the host loop fills in each frame (defined in main.cpp).
extern OverlayStats g_overlay_stats;

void UiOverlayPaint(UiState &state) {
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin(u8"AetherKiri 调试", nullptr, ImGuiWindowFlags_NoCollapse);

    if (ImGui::BeginTabBar("overlay_tabs")) {
        // ---- 性能 ----
        if (ImGui::BeginTabItem(u8"性能")) {
            const OverlayStats &s = g_overlay_stats;
            if (s.stats_valid) {
                ImGui::Text(u8"FPS: %.1f  (%ux%u)", s.fps, s.surface_w,
                            s.surface_h);
                ImGui::Separator();
                ImGui::Text(u8"帧耗时分解:");
                ImGui::Text(u8"  tick:      %.3f ms", s.tick_ms);
                ImGui::Text(u8"  readback:  %.3f ms", s.readback_ms);
                ImGui::Text(u8"  upload:    %.3f ms", s.upload_ms);
                ImGui::Text(u8"  total:     %.3f ms", s.total_ms);
                ImGui::ProgressBar(
                    s.total_ms > 0.0f
                        ? static_cast<float>(s.total_ms / 33.3f)
                        : 0.0f,
                    ImVec2(-1, 0), "");
                ImGui::Spacing();
            } else {
                ImGui::Text(u8"等待引擎帧...");
            }
            if (s.mem_valid) {
                ImGui::Separator();
                ImGui::Text(u8"内存:");
                ImGui::Text(u8"  进程 RSS:   %.1f MB",
                            s.rss_bytes / 1048576.0);
                ImGui::Text(u8"  图形缓存:  %.1f MB",
                            s.gfx_cache_bytes / 1048576.0);
                ImGui::Text(u8"  XP3 缓存:  %.1f MB",
                            s.xp3_cache_bytes / 1048576.0);
                ImGui::Text(u8"  PSB 缓存:  %.1f MB",
                            s.psb_cache_bytes / 1048576.0);
            }
            ImGui::EndTabItem();
        }

        // ---- 日志 ----
        if (ImGui::BeginTabItem(u8"日志")) {
            ImGui::Checkbox(u8"自动滚动", &state.log_auto_scroll);
            ImGui::Separator();
            ImGui::BeginChild("log_scroll", ImVec2(0, -ImGui::GetTextLineHeightWithSpacing() * 1.2f), true);
            ImGui::TextUnformatted(g_overlay_stats.log_buffer.c_str());
            if (state.log_auto_scroll) {
                ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
            if (ImGui::Button(u8"清空")) {
                g_overlay_stats.log_buffer.clear();
            }
            ImGui::EndTabItem();
        }

        // ---- 诊断 ----
        if (ImGui::BeginTabItem(u8"诊断")) {
            ImGui::TextWrapped(u8"诊断事件 (--diagnostics 开启时可见)");
            ImGui::BeginChild("diag_scroll", ImVec2(0, -40), true);
            ImGui::TextUnformatted(g_overlay_stats.diag_buffer.c_str());
            ImGui::EndChild();
            if (ImGui::Button(u8"清空")) {
                g_overlay_stats.diag_buffer.clear();
            }
            ImGui::EndTabItem();
        }

        // ---- 控制 ----
        if (ImGui::BeginTabItem(u8"控制")) {
            ImGui::TextWrapped(u8"引擎控制与工具:");
            if (ImGui::Button(u8"截图 (PPM)", ImVec2(160, 0))) {
                state.want_screenshot = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(u8"暂停", ImVec2(100, 0))) {
                state.want_pause = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(u8"恢复", ImVec2(100, 0))) {
                state.want_resume = true;
            }
            ImGui::Spacing();
            if (ImGui::Button(u8"重启引擎", ImVec2(160, 0))) {
                state.want_restart = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(u8"退出", ImVec2(100, 0))) {
                state.want_quit = true;
            }
            if (!state.last_benchmark.empty()) {
                ImGui::Separator();
                ImGui::TextWrapped("%s", state.last_benchmark.c_str());
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}
