# AGENTS.md —— 在这个仓库里协助开发前，请先读完这一页

> 这份文件是写给**下一个接手这个仓库的 AI 助手**的。用户会带着它开新对话。
> 读完之后你应该知道：这是什么、现在到哪一步、下一步做什么、什么绝对不能做。

---

## 0. 一句话

把这个仓库里的播放内核（解封装 / 解码 / 队列 / 音画同步 / 时钟）做成一个**能被非 Qt 程序复用**的 SDK，
第一个真实消费者是**鸿蒙（HarmonyOS NEXT）**，第二个是**安卓**。

---

## 1. 仓库定位

| | |
|---|---|
| 本仓库 | `op-SDK` —— 内核 + 对外接口 + 单测 + 文档。**只放与 Qt 无关的代码** |
| 上游 | [`QtFFmpegPlayer`](https://github.com/Oolongmilkcover/QtFFmpegPlayer) —— Qt 播放器 + 推流器，是**参考消费者** |
| 关系 | 内核从上游迁出，本仓库是内核的**唯一开发源**；上游保留一份快照用于构建，**不要去改上游** |

`core/` 里的代码来自上游的同名目录，已经过"去 Qt"改造：`QThread→std::thread`、
`qDebug→core/log.h`、`QString→std::string`、信号槽 → `MessageQueue` + `PlayerCore` 消息驱动。
它现在**能编译、能跑单测**，不是半成品骨架。

---

## 2. 当前状态（可验证，别假设更多）

| 项 | 状态 |
|---|---|
| `core/` 零 Qt | ✅ 构建参数里没有任何 Qt 路径，`AUTOMOC/AUTUIC/AUTORCC` 全关 |
| `op_core` 静态库 | ✅ 能编（`build/op_core.lib`） |
| `tests/test_framequeue` | ✅ `ALL TESTS PASSED`（FrameQueue 状态机，headless） |
| 对外 C 接口 | ⚠️ **只有设计稿** `include/op/op_player.h`，**没有任何实现** |
| 门面（pimpl） | ❌ 不存在 |
| console 示例 | ❌ 不存在 |
| 鸿蒙壳 | ❌ 不存在（DevEco 工程都还没建） |

也就是说：**内核是能用的，但"SDK"只完成了一半 —— 只有壳没做**。

---

## 3. 用户的路线图（按这个顺序，别跳）

1. **把 `include/op/op_player.h` 落地**：写 C 门面（pimpl 包住 `PlayerCore` + `PlayerContext`），
   让"非 Qt 的 C/C++ 程序"能打开文件、收到事件、拿到视频帧和 PCM。
2. **音频从 push 改成 pull**：`op_audio_read(buf, cap)`。桌面的 `IAudioOutput` 是 push 模型
   （core 写、设备给 `getFree`），而鸿蒙 `AudioRenderer`、安卓 `AudioTrack` 都是"设备来要数据"，
   所以 C 接口必须是**被拉**的。内部包一层环形缓冲即可，**不用重写 `AudioThread`**。
3. **`op_open` 支持 fd**：鸿蒙 picker 给 URI、安卓给 `content://`，NDK 侧基本只能拿到文件描述符。
4. **一个无 Qt 的 console 示例**（打开文件 → 打印每帧 pts → 音频写 pcm 文件）。
   这个示例同时是文档，也是后面鸿蒙调试的对照组。
5. **鸿蒙壳**：ArkTS + NAPI + XComponent/EGL + AudioRenderer（这是用户的课程大作业）。
6. 之后再谈安卓。

---

## 4. 硬约束（违反就别提交）

1. **`core/` 里不许出现 Qt**：没有 `#include <Q...>`、没有 `QObject`/`QThread`/信号槽。CMake 里 `op_core` 不许链 Qt。
2. **跨 C 边界不许出现 C++ 类型**：`std::function` / `std::string` / 引用参数 / 异常，一个都不行。
   错误用返回值（`op_result`），字符串用 `const char*` 且只在回调期间有效。
3. **`AVFrame*` 不许穿过 C 接口**：视频外传 `op_frame`（借用视图，回调返回即失效）。
4. **`op_destroy` 必须 join 所有线程**，返回后不再有任何回调；
   **回调里禁止调用 `op_destroy` / `op_stop`**（自己等自己 = 死锁），这条要写进头文件注释。
5. **回调在 SDK 自己的线程上触发**，不保证是调用方的 UI 线程 —— 调用方要自己排队，文档必须写清。
6. **不要为了"看起来像 SDK"加抽象**。每加一层都要能说清它解决了什么问题。

---

## 5. 已经验证过、别推翻的设计决定

- **`PlayerContext` 只做单向广播，不能当状态源**。它比真实状态慢一个消息往返；
  core 内部的判断一律读各自的本地成员（`DemuxThread::m_isPause`、`Player::m_seekTargetMs` 之类）。
  上游因此吃到过五种不同的 bug（见第 6 节）。
- **一次 seek 请求 → 恰好一条 `SEEK_DONE`**，早退路径也要补发。
- **解码器的 send/recv 必须整段串行**（上游用 `m_decodeGate` 包住一整轮）。
- **像素格式在入口统一**：`FrameConverter` 按每帧真实 `frame->format` 做 `ensure`，
  `SwsContext` 加锁（解码线程和 demux 线程共用）。
- **`op_event` 用扁平结构**（不用 union），FFI 好读、加字段不破坏偏移。

---

## 6. 已知的坑（都真踩过，别重犯）

| 坑 | 表现 | 正确做法 |
|---|---|---|
| 异步广播当状态源 | 逐帧退出时无限递归、停止后黑屏、快进后要按两次播放、倍速标签跳格 | 权威状态放本地，`PlayerContext` 只给 UI 看 |
| 一个标志管两件事 | `isExit` 同时表示"整体退出"和"这次打开别继续" → 停止后打开黑屏 | 拆成两个标志 |
| 锁保护"调用"而不是"状态机" | seek 落点偶发偏一帧、回填画面串 | 用闸门包住 send+recv 整轮 |
| 只统一了部分路径的像素格式 | 逐帧回填时花屏（回填路径漏了转换） | 每条产出帧的路径都要过转换器 |
| 周期性回调写非周期状态 | 150ms 的 tick 把用户刚点的值覆盖回去 | 周期性回调只写周期数据 |
| Windows 上 `sleep_for(1ms)` | 实际睡 15.6ms，demux 吞吐掉到 ~64 包/秒 → 卡顿 | 需要 1ms 精度就 `timeBeginPeriod(1)`；但 core 里应优先改成"不依赖 1ms" |

完整版（33 条）在上游仓库 `docs/problems-and-solutions.md`，第 24~33 条就是内核化那一轮。

---

## 7. 工作方式（用户偏好，请遵守）

- **注释写"为什么"，不写"是什么"**；用中文。用户自己的代码就是这个风格。
- **每一步都要能验证**：改完必须 `cmake --build` + `ctest` 通过；不要留下"理论上能跑"的改动。
- **一个主题一次提交**，message 用 `feat/fix/refactor/docs(scope): 中文摘要`。
- **不要静默失败**：错误要返回码 + 日志，不要吞。
- **大改动先给方案再动手**：尤其涉及接口形状（C 头）和线程模型时。
- 用户会同时在看代码，别做与当前任务无关的大范围格式化/重命名。

---

## 8. 第一阶段任务清单（照做即可）

### T1 · C 门面骨架（先跑通"能创建/销毁/读状态"）
- 新增 `src/op_player.cpp`（pimpl：`struct op_player { core::PlayerCore core; PlayerContext ctx; ... }`）
- 实现 `op_create / op_destroy / op_version / op_set_event_cb / op_set_video_cb`
- CMake 加 `add_library(op_sdk SHARED ...)` 或把门面加进 `op_core`（二选一，说明理由）
- **验收**：一个 `tests/test_api.cpp` 能 `op_create` → 查版本 → `op_destroy`，不崩、无泄漏（能跑就行）

### T2 · 事件映射
- 把 core 的 `EVT_*` 映射成 `op_event`（`OPENED/POSITION/STATE/SEEK_DONE/COMPLETED/ERROR`）
- `arg1/arg2` 里的打包值（时长、`w<<16|h`、seek 高低 32 位）拆成结构体字段
- **验收**：console 示例能打印出 `OPENED` 的时长/宽高

### T3 · 视频帧
- 把 `PaintFrame` 回调转成 `op_video_cb`，填 `op_frame`（`data/linesize/width/height/pts_ms`）
- **验收**：console 示例把前 10 帧存成 PPM（能肉眼看）

### T4 · 音频 pull
- 实现一个内部环形缓冲 sink（满足 `IAudioOutput`），对外暴露 `op_audio_read / op_audio_pending / op_audio_latency_ms`
- **验收**：console 示例把 10 秒 PCM 写进 `.pcm`，`ffplay -f s16le -ar 48000 -ac 2` 能放

### T5 · fd 打开
- `op_open_options.fd` 生效：内部用 `avformat_open_input` + 自定义 AVIO（或 `pipe:`/`fd:` 协议）
- **验收**：把 mp4 用 `open()` 拿到 fd 再喂给 SDK，能正常播

> 顺序建议：T1 → T2 → T3 → T4 → T5。每完成一个就和用户确认一次，
> 因为接口形状一旦定下来，鸿蒙侧的胶水就得跟着改。

---

## 9. 文件导航

```text
include/op/op_player.h   ← 唯一的对外接口（目前是设计稿，T1 之后变成真实现）
core/                    ← 内核：27 个文件，零 Qt
  playercore.h          ←   C++ 入口（postMessage / 事件回调 / start-stop）
  messagequeue.h        ←   消息定义（CMD_* / EVT_* / CTX_*）
  playercontext.h       ←   状态广播（只给 UI 看，不能当状态源）
  demuxthread.h         ←   解封装 + seek + 逐帧回填
  audiothread.h         ←   音频解码 + atempo 倍速 + 音频时钟
  videodecodethread.h   ←   视频解码 + 像素格式统一
  videorenderthread.h   ←   节奏 / 同步 / 丢帧
  framequeue.h          ←   未来环 + 历史窗口（逐帧核心）
  iaudiooutput.h        ←   音频出口接口（push）→ T4 要包成 pull
  frameconverter.h      ←   swscale 转 YUV420P（加锁）
tests/                   ← 只有 FrameQueue 单测，T1 之后要加 api 测
docs/SDK-PLAN.md         ← 分阶段计划与验收标准（细节在这里）
```

---

## 10. 怎么验证（每次改完都跑）

```bash
# Windows（MSVC + Ninja，FFMPEG_PATH 按本机改）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFFMPEG_PATH="C:/Program Files/ffmpeg/ffmpeg8.1"
cmake --build build
./build/tests/test_framequeue

# Linux（Ubuntu 24.04：装 qt6 不需要，只需要 FFmpeg dev 包 + cmake + ninja）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build && ctest --test-dir build --output-on-failure
```

**如果 `build/` 是从别处搬来的**（目录改过名），`CMakeCache.txt` 里还写着旧路径，
直接删掉 `build/` 重配，别硬跑。

---

## 11. 一句话总结给用户的 AI

> 这个仓库的内核已经能用了，缺的是"外壳"：C 门面、事件映射、pull 音频、fd 打开、console 示例。
> 按 `docs/SDK-PLAN.md` 的阶段走，每步都要能编、能测、能演示。
