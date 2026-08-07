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

## 运行 sdl_host

- 必须带 vcpkg 运行库：`LD_LIBRARY_PATH=$PWD/out/linux/debug/vcpkg_installed/x64-linux/lib`
- 游戏路径传**绝对路径**（引擎 normalize 相对路径有 bug）。
- 常用参数：`--fps 0`（不设帧率上限）、`--screenshot <path> --screenshot-frames <n>`
  （帧验证）、`--diagnostics [profile]`（结构化诊断 JSONL）、`--benchmark <sec>`
  （计时统计，阶段 2 GPU 对比基线）、`--slow-frame-threshold-ms <n>`（默认 20）、
  `--option key=value` + 便捷开关（--trace/--plugin-trace 等）。
- Linux 桌面**无系统字体回退**：引擎只找工作目录的 `NotoSansCJK-Regular.ttc`，
  开发时从 `/usr/share/fonts/opentype/noto/` 复制（勿提交仓库）。
- 自检 demo：`--game demos/aetherkiri-test/data`（纯 TJS，测试渲染/音频/输入/层级）。

## 架构事实（文件名看不出）

- **边界**：`bridge/engine_api/include/engine_api.h` 是唯一宿主契约（C ABI）。
  引擎核心经 C ABI 暴露；宿主不直接碰 core。
- **渲染后端可插拔**：`TVPRegisterRenderManager` 工厂（software/godot_native/
  gpu_bridge/debug_cpu）。`cpp/core/visual/godot/GodotRenderManager.*` **不依赖 Godot**
  （GPU 全走 `TVPGodotGpuBridgeCallbacks` 回调表 + uint64 句柄）——SDL3 GPU 后端
  只需实现该回调表，RenderManager 可复用。
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
  ——先 `ps aux | grep` 确认 pid 再 kill。
- **xdotool 注入键/点击无效**：SDL 过滤 XSendEvent 合成事件——交互验证交给用户手动。
- **窗口截图采样**：`xwd` 抓的是含边框的整个窗口（内容区偏移未知），
  引擎坐标→窗口像素映射不可靠，验证画面用 `--screenshot`（引擎侧 PPM）或问用户。
- **引擎终止无宿主通知**：脚本 `win.close()` → `TVPTerminateAsync` 后帧停止但 sdl_host
  不自动退出——已有帧停止检测（3 秒无新帧退出），改宿主循环时勿删。

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
