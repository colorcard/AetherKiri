# AGENTS.md

AetherKiri：Godot 宿主 + C ABI + C++17 KiriKiri2 引擎核心。
当前主线：引擎独立为 SDL3 渲染/播放库（阶段 1 完成：SDL3 宿主 + 软件渲染；
阶段 2 规划中：SDL3 GPU 渲染后端）。详细迁移记录见 `doc/migration-sdl3.md`。

## 构建（Linux）

- vcpkg 全量依赖首次构建极慢（30-60 分钟），缓存于 `.aetherkiri-cache/`（
  `AETHERKIRI_CACHE_DIR`，含 vcpkg/ccache/下载）。勿删，除非要回收空间。
- 无 Godot 的 sdl_host 构建：`./tools/build_sdl_host.sh linux debug`
  （内部 `cmake --preset "Linux Debug Config" -DBUILD_GODOT_EXTENSION=OFF
  -DBUILD_SDL_HOST=ON`）。注意 preset 名是 `Linux Debug Config` 而非 `Linux Debug`。
- 完整构建（Godot 生态）用 `./build.sh <platform> <debug|release>`。
- 网络下载（vcpkg/git）需代理：
  `export http_proxy=http://127.0.0.1:10808 https_proxy=... all_proxy=socks5://127.0.0.1:10808`
- 系统依赖：`libopenal-dev`（OpenAL 头）、`nasm/yasm`（ffmpeg port）、
  `bison/flex`（gettext port）、`libxrender-dev`（libgdiplus port）。

## 构建（macOS）

- 同一脚本：`./tools/build_sdl_host.sh macos debug`（arm64，preset
  `MacOS Debug Config`，deployment target 13.0）。需 `brew install ninja`。
- **vcpkg 缓存复用**：脚本优先用现成 vcpkg checkout
  （`AETHERKIRI_REF_VCPKG_ROOT`，默认 `.devtools/vcpkg`，仓库根下；旧位置
  `~/Documents/AetherKiri/.devtools/vcpkg` 作兜底）的 downloads/buildtrees
  存量，避免重下源码；新编译产物进 `.aetherkiri-cache/vcpkg-binaries`。
  勿用那个 checkout 做其他 triplet 的并发构建。
- **vcpkg.json 的 sdl3 features**（wayland/x11/ibus/alsa）已限 linux——
  不要改回无条件，否则 macOS 报 unsupported。
- 网络：github 直连可达，但 **ffmpeg.org 被墙**（vcpkg 内置 curl 报 SSL
  connect error 35）。手动预下载 tarball 到
  `$VCPKG_DOWNLOADS/ffmpeg-8.1.2.tar.xz`（sha512 与 `vcpkg/ports/ffmpeg/
  portfile.cmake` 一致）即可继续；或先 export http_proxy/https_proxy
  （http://127.0.0.1:10808，**不要设 socks5 all_proxy**）。
- 构建后脚本自动给 `aetherkiri_sdl`/`libengine_api.dylib` 加 vcpkg_installed
  的 @rpath 并 ad-hoc 重签（install_name_tool 会废掉签名），无需
  DYLD_LIBRARY_PATH。engine_api 链接走 `LINK_LIBRARY_OVERRIDE ... WHOLE_ARCHIVE`
  （CMake 在 Apple 上映射为 -force_load），`LINK_GROUP:RESCAN` 仅 Linux 需要。
- 完整构建（Godot 生态）用 `./build.sh macos <debug|release>`。

## 运行 sdl_host

- Linux 必须带 vcpkg 运行库：
  `LD_LIBRARY_PATH=$PWD/out/linux/debug/vcpkg_installed/x64-linux/lib`；macOS
  无需（脚本已嵌入 @rpath）。
- 游戏路径传**绝对路径**（引擎 normalize 相对路径有 bug）。
- 常用参数（ui 壳）：`--fps 0`（不设帧率上限）、`--screenshot <path> --screenshot-frames <n>`
  （帧验证）、`--diagnostics [profile]`（结构化诊断 JSONL）、`--benchmark <sec>`
  （计时统计，GPU/软件对比）、`--slow-frame-threshold-ms <n>`（默认 20）、
  `--render-backend <software|gpu_bridge|sdl3_gpu>`（渲染后端；gpu_bridge 与
  sdl3_gpu 均为引擎内 SDL 纹理直显，software 为 CPU readback）、
  `--option key=value` + 便捷开关（--trace/--plugin-trace 等）。
- Linux 桌面**无系统字体回退**：引擎只找工作目录的 `NotoSansCJK-Regular.ttc`，
  开发时从 `/usr/share/fonts/opentype/noto/` 复制（勿提交仓库）。macOS 同理
  （本机无 PingFang.ttc，可用 `/System/Library/Fonts/Hiragino Sans GB.ttc` 替代）。
- 自检 demo：`--game demos/aetherkiri-test/data`（纯 TJS，测试渲染/音频/输入/层级）。

## 架构事实（文件名看不出）

- **边界**：`bridge/engine_api/include/engine_api.h` 是唯一宿主契约（C ABI）。
  引擎核心经 C ABI 暴露；宿主不直接碰 core。
- **渲染后端可插拔**：`TVPRegisterRenderManager` 工厂（software/godot_native/
  gpu_bridge/debug_cpu）。`cpp/core/visual/godot/GodotRenderManager.*` **不依赖
  Godot、也不依赖任何宿主回调表**——引擎内嵌 SDL3 渲染（krkrz 式）：纹理由引擎
  在宿主注入的 `SDL_Renderer` 上创建/直写（`TVPSetSdlRenderer`，
  `cpp/core/environ/sdl/sdl_render_backend.{h,cpp}`），宿主只负责 present。
- **SDL3 GPU 直显（已落地）**：宿主/壳 `engine_set_sdl_renderer` 注入 renderer →
  引擎创建 ABGR8888 流式纹理（blend NONE）→ `engine_get_gpu_frame_texture` 取
  句柄 → `SDL_RenderTexture` 直显——**无 readback**。blend/triangles/mosaic
  GPU 合成返回 false（引擎回退软件）。引擎 release 的纹理延迟到 present 后销毁
  （`engine_flush_released_textures`，宿主 present 后调）。注意：新 C ABI 函数
  必须同时接 **dispatch 层**（engine_api_dispatch.cpp Route 转发）——导出
  handle 是 dispatch 包装，直接调 legacy 层实现会收到无效 handle。
- **Linux engine_api 链接是手工维护的**（`bridge/engine_api/CMakeLists.txt`：
  显式 `--whole-archive` + 子插件显式链接）——CMake 的 LINK_LIBRARY_OVERRIDE +
  RESCAN 组合会重复包装 krkr2plugin（历史 bug）。**勿改回 override 方案**。
- **模块依赖环**：core 各模块 PRIVATE 互链成环（TVP 内核本质），链接靠
  `LINK_GROUP:RESCAN`。
- **TJS2**：`cpp/core/tjs2/` 完整 TJS2 实现（82 条 VM 指令、class/继承/正则/序列化）。
- **诊断/日志**：引擎运行期日志经 `engine_drain_runtime_logs` 暴露（勿直接读
  `TVPLogDeque`——无锁且宿主不应碰）。崩溃现场落盘到 `$AETHERKIRI_CRASH_DIR`。

## 踩坑（agent 极易miss）

- **SDL 像素格式端序**：`SDL_PIXELFORMAT_RGBA8888` 在小端机器内存布局是 ABGR；
  上传 [R,G,B,A] 数据必须用 `SDL_PIXELFORMAT_ABGR8888`（否则画面偏红/R=alpha）。
  另需 `SDL_RenderClear` 黑底 + `SDL_SetTextureBlendMode(NONE)`（SDL3 默认 BLEND）。
- **TJS2 语法限制**：**无对象字面量** `{}`（用 `new Dictionary()` + 属性赋值）、
  **无 `Object` 类**、函数值不能进对象字面量。`onKeyDown(key)` 的 key 是**数字**
  （TVP VK，如 'Z'=0x5A），字符串 `'z'` 永不匹配。语法错误消息（verbose+行号+caret）
  是近期修复的，写 TJS 脚本错误定位很快。
- **KiriKiri 层机制**：`win.add/remove` 只是窗口对象引用清单**不是层树**（层树由
  `Layer(parent/children/order)` 决定）；透明层默认 `HitThreshold=16`（alpha<16 不接收
  事件，全屏交互层需 `hitThreshold=0`）；`fillRect(0,0,w,h,0)` 是标准清空
  （`colorRect(...,0)` 是 no-op）；`ltOpaque` 层未填充区域显示白色（必须显式填充）。
- **pkill 自杀**：`pkill -f <模式>` 若模式匹配当前 shell 命令行会杀掉自己导致命令挂起
  ——先 `ps aux | grep` 确认 pid 再 kill。更稳的是 `ps -C <进程名> -o pid=`。
- **xdotool 注入键/点击无效**：SDL 过滤 XSendEvent 合成事件——交互验证交给用户手动。
- **窗口截图采样**：`xwd` 抓的是含边框的整个窗口（内容区偏移未知），
  引擎坐标→窗口像素映射不可靠，验证画面用 `--screenshot`（引擎侧 PPM）或问用户。
- **引擎终止无宿主通知**：脚本 `win.close()` → `TVPTerminateAsync` 后帧停止但 sdl_host
  不自动退出——已有帧停止检测（3 秒无新帧退出），改宿主循环时勿删。
- **fork 与 origin 的引擎核心差异可能是游戏兼容问题的根因**：同一游戏在
  AetherKiri/AetherKiri（origin）与 krkrsdl3 正常、而本 fork 异常时，先
  `git diff origin/main fork/main -- <模块>` 找缺失的 origin 修复，再逐个验证
  （如 `176ea404 fix: stabilize L3J title transitions`——千恋万花主菜单背景丢失的根因，
  本 fork 一直缺它；缺失的 yuzu 标题动画时钟逻辑导致动画结束帧把背景变黑）。
- **柚子社（Yuzusoft）游戏的 xp3 是加密变体**（头 8 字节 `XP3\r\n \x1a\x8b` 且
  索引 zlib 压缩），项目自带 `tools/xp3` 解不开；用
  `https://github.com/storycraft/xp3-tool`（Rust，`cargo build --release` 后
  `xp3-unpacker <xp3> <dir>`）解包读 TJS/PSB。`yuzuex.dll` 是 Windows PE，
  Linux 引擎无法加载（`LoadModule('yuzuex.dll'): not found in internal plugin map`），
  但 krkrsdl3 无它也能跑——不要把它当成画面问题的根因。
- **readback 帧内容只有局部区域有内容**（如 4096 采样点中只有几百可见）时，先用
  `StoreLatestCpuFrameFromTexture`（ui_stubs.cpp）采样 readback 源纹理的像素，
  区分"合成只写了局部"与"读取/同步错误"；再结合"合成 Blt 日志"（LayerManager::DrawCompleted）
  判断。层更新区域（UpdateRegion）为空不等于画面不该更新——静态背景层依赖
  内容变化触发 InvalidateRect，场景切换（draw buffer RESIZE/清空）后若未全屏重绘
  会黑屏（`EnsureDrawBufferSize` RESIZE 分支需 `AddUpdateRegion(全屏)`）。

## 调试设施速查

- 运行期日志/诊断事件/崩溃落盘/慢帧/benchmark 均由 sdl_host 提供（见 `--help`）。
- `doc/diagnostics.md`：Godot 生态的诊断会话体系（in-app drawer、diagnose.py 收集）。
- 崩溃现场：`~/.local/share/aetherkiri-sdl/crashes/`（stack.txt 符号化栈/log.txt/
  frame.ppm）。
- `tests/profiles/` + `apps/godot_app/scripts/*_probe.gd`：probe 体系（渲染/交互回归）。

## 文档与参考

- `doc/migration-sdl3.md`：SDL3 迁移计划/踩坑记录（最新权威）
- `doc/krkr2_plugins.md`：插件兼容清单
- `.agents/skills/`：项目级 skill（游戏兼容修复/跨仓库 PR/诊断归档分析）
- `DESIGN.md`：误放的无关文件（Anthropic 设计文档），可忽略
