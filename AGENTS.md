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
| `op_core` 静态库 | ✅ 能编 |
| `op_sdk` 静态库（门面 + 环形 sink + fd AVIO） | ✅ MSVC 19.40 与 MinGW 13.1 都编过、都跑过 ctest |
| `tests/test_framequeue` | ✅ `ALL TESTS PASSED`（FrameQueue 状态机，headless） |
| `tests/test_api`（T1/T2 + T4/T5 端到端） | ✅ 含"打不开的文件 → ERROR → 回 IDLE"、"合成 WAV → fd + offset → OPENED → 拉到 PCM"；断言回调不在调用方线程 |
| `tests/test_audioring`（T4） | ✅ 8 组（绕回/对齐/字节→毫秒/外推夹紧/暂停冻结/过期上报/SPSC 1MB） |
| `tests/test_fdavio`（T5） | ✅ 7 组（offset/size/EOF/越界/幂等释放/不关调用方 fd） |
| 对外 C 接口 | ✅ T1/T2/T4/T5 已实现（open/play/pause/stop/seek/step/speed + 事件 + 查询 + 日志 + 音频 pull + fd） |
| 视频帧（T3） | ⚠️ 链路已通（真文件验过 320×240 / 854×480 / H.264），但**没有 console 示例落仓库** |
| 音频 pull（T4） | ✅ 已接线（ring + sink + 门面 + 采样率贯通）；**正式验收还缺真实媒体** |
| fd 打开（T5） | ✅ 已实现（`core/fdavio.*` 自定义 AVIO，支持 offset/size，不可 seek 降级） |
| console 示例 / 鸿蒙壳 | ❌ 不存在 |

**已知未解决（下一个助手请优先看这条）**：**暂停状态下 `op_seek_ms` 不重绘、落点也不生效**
—— 实测 seek 到 1500ms 后 `frames=0`、恢复播放却从 ~120ms 继续，且一次 seek 收到**两条**
`SEEK_DONE`。`op_seek_ms` 只投了一条 `CMD_SEEK`、拆包也对，嫌疑在 core 的
`doSeek()` / `repaintPts()`（含早退分支也会补发 `SEEK_DONE`）。拖动进度条是这个 SDK 最核心的
交互，这条不通，桌面壳和鸿蒙壳都会踩。

**另一条待验收**：T4 的**时钟正确性**还没用真实媒体验过（10 秒 PCM 用 `ffplay` 播、
带音轨视频 3 秒 ≈ 60 帧）。现有单测只能证明"接线通、能拉到 PCM"，证明不了"音画同步准"。

也就是说：**内核 + C 门面（T1/T2/T4/T5）都能用了；T3 差验收物，T4 差真实媒体验收，
seek 有个真 bug**。

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

### T1 · C 门面骨架 ✅ 已完成
- `src/op_player.cpp`：pimpl 包住 `PlayerCore` + `PlayerContext`
- 已实现：`op_create / destroy / version / 三个回调注册 / open / play / pause / stop / seek / step / speed`
- 门面是独立的 `op_sdk`（STATIC）target，不塞进 `op_core` —— 理由写在 `CMakeLists.txt` 里
- **验收**：`tests/test_api.cpp`（已通过）

### T2 · 事件映射 ✅ 已完成
- `EVT_* → op_event`，`arg1/arg2` 拆包（时长、`w<<16|h`、seek 高低 32 位）
- 状态机由门面合成（`OPENING/PLAYING/PAUSED/ENDED/IDLE`）。`STATE` 事件走
  "门面投私有消息 id → `PlayerCore` 的 `default` 分支转发"，这样回调仍在消息循环线程上
- 最近已知状态缓存进 atomic；查询接口读缓存，**不读** `PlayerContext`（它慢一个消息往返）
- `EVT_COMPLETED` 在 core 播完的两个分支里补发（以前只在枚举里、从没人发）

### T3 · 视频帧 ⚠️ 链路已通，缺验收物
- `PaintFrame → op_video_cb`；`op_frame` 是**紧凑连续缓冲**（`data/size/linesize[4]`），
  `linesize` 用 `av_image_fill_linesizes()` 按紧凑布局重算 —— **不能抄源 AVFrame 的对齐值**
  （宽 854 时源是 864，抄过去消费方会逐行读越界）
- 回调返回后 `av_frame_free`（core 的 painter 是把所有权交出来的）
- 已验：320×240 MPEG4、854×480（奇数宽）、640×360 H.264；`step ±1` 出帧正常
- **缺**：`examples/console` —— 原来"存 10 帧 PPM"的验收物没有落进仓库

### T4 · 音频 pull（B 方案）✅ 已接线，待真实媒体验收

> 目标：把桌面的 push 出口（`IAudioOutput`，设备给 `getFree`）包成 C 接口的 pull
> （`op_audio_read`，壳来拉）。**不是**"壳自己解码"：解码一直在 core 里，
> 壳只负责把 PCM 塞进设备。

#### 数据流与时钟（先把账算清）

```text
core 音频解码线程 ──IAudioOutput::write()──> [AudioRing] ──sink.read()──> 壳（QAudioSink / AudioRenderer / AudioTrack）
                                                  ↑                              │
                                           setDevicePendingMs(ms) ←──────────────┘
```

core 的时钟（`AudioThread::getPts()`）：`clock = 最后写入帧的 pts − getNoPlayMs() × speed`

"还没播"的数据同时躺在两处：**ring 里未读走的**（SDK 知道）+ **设备里已读走未播的**（只有壳知道）。
所以 B 方案的全部内容就是：**壳回传设备积压，SDK 把两处加起来**。

- `getNoPlayMs()` 返回**设备时间（毫秒）**；core 会乘 `speed` 换成内容时间，ring 侧不要乘
- `bytesPerMs = sr × ch × 2 / 1000`（48k/2ch = 192）；反向换算用 `bytes × 1000 / (sr × ch × 2)`

#### 已完成（Step 1~5）

- `src/audioring.h/.cpp`：`AudioRing`（SPSC 字节环，2 的幂容量、`&mask` 取模、
  读写持锁但 `available/space` 只读原子）+ `AudioRingSink : IAudioOutput`
- `tests/test_audioring.cpp`：8 组（绕回 / 帧对齐 / 满时写 0 / 字节→毫秒 / 外推夹紧 /
  暂停冻结 / 过期上报 / **SPSC 1MB 并发**）
- **Step 3 采样率贯通**：`CMD_OPEN_OBJ` 的 `outSampleRate/outChannels` →
  `DemuxThread::openFile(..., sr, ch)` → `AudioThread::open(stream, out, sr, ch)`，
  在 `resampleInit()` 和 `m_out->open()` **之前**赋值；`AudioThread::open` 拒绝 `<=0`。
- **Step 4 门面接线**：`op_player` 持有 `AudioRingSink`（声明在 `PlayerCore` **之前**，
  保证比 core 活得久）；`op_open` 把它当 `CMD_OPEN_OBJ::out`；
  `op_audio_read/pending/latency_ms` 分别转 `read()/availableBytes()/getNoPlayMs()`；
  新增 `op_audio_set_device_pending_ms` 给壳回传设备积压。
- **Step 5 头文件**：新函数已宣布，音频段改成"已实现"，写明"没数据返回 0"和
  "设备积压怎么算"。

**踩到的真 bug**：`op_player::outSampleRate/outChannels` 初值是 0（头文件承诺 48000/2），
于是"没调过 `op_set_audio_format` 的播放器"会被 `AudioThread::open` 拒绝、静默降级成
静音（`OPENED` 里 `has_audio=0`，看起来像文件没音轨）。是 T5 的 fd 端到端测试抓出来的。

#### 还剩：Step 6 验收

见下面的"验收"。headless ctest 只能证明"接线通、能拉到 PCM"，证明不了"时钟准"。

#### 陷阱（推演过 / 踩过，别再犯）

| 陷阱 | 后果 | 正确做法 |
|---|---|---|
| `getNoPlayMs()` 只算 ring、不算设备积压 | 时钟超前 → 视频判"落后音频" → 疯狂丢帧（实测 3 秒只渲染 6 帧） | 加上壳回传的设备积压 |
| 把上报值当实时值 | 时钟阶梯状，A/V 一顿一顿 | 锚点 + 按真实时间**线性外推** |
| 外推不夹紧 | 时钟倒退或穿到未来 | `≥0`；暂停时停止外推；`≤ 距上次 clear 的时间` |
| 用"距上次 write 的时间"当上界 | 把合法积压压没（100ms 的设备缓冲可以比它久） | **别用这条** |
| `setPause` 先切标志再固化 | 暂停瞬间时钟倒退，恢复时跳 | 先算当前外推值固化，再切标志并重锚 |
| seek 后不重锚 | 晚到的旧上报把时钟拽回旧位置 | `clear()` 同时清 ring、清积压、记 `lastClear` |
| `getNoPlayMs()` 里加锁 / 调设备 API | 被 `getPts()` 高频调用，拖慢 demux | 只读原子量 |
| `write()` 阻塞等空间 | 卡死 core 的音频解码线程 | 满了返回 0，core 自己有 1ms 轮询 |
| 环容量非 2 的幂 / 用 `%` | 高频除法开销 | `& (cap-1)` |
| `read()` 用 -1 表示没数据 | 门面把 `<0` 当 `op_result`，-1 = `OP_ERR_INVALID_ARG` | 没数据返回 **0** |

#### 验收

1. 10 秒 PCM 落盘 → `ffplay -f s16le -ar 48000 -ac 2` 能放（原始验收）
2. **带音轨的视频**渲染帧数 ≈ 时长 × fps（现在是 6 帧/3s，目标 ~60 帧/3s）—— 这条才证明时钟对了
3. `op_audio_latency_ms()` 稳定在几十 ms、不单调增长
4. 长跑 5 分钟：`op_audio_pending()` 不打满、RSS 不涨

**平台侧怎么写**（决定 B 准不准）：Qt 在 `QIODevice::readData(data,maxlen)` 里调
`op_audio_read`，用 `bufferSize() − bytesFree()` 上报；鸿蒙用 `getAudioTime()` /
`getWrittenDataSize()`；安卓用 `已写字节 − getPlaybackHeadPosition()×frameSize`。
Qt 侧注意：`readData` 在音频线程、`op_seek_ms` 在 GUI 线程 —— 过期上报就出在这里，
靠 `clear()` 的夹紧兜住。

### T5 · fd 打开 ✅ 已完成

- `core/fdavio.h/.cpp`：用 fd 建自定义 `AVIOContext`（读 / seek / `AVSEEK_SIZE`），
  把 `fd_offset/fd_size` 映射成一个"虚拟文件"；不可 seek 时 `size=-1` + `ESPIPE` 降级。
- `DemuxThread::openFile` 增加 `const core::FdInput*` 参数：挂 `pb` +
  `AVFMT_FLAG_CUSTOM_IO`（少了它 close 时会双重释放）；5 条失败路径 + `close()` +
  析构统一走 `releaseFdAvio()`。
- `op_open`：`fd >= 0` 优先，`path` 降级为可选的扩展名提示；顺带修掉 fd-only 时
  `std::string(nullptr)` 的崩溃。
- **验收**：`tests/test_fdavio`（7 组，直接用 `avio_read/seek/size` 验证 offset/size/
  EOF/越界/幂等释放）+ `test_api` 第 9 节（合成 WAV + 100 字节前缀 → fd + offset →
  `OPENED` → 确实能拉到 PCM）。
- **设计细节和 11 条坑**：`docs/NOTES.md` 的 T5 篇；AVIO 入门看 `docs/AVIO.md`。

> 顺序建议：T1 → T2 → T3 → T4 → T5。每完成一个就和用户确认一次，
> 因为接口形状一旦定下来，鸿蒙侧的胶水就得跟着改。

---

## 9. 文件导航

```text
include/op/op_player.h   ← 唯一的对外接口（T1/T2/T4/T5 已实现）
src/                     ← C 门面：只有这一层出现 C++ 类型，调用方看不见
  op_player.cpp         ←   pimpl：PlayerCore + PlayerContext + 事件映射 + 状态缓存（+日志广播）
  audioring.h/.cpp      ←   T4：SPSC 字节环 + AudioRingSink（IAudioOutput 的实现）
core/                    ← 内核：零 Qt
  playercore.h          ←   C++ 入口（postMessage / 事件回调 / start-stop）
  messagequeue.h        ←   消息定义（CMD_* / EVT_* / CTX_*）
  playercontext.h       ←   状态广播（只给 UI 看，不能当状态源）
  demuxthread.h         ←   解封装 + seek + 逐帧回填
  fdavio.h/.cpp         ←   T5：fd → 自定义 AVIOContext（虚拟文件 / 不可 seek 降级）
  audiothread.h         ←   音频解码 + atempo 倍速 + 音频时钟（采样率由 open 参数传入）
  videodecodethread.h   ←   视频解码 + 像素格式统一
  videorenderthread.h   ←   节奏 / 同步 / 丢帧
  framequeue.h          ←   未来环 + 历史窗口（逐帧核心）
  iaudiooutput.h        ←   音频出口接口（push）→ T4 的 ring sink 实现它
  frameconverter.h      ←   swscale 转 YUV420P（加锁）
tests/                   ← test_framequeue / test_api / test_audioring / test_fdavio，全部进 ctest、headless
docs/SDK-PLAN.md         ← 分阶段计划与验收标准
docs/NOTES.md            ← 开发笔记：为什么这么设计、踩过的坑（AudioRing / 自定义 AVIO）
docs/AVIO.md             ← AVIO 学习笔记：概念、最小示例、缓冲行为、排查表
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

**改过头文件之后必须 `--clean-first`**：这台机器（中文 VS + ninja）的 `/showIncludes`
前缀是 GBK，CMake 写进 `rules.ninja` 的 `msvc_deps_prefix` 编码对不上，ninja 记不到
头文件依赖（`ninja -t deps` 显示 `#deps 0`）。所以动了 `core/*.h` 或 `include/op/*.h`
之后，用 `cmake --build build --clean-first`，否则会拿到"编过了但其实是旧代码"的
二进制 —— 失败现象看起来像业务 bug（踩过一次：枚举值改了而门面没重编，事件全错位）。

---

## 11. 一句话总结给用户的 AI

> 内核 + C 门面已经能用：T1/T2/T4/T5 全部实现（open/play/pause/stop/seek/step/speed、
> 事件、日志、视频帧、音频 pull、fd 打开），MSVC 与 MinGW 双工具链 ctest 4/4。
> 下一步：先修第 2 节那个"暂停中 seek 不重绘"的真 bug，再做 `examples/console`
> 补齐 T3 验收物 + T4 的真实媒体验收，然后进鸿蒙壳。每步都要能编、能测、能演示。
