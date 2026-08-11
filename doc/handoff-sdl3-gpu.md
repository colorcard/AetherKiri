# AetherKiri SDL_GPU 阶段 2 交接文档

日期: 2026-08-11
作者: 交接给后续 AI（前序会话已完成 SDL_GPU GPU 合成后端）
仓库: /home/colorcard/AetherKiri
分支: main（本地，未推送）

---

## 1. 当前状况一句话

sdl3_gpu 后端（SDL_GPU shader-pipeline GPU 合成）已完成并验证：demo 与 software
渲染**逐像素 0.00% 一致**，`aetherkiri_engine` 已通过 swapchain 显示原生 GPU 帧，
千恋万花 1920x1080 启动画面正确，退出无 Vulkan 子资源泄漏警告（见 §6）。

## 2. 已完成的工作（阶段 2 主线）

### 2.1 七个已提交 commit（本地 main）

```
57e1d7c5 fix(sdl3_gpu): present native frames and clean up GPU resources
06424285 fix(sdl3_gpu): handle dst==src self-copy via scratch texture
84956bd6 docs: record sdl3_gpu swapchain present + SDL_GPU shader layout pitfalls
21ff6533 feat(sdl3_gpu): zero-copy swapchain present for the standalone engine shell
936b60bf feat(sdl_host): support --render-backend sdl3_gpu; document phase-2 baselines
1355ddf3 fix(sdl3_gpu): keep destination-read (_d) blends on the software delegate
15e287e3 feat(sdl3_gpu): SDL_GPU shader-pipeline compositor backend (phase 2)
```

tracked 工作区干净，存在未跟踪的本地字体、demo、存档与参考目录，均不得提交。未推送。

### 2.2 具体成果

- **SDL3 升级 3.2.22 → 3.4.14**：`vcpkg-configuration.json` 的 baseline 从
  `b1e15efef...` 更新到 `d92484ed3c5020c6679d095ad3e5add907887b62`。3.2.22 的
  Vulkan backend 对自定义 shader 的 descriptor set 布局不匹配，导致离屏 render
  pass 静默无效。**SDL 最新 release 就是 3.4.14**（2026-08-03），无更新版。
- **sdl3_gpu 渲染后端**：`cpp/core/visual/sdl3/SdlGpuRenderManager.{h,cpp}` +
  `cpp/core/environ/sdl/sdl_gpu_backend.{h,cpp}`。krkrz/krkrsdl3 双管理器同构。
  rect blend（Copy/AlphaBlend/PsScreen/PsAdd/PsSub/PsMul）走 GPU；
  `_d`（读目标）模式 + triangles + mask 回退软件。
- **C ABI 新增**：
  - `engine_set_sdl_gpu_device`（注入 SDL_GPUDevice）
  - `engine_submit_sdl_gpu_frame`
  - `engine_get_sdl_gpu_frame_texture`（取合成帧 SDL_GPUTexture，swapchain present 用）
  - 全部已接 dispatch 层（engine_api_dispatch.cpp）+ legacy rename。
- **swapchain 零拷贝 present**（aetherkiri_engine）：宿主 acquire swapchain →
  `SDL_BlitGPUTexture` 引擎帧 → submit。截图走 `engine_read_frame_rgba`。
- **shader 工具链**：`glslangValidator` 编译 GLSL → SPIR-V → 头文件
  （`cpp/core/visual/sdl3/shaders/*.spv.h`）。三格式跨平台预留（MSL/DXIL）。
- **验证结果**：
  - demo（`demos/aetherkiri-test/data`）：software/gpu_bridge/sdl3_gpu 三后端
    全部 0.00% 像素差异。
  - 千恋万花（`demos/【krkr】千恋万花/千恋＊万花`）：sdl3_gpu 截图 87.7% 彩色正常。
  - ci-tests（motionplayer-dll）：101 测试 100 通过（仅 E-mote mouth 既有失败）。
  - benchmark（demo，sdl_host）：software 2483fps / sdl3_gpu(readback) 2421fps /
    gpu_bridge 6787fps。性能瓶颈是 present readback，非合成。

## 3. 关键文件索引

| 文件 | 作用 |
|---|---|
| `cpp/core/visual/sdl3/SdlGpuRenderManager.{h,cpp}` | SDL_GPU 渲染后端（纹理/合成/blend 表） |
| `cpp/core/visual/sdl3/SdlRenderManager.{h,cpp}` | 原 SDL_Renderer 后端（gpu_bridge，未破坏） |
| `cpp/core/visual/sdl3/shaders/*.spv.h` | 编译好的 SPIR-V 头文件 |
| `cpp/core/environ/sdl/sdl_gpu_backend.{h,cpp}` | SDL_GPU 设备 + 帧 command buffer 生命周期 |
| `cpp/core/environ/stubs/ui_stubs.cpp` | 引擎 present 发布（PublishHostGpuFrame / Sdl 版本） |
| `apps/aetherkiri_engine/main.cpp` | 独立壳（swapchain present / readback present / 截图） |
| `apps/sdl_host/src/main.cpp` | UI 壳（Launcher/Overlay，sdl3_gpu 走 readback present） |
| `bridge/engine_api/` | C ABI（新增 4 个 sdl3_gpu 相关函数） |
| `doc/migration-sdl3.md` | 迁移文档（阶段 2 状态 + benchmark + 后续项） |
| `example/krkrsdl3/` | GitHub 拉取的 krkrsdl3 参考（用 OpenGL，层合成为 CPU） |

## 4. SDL_GPU 关键技术点（踩坑，务必读）

1. **shader descriptor set 布局是硬约束**（SDL 3.x Vulkan backend）：
   - vertex uniform 用 **set 1**，binding 0
   - fragment sampler 用 **set 2**，binding 0（多 sampler 用 binding 0,1,...）
   - fragment uniform 用 **set 3**，binding 0
   - 用错 set → 离屏 render pass 静默无效（VUID layout 错误）。
2. **同一 command buffer 里 copy pass 与 render pass 不能同时开**：先
   `TVPEnsureSdlGpuRenderPassReady()`（结束 open copy pass）再 BeginRenderPass。
3. **SDL_GPU 禁止同一 render pass 中目标纹理同时被采样**（`_d` 不能直接做）。
4. **dst==src 自复制**（KiriKiri 合法操作，如千恋万花顶部横幅内部滚动）：
   必须先把 dst 复制到 scratch 纹理再采样 scratch（已实现，见 DrawRect）。
   不处理会导致 VUID image-layout 验证错误 + 画面错误。
5. **`_d`（读目标）blend**：krkrz 用 `SetTargetAsSrc`（GL framebuffer fetch），
   SDL_GPU 无此能力。`DrawRectD` + `blend_d.frag`（scratch 复制 + 双 sampler）已实现
   但**未启用**（GPU/CPU 目标内容分叉导致像素不一致），当前 `_d` 回退软件。
6. **纹理 usage**：合成纹理用 `SAMPLER | COLOR_TARGET`。SDL 推断 default usage 时
   SAMPLER 优先。

## 5. 构建与运行

```bash
# 构建（需代理下载）
export http_proxy=http://127.0.0.1:10808 https_proxy=... all_proxy=socks5://127.0.0.1:10808
./tools/build_sdl_host.sh linux debug --jobs=8

# 运行（必须带 vcpkg 运行库）
export LD_LIBRARY_PATH=$PWD/out/linux/debug/vcpkg_installed/x64-linux/lib
./out/linux/debug/apps/aetherkiri_engine/aetherkiri_engine \
  --game "demos/【krkr】千恋万花/千恋＊万花" --render-backend sdl3_gpu --fps 0

# 截图验证（引擎侧 PPM）
./out/linux/debug/apps/aetherkiri_engine/aetherkiri_engine \
  --game demos/aetherkiri-test/data --render-backend sdl3_gpu --fps 0 \
  --screenshot /tmp/x.ppm --screenshot-frames 60

# benchmark（sdl_host）
./out/linux/debug/apps/sdl_host/aetherkiri_ui --game demos/aetherkiri-test/data \
  --render-backend sdl3_gpu --fps 0 --benchmark 8
```

## 6. 已修复：运行窗口全黑与退出资源泄漏

**原现象**：
- 千恋万花 / demo 用 `--render-backend sdl3_gpu` 运行，**窗口全黑**。
- 但 `--screenshot` 生成的 PPM **内容正确**（截图走 `engine_read_frame_rgba`，
  读的是 CPU readback 帧，有内容）。
- 运行日志显示 `host final frame: source=cpu_store`（说明引擎发布的是 CPU 帧，
  而非 SDL_GPU 帧）。swapchain present 未生效。

**根因分析**：
1. `engine_api.cpp` 的 `ShouldUseHostGpuFrameForRenderer("sdl3_gpu")` 返回 **false**
   （为规避 SDL_Renderer 路径误发布 GPU 帧而加，见 commit 21ff6533）。
2. 因此引擎 `TVPHostSetPreferGpuFrame(false)` → `g_host_prefer_gpu_frame = false` →
   `ui_stubs.cpp::UpdateDrawBuffer` 的 `PublishHostGpuFrameSdl` 分支**不执行**（外层
   if 要求 prefer_gpu_frame）→ `engine_get_sdl_gpu_frame_texture` 返回 NOT_SUPPORTED。
3. 引擎回退 `StoreLatestCpuFrameFromTexture`（CPU readback）。
4. 宿主 `aetherkiri_engine`：`engine_get_sdl_gpu_frame_texture` 失败 →
   `g.gpu_frame_texture = 0` → PresentShell 的 swapchain 分支不执行。
5. 但 sdl3_gpu 模式下 **CreatePresentation 不创建 SDL_Renderer / g.screen**
   （commit 21ff6533 改成 swapchain 专用）→ PresentShell 的 readback 分支
   `g.renderer == nullptr` 直接 return → **窗口什么都不画 → 全黑**。

**已实施修复**：
- `ShouldUseHostGpuFrameForRenderer("sdl3_gpu")` 返回 true；SDL_GPU 发布路径移到
  `KRKR_ENABLE_GPU_BRIDGE` 两个编译分支共用位置，并同步 hybrid CPU fallback 后的
  最终纹理再发布。
- `engine_read_frame_rgba` 在 CPU 帧缓存为空时按需下载已发布的 SDL_GPUTexture，
  因此修复窗口后截图仍保持正确，正常 present 不发生 readback。
- swapchain present 后刷新延迟纹理；退出时释放全部存活的 SdlGpuTexture2D、pipeline、
  scratch 与延迟队列，清空全局 device；两个宿主在 DestroyGPUDevice 前 unclaim window。

**验证结果**：demo 的 software/gpu_bridge/sdl3_gpu 三份 640x480 PPM SHA-256 完全
相同（0/921600 通道差异）；千恋万花输出正确的 1920x1080 启动画面，日志显示
`source=sdl_gpu` 与 `presenting native frame via swapchain`；demo、千恋万花及 sdl_host
benchmark 退出均无 Vulkan validation 子资源泄漏。

## 7. 已知问题 / 待办

1. **`_d`（读目标）模式 GPU 化**：`DrawRectD` + `blend_d.frag` 已实现为参考但未启用。
   需全 GPU 合成管线（消除 GPU/CPU 目标内容分叉）才能像素一致启用。当前回退软件。
2. **triangles / mask GPU 路径**：仍回退软件。
3. **sdl_host 的 swapchain present**：sdl_host 用 ImGui + SDL_Renderer，swapchain
   present 需要 ImGui 改 SDL_GPU 后端（大工程）。当前 sdl_host 的 sdl3_gpu 走
   readback present（2421fps）。
4. **benchmark 量化 swapchain present fps**：aetherkiri_engine 无 benchmark 命令，
   需加或临时脚本。
5. **千恋万花截图后退出 SIGSEGV**：既有问题（软件/gpu_bridge 同样崩），NVIDIA GL
   驱动清理 bug，与 sdl3_gpu 无关。
6. **BUILD_GPU_BRIDGE=ON 无 Godot 链接**：交叉编译能编译 SDL_GPU 改动，但最终
   `engine_api` 链接存在既有 `TVPKrkrGLESCreateModuleObject` 重复定义，与本修复无关。
7. **推送**：本地 main 未 push。需要时 `git push fork main`（代理）。

## 8. 环境备注

- 双 GPU：NVIDIA + AMD，Vulkan ICD 齐全。SDL_GPU 用 SPIR-V（Vulkan）驱动。
- 系统依赖已装：`libvulkan-dev glslang-tools spirv-cross spirv-tools
  vulkan-validationlayers autoconf-archive`（sudo 密码：liwq6688）。
- vcpkg 缓存：`.aetherkiri-cache/`（含 vcpkg/ccache/下载，勿删）。
- 网络需代理：`http_proxy=http://127.0.0.1:10808 https_proxy=... all_proxy=socks5://127.0.0.1:10808`。
- 参考：`example/krkrsdl3/`（GitHub 克隆，OpenGL + CPU 层合成，无 SDL_GPU 参考价值
  于本问题，但其"层合成放 CPU"的架构决策是避开 SDL_GPU 局限的可行方案）。

## 9. 交接给下一 AI 的建议起点

1. 保持 §6 的 GPU 帧发布、按需 readback 与 teardown 生命周期回归测试。
2. 修复 `BUILD_GPU_BRIDGE=ON` 无 Godot 构建的既有重复符号链接问题。
3. 再决定是否推进 §7 的 `_d` GPU 化、triangles 或 sdl_host swapchain。
