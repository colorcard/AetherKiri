# AetherKiri → SDL3 渲染/播放层迁移计划

日期: 2026-08-07（更新: 2026-08-11）
状态: 阶段 1 完成；引擎内嵌 SDL 渲染落地；GPU 合成/UI 层为后续
范围: 引擎独立为渲染/播放库（无 Godot），UI 层由 Flutter/Swift 另行构建

## 1. 背景与目标

AetherKiri 当前为 Godot 4.7 宿主应用：C++17 引擎核心 + C ABI + GDExtension
宿主 + Godot UI 壳。目标将引擎剥离为独立渲染/播放库，UI 层（Flutter/Swift）
自建窗口、事件、呈现，通过 C ABI 驱动引擎。

### 非目标

- 不迁移 Web 平台
- 不迁移 Godot UI 壳（游戏库/设置/诊断/IAP）——由新 UI 层重建
- 不保证私有包 AetherInternal 的 E-mote/Live2D 在阶段 1-2 可用（阶段 2 验证）

## 2. 决策记录（ADR）

| # | 决策 | 理由 |
|---|---|---|
| D1 | 平台范围：macOS/iOS + Android + 桌面(Win/Linux)，不含 Web | 用户确认 |
| D2 | 先 CPU readback 跑通，GPU 零拷贝后置 | 用户确认；1080p ≈3.7MB/帧 可接受 |
| D3 | 音频引擎内部播放，SDL3 音频 | 用户确认；WaveMixer.cpp 已是 SDL |
| D4 | ~~主力渲染后端：SDL3 GPU API~~ → **引擎内嵌 SDL 渲染（krkrz 式）**：引擎在宿主注入的 `SDL_Renderer` 上直接创建/绘制纹理（`engine_set_sdl_renderer`），宿主只负责 present；Godot GPU 回调表已删除 | 回调表中间层（17 回调 + 句柄 + 延迟释放）纯属用 C ABI 模拟一体化；krkrz/krkrsdl3 证明引擎内嵌路径更简单 |
| D5 | 渲染后端可插拔：software / gpu_bridge（引擎内嵌 SDL）| `TVPRegisterRenderManager` 工厂现成；GodotRenderManager 已改名 SDLRenderManager（不依赖 Godot/宿主回调） |
| D6 | 宿主 C ABI 作为唯一边界，engine_api.h 基本不变 | 已解耦，仅追加导出接口 |
| D7 | **引擎独立壳 `aetherkiri_engine`**（krkrz 式 SDL 入口：SDL_AppInit/AppEvent/AppIterate/AppQuit）直接跑游戏（无 UI）；`aetherkiri_ui`（原 sdl_host）为 UI 壳（Launcher/Overlay/诊断） | 双形态：引擎可独立运行 + 宿主可选的 UI 壳 |
| D8 | 软件路径 readback 保留（截图/诊断依赖）；GPU 合成操作（blend/triangles/mask）回退软件 | 引擎内嵌后主路径为 SDL 纹理直写；PS 特效等 GPU 合成为后续增强 |

## 3. 目标架构（现状）

```
UI 壳 (aetherkiri_ui: ImGui Launcher/Overlay/诊断)   ── 可选
  │  C ABI (engine_api.h)
  ├─ engine_create / engine_open_game / engine_tick
  ├─ engine_send_input
  ├─ engine_set_sdl_renderer (注入 SDL_Renderer)
  ├─ engine_get_gpu_frame_texture (引擎内 SDL 纹理句柄, 零拷贝 present)
  ├─ engine_read_frame_rgba (软件路径 readback)
  └─ engine_flush_released_textures (present 后清引擎释放队列)
引擎 (含独立壳 aetherkiri_engine: SDL_AppInit/AppIterate 自含窗口/输入/present)
  ├─ 渲染: SDLRenderManager (cpp/core/visual/sdl3/, 引擎内嵌 SDL 纹理直写)
  │        软件路径保留 (CPU readback)
  ├─ 音频: SDL3 (WaveMixer.cpp)
  └─ 平台: SDL3 (窗口/输入/事件)
```

关键点（2026-08-11 架构快照）：

- **引擎内嵌 SDL 渲染**：`cpp/core/visual/sdl3/SdlRenderManager.{h,cpp}`（原
  GodotRenderManager，Godot 依赖与回调表已彻底删除）；`SDLTexture2D` 在宿主
  注入的 renderer 上创建 ABGR8888 流式纹理，合成结果经 `SDL_LockTexture`
  直写/直读；GPU 合成操作返回 false 回退软件。
- **双可执行**：`apps/aetherkiri_engine`（引擎独立壳，无 UI，直接
  `--game <path>` 跑游戏）；`apps/sdl_host`（产物 `aetherkiri_ui`，UI 壳：
  Launcher/Overlay/诊断/引擎级重启）。
- **宿主注入**：`engine_set_sdl_renderer(handle, SDL_Renderer*)`（宿主/壳创建
  renderer 后注入）；present 归宿主（保留 ImGui 叠层）；`engine_get_gpu_frame_texture`
  返回引擎创建的 SDL_Texture* 句柄零拷贝直显。
- **motionplayer**：恢复 origin 的 `176ea404`（L3J 标题转场/yuzu 标题动画时钟），
  缺少它会导致千恋万花主菜单动画结束后背景变黑。

## 4. 现状资产分析（已验证）

- 引擎核心 525 文件/24.3 万行，模块化子库（tjs2/base/environ/visual/...）
- C ABI 570 行，干净平台无关；engine_api 独立共享库 target（OUTPUT_NAME engine_api）
- `visual/godot/` 无 godot_cpp 依赖（GPU 全走 `TVPGodotGpuBridgeCallbacks` 回调表 + uint64 句柄）
- 软件渲染器现成（`debug_cpu`/`software` 路径，GPU fast-path 关闭时纯 CPU 合成）
- GLES 渲染器现成 4987 行（RenderManager_ogl + ANGLE，GPU Bridge 路径，默认关闭）
- 音频已 SDL：WaveMixer.cpp 是唯一真实 SDL API 调用点（其余文件仅前向声明/注释）
- GPU 回调契约现成：`TVPGodotGpuBridgeCallbacks`（22 回调 + batch 表 + 22 blend 模式）
- `engine_media_*` 视频接口现成（read_frame_rgba 可拿帧）
- `engine_set_render_target_iosurface/surface`（macOS/Android 零拷贝）已存在，依赖 GPU Bridge 后端
- 输入语义参考：`apps/godot_app/scripts/main.gd`（key→TVP VK 映射、pointer/scroll/button 约定）

## 5. 阶段计划

### 阶段 1: SDL3 平台层 + 软件渲染跑通（2-4 周）✅ 完成

目标：Linux/macOS 上无 Godot 构建出 `engine_api` + SDL3 宿主，软件渲染播放内置 demo。

任务（全部完成）：

- T1.0 环境基线：vcpkg bootstrap（代理）、sdl3 端口确认、预设确认
- T1.1 依赖切换：`vcpkg.json` sdl2→sdl3（wayland/x11/ibus/alsa 特性）
- T1.2 音频升级：WaveMixer.cpp → `SDL_OpenAudioDeviceStream` + `SDL_PutAudioStreamData`
- T1.3 无 Godot 构建验证：BUILD_GODOT_EXTENSION=OFF 下编译 engine_api，符号检查
- T1.4 SDL3 宿主 `apps/sdl_host/`（窗口/输入/循环/readback 呈现）
  - 输入语义与 main.gd 对齐：key→TVP VK、modifiers、pointer button、scroll、TEXT_INPUT
  - `AETHERKIRI_GAME_PATH` 或 `--game <path>`；数据目录 `~/.local/share/aetherkiri-sdl`
- T1.5 构建入口 `tools/build_sdl_host.sh`
- T1.6 验证与回归

阶段 1 后追加（2026-08-11 架构演进，见第 3 节）：

- **引擎独立壳** `apps/aetherkiri_engine`（krkrz 式 SDL 入口，无 UI 直接跑游戏）
- **引擎内嵌 SDL 渲染**：GodotRenderManager → `cpp/core/visual/sdl3/SdlRenderManager`
  + `SDLTexture2D`（宿主注入 renderer 上直写），Godot GPU 回调表与 `bridge/godot_extension`
  宿主彻底删除
- **UI 壳改名**：`apps/sdl_host` 产物 → `aetherkiri_ui`
- C ABI 增补：`engine_set_sdl_renderer`、`engine_flush_released_textures`、
  `engine_get_gpu_frame_texture`；移除 `engine_register_godot_gpu_bridge` 等回调表 API

### 阶段 2: 渲染性能深化（进行中）

原"SDL3 GPU 回调表"方案已废弃（D4 变更）。当前渲染：

- **软件路径**：CPU 合成 → readback（保留，截图/诊断依赖）
- **gpu_bridge**：引擎内嵌 SDL 纹理直写（ABGR8888 流式 + `SDL_LockTexture`），宿主
  `SDL_RenderTexture` 零拷贝直显（已落地）
- **sdl3_gpu（2026-08-11 落地）**：SDL_GPU shader-pipeline 合成器。rect blend 操作
  （Copy/AlphaBlend/PsScreen/PsAdd/PsSub/PsMul）记录到逐帧 SDL_GPU command buffer，
  用自定义 SPIR-V pipeline 合成；`_d`（读目标）模式、triangles、mask 回退软件保证
  像素一致。宿主注入 `SDL_GPUDevice`（`engine_set_sdl_gpu_device`）。

关键结论（2026-08-11）：

- **语义保真**：demo 逐像素 0.00% 差异（sdl3_gpu vs software），gpu_bridge 也 0%。
  这证明 GPU 合成（非 `_d`）完全正确。
- **swapchain 零拷贝 present（aetherkiri_engine 已落地）**：`engine_get_sdl_gpu_frame_texture`
  发布合成帧的 SDL_GPUTexture，宿主 `SDL_BlitGPUTexture` 到 swapchain 直显（无 readback）。
  截图通过 `engine_read_frame_rgba` 按需下载 GPU 纹理，不影响正常 present 热路径。
  2026-08-11 修复 GPU 帧偏好与条件编译分支后，demo/software 截图 SHA-256 相同、
  千恋万花 1920x1080 启动画面正确，日志确认 `source=sdl_gpu` 和 swapchain present。
- **SDL3 升级 3.2.22 → 3.4.14**（vcpkg baseline 更新）：3.2.22 的 Vulkan backend 对
  自定义 fragment shader 的 descriptor set 布局要求不匹配（fragment 资源必须 set 2、
  fragment uniform set 3、vertex uniform set 1），导致离屏 render pass 无效。
- **benchmark 基线（demo, 2026-08-11 重测）**：
  - software：2483 fps（tick 0.13ms，readback 0.02ms，upload 0.06ms）
  - sdl3_gpu（readback present）：2421 fps（tick 0.14ms）
  - gpu_bridge（零拷贝 present）：6787 fps（tick 0.00ms，无 upload）
  - **结论：性能瓶颈是 present readback/upload，不是合成**。sdl3_gpu 的 swapchain
    present（已落地）应达到与 gpu_bridge 相当的水平（待 sdl_host benchmark 确认）。

后续可深化项：

- **sdl_host 的 swapchain present**：aetherkiri_engine 已落地零拷贝 swapchain present；
  sdl_host（ImGui 叠层）仍走 readback，需 ImGui 改 SDL_GPU 后端才能直显。同时用
  sdl_host benchmark 量化 swapchain present 的 fps。
- **`_d`（读目标）模式 GPU 化**：`DrawRectD` + `blend_d.frag`（scratch 复制 dst +
  双 sampler）已实现为参考，但需全 GPU 合成管线（消除 GPU/CPU 目标内容分叉）才能
  像素一致启用。当前 `_d` 回退软件（0% 一致）。
- **triangles / mask GPU 路径**（`SDL_RenderGeometry` 等价的 SDL_GPU 管线）。
- **SDL_GPU 退出清理（2026-08-11 完成）**：引擎销毁时统一释放仍存活的 GPU 纹理、
  pipelines 和延迟队列并 detach device；宿主 unclaim swapchain 后再销毁 device。
  demo、千恋万花和 sdl_host benchmark 退出均无 `VUID-vkDestroyDevice-device-05137`。
- 软件路径 readback 最终移除（截图/诊断保留）。

### 阶段 3: 纹理导出 + UI 层接入（1-2 周）

- C ABI 新增 `engine_export_texture`（SDL3 互操作 props: MTLTexture/CVPixelBuffer、
  Vulkan image、D3D12 resource、AHardwareBuffer）
- Flutter: Dart FFI + Texture widget（阶段 2 前 ImmutableBuffer 上传 RGBA）
- Swift: MTKView / MTLTexture 互操作
- Apple 文件选择器/StoreKit 复用现有 Swift 文件；Android 权限走 NDK JNI

### 阶段 4: 多平台验证（1 周）

- macOS/iOS: Metal + MTLTexture 导出
- Windows: D3D12（D3D11 fallback）+ 共享纹理
- Linux: Vulkan
- Android: Vulkan + AHardwareBuffer
- 回归: tests/profiles/ probe 体系

## 6. 风险登记

| 风险 | 影响 | 缓解 | 状态 |
|---|---|---|---|
| SDL3 GPU readback 异步时序阻塞 UI 线程 | 高 | begin/poll 接口已设计；阶段 2 早做压力测试 | 待验证 |
| 22 种 blend 模式语义保真（E-mote/Live2D 敏感） | 高 | 阶段 2 逐像素 diff 对照 debug_cpu | 待验证 |
| 私有包 AetherInternal 渲染依赖 Godot API | 中 | 阶段 2 前确认其走回调表；否则适配层 | 待验证 |
| shader 三格式交叉编译工具链搭建 | 中 | CI 固化，配置期暴露 | 待验证 |
| SDL3 音频流模型行为差异 | 低 | 单文件改动，A/B 对比 | 已实现，待验证 |
| SDL3 回调 total_amount 与混音格式转换比 | 低 | 按字节速率比例换算 put 量 | 已实现 |
| vcpkg baseline 需要全量历史 | 低 | git fetch --unshallow | 已解决 |

## 7. 验证策略

- 渲染一致性: 关键帧截图逐像素对比（software vs godot debug_cpu vs sdl3_gpu）
- 音频: A/B 行为对比 + suspend/resume 压力
- 性能: tick 耗时 / readback 耗时 / FPS 基线，阶段间对比
- 稳定性: 启动-播放-存档-退出 ×10 循环，泄漏抽查
- 平台: 阶段 4 逐平台冒烟 + probe 回归

## 8. 阶段 1 完成定义（DoD）

- [x] `tools/build_sdl_host.sh` 在 Linux 产出可运行 `aetherkiri_sdl`，无 Godot 参与
- [x] sdl_host 完整播放内置 demo：画面（截图验证）、输入（点击跳转 first_zh_cn.ks）、音频（无崩溃）、退出
- [x] 软件渲染帧正常（640x480 游戏逻辑分辨率）
- [x] `engine_api` 导出符号无 godot 依赖（3563 NCB 插件注册符号保留）
- [x] SDL3 音频流 API 接入（WaveMixer.cpp，桌面走 OpenAL 后端）
- [ ] 渲染一致性与性能基线 vs Godot debug_cpu（需 Godot 环境，暂缓）

### 阶段 1 发现并修复的问题

| # | 问题 | 修复 |
|---|---|---|
| P1 | Linux engine_api 链接失败（krkr2plugin 重复 whole-archive）——项目 Linux 桌面从未构建成功 | Linux 分支改显式 `LINKER:SHELL --whole-archive` + 子插件显式链接 |
| P2 | 引擎输入坐标空间 = 表面尺寸，sdl_host 用帧尺寸做 scale 导致点击 1/4 错位 | 检测帧尺寸后同步 `engine_set_surface_size` |
| P3 | 相对游戏路径被引擎错误绝对化 | sdl_host 传绝对路径 |
| P4 | Linux 无系统字体回退（引擎只找工作目录 NotoSansCJK-Regular.ttc） | 开发期复制字体文件；后续给引擎补 Linux 系统字体枚举 |
| P5 | vcpkg shallow clone 无法解析 baseline port | `git fetch --unshallow` |
| P6 | 窗口画面偏红（R 通道=alpha）——`SDL_PIXELFORMAT_RGBA8888` 在小端机器是 ABGR 内存布局 | sdl_host 改用 `SDL_PIXELFORMAT_ABGR8888`（内存 [R,G,B,A]）|
| P7 | SDL3 渲染器目标初始为红色垃圾、纹理默认混合模式为 BLEND | `SDL_RenderClear` 黑底 + `SDL_SetTextureBlendMode(NONE)` |
| P8 | 引擎终止（脚本 `win.close()` → `TVPTerminateAsync`）无宿主通知，sdl_host 空转不退出 | sdl_host 检测 frame_serial 3 秒无推进自动退出 |

## 9. 自检 demo（demos/aetherkiri-test/）

用于快速定位引擎渲染/音频/输入/层级问题的纯 TJS 测试游戏（不依赖 KAG 系统）。

```bash
# 生成资源（WAV）并运行
demos/aetherkiri-test/build.sh
out/linux/debug/apps/sdl_host/aetherkiri_sdl --game demos/aetherkiri-test/data --fps 60
```

覆盖项与操作：

| 测试项 | 验证内容 | 操作 |
|---|---|---|
| 画面 | 16 级灰度色带、RGB 三原色、网格、半透明色块/文字、文字渲染 | 目视 + 截图 |
| 音乐 | `WaveSoundBuffer` 播放 WAV（open/play/stop） | 底部 BGM 按钮 / X 键 / 右键 |
| 触控定位 | 鼠标坐标映射（窗口→引擎 1:1）、点击命中 | 移动=绿色十字+坐标，左键=青色十字 |
| 层级 | `bringToFront` 层序轮换、`visible` 显隐 | Z 键 / Layer 按钮；C 键 / Pattern 按钮 |
| 键盘 | TVP 虚拟键码分发（`win.onKeyDown`） | Z/X/C/S/Esc |
| 退出 | `win.close()` → 引擎终止 → sdl_host 帧停止检测退出 | Esc |

编写要点（KiriKiri 绘制/事件机制，踩坑记录）：

- **层树**：`win.add/remove` 只维护窗口对象引用清单（ObjectVector），**不是层树**；渲染顺序由 `Layer(parent/children/order)` 决定，移除层需操作层树（动态层用 `visible=false` 或重建更简单）
- **事件命中**：层默认 `HitThreshold=16`，**alpha<16 的透明像素不接收鼠标事件**；全屏交互层需 `layer.hitThreshold = 0`
- **透明层绘制**：绘制用 `colorRect(x,y,w,h,color,opacity)`（显式 opacity）；`fillRect(...,0)` 是全透明填充（KiriKiri 标准清空方式，见 EditLayer/CheckBoxLayer）；`colorRect(opacity=0)` 是 no-op（RemoveConstOpacity(0)），`opacity<0` 才擦除
- **ltOpaque 层**未填充区域显示**白色**（层初始图像为白且忽略 alpha）——全屏不透明层必须显式填充（KAG 用 `fillRect(0,0,w,h,0)` 填黑）
- **TJS 键盘键码**：`onKeyDown(key)` 的 key 是**数字**（TVP VK，如 'Z'=0x5A）；字符串字面量 `'z'` 永不匹配
- **鼠标按钮**：mbLeft=0 / mbRight=1 / mbMiddle=2；事件签名 `onMouseDown(x,y,btn,flags)`、`onMouseMove(x,y,flags)`

## 10. 参考

- README.md / doc/development.md / doc/verified_games.md
- tests/profiles/ 现有 probe 配置
- bridge/engine_api/include/engine_api.h（C ABI 契约）
- bridge/engine_api/include/engine_options.h（选项 key）
- cpp/core/visual/godot/GodotGpuBridge.h（GPU 回调契约）
- apps/godot_app/scripts/main.gd（输入/按键语义参考）
