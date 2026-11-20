# SDK 化路线图与验收标准

> 上游背景：`QtFFmpegPlayer` 的 `docs/problems-and-solutions.md`（尤其 24~33 条）与
> `docs/CORE-FORK.md`（分叉与回落约定）。这里只写"从这里往哪走"。

## 目标 / 非目标

**目标**
1. 一个稳定的 C 接口（`include/op/op_player.h`），能被鸿蒙 NAPI、安卓 JNI、纯 C 程序、C++ 程序调用。
2. 同一份内核既能驱动 Qt 播放器，也能驱动 CLI，也能驱动移动端壳。
3. 第一个真实消费者：鸿蒙（课程大作业）；第二个：安卓。

**非目标**
- 不做 GUI / 不做播放列表 UI（那是消费者的事）
- 不做硬解（阶段 4 再谈）
- 不追求跨平台 ABI（先保证源码级可用；`.so`/`.dll` 的稳定 ABI 是阶段 3 之后的事）

---

## 阶段 0 · 现状（已完成）

- `core/` 迁入且零 Qt；`op_core` 静态库可编；`tests/test_framequeue` 通过
- C 接口只有设计稿

---

## 阶段 1 · C 门面（3~5 天）

| 任务 | 说明 |
|---|---|
| T1 骨架 | `src/op_player.cpp`：pimpl `struct op_player { PlayerCore core; PlayerContext ctx; ... }`；实现 create/destroy/version/回调注册 |
| T2 事件 | `EVT_*` → `op_event` 字段映射（时长、宽高、seek 落点、状态、错误文本） |
| T3 视频帧 | `PaintFrame` → `op_video_cb` + `op_frame`（借用视图，回调内有效） |
| T4 音频 pull | 内部环形缓冲 sink（满足 `IAudioOutput`）+ 对外 `op_audio_read/pending/latency_ms` |
| T5 fd 打开 | `op_open_options.fd` 走自定义 AVIO / `fd:` 协议 |

**验收**
- `tests/test_api.cpp`：create → version → destroy；open 一个本地 mp4 → 收到 `OPENED` 与 ≥10 个 `POSITION` → destroy 不崩
- `examples/console`：能打印时长/宽高、把前 10 帧存 PPM、把 10 秒 PCM 存文件（`ffplay` 能放）
- `ctest` 全绿；Linux 上也能编

**风险**
- 门面如果直接把 `PlayerMessage` 抛给用户，就等于把内部协议变成 ABI —— 必须显式映射
- 回调线程语义要在头文件里写死，否则消费者第一件事就是死锁

---

## 阶段 2 · 鸿蒙第一个消费者（课程大作业，2~3 周）

**目标**：DevEco 工程里用 ArkTS 调 NAPI，画面走 `XComponent` + EGL，声音走 `AudioRenderer`。

| 任务 | 要点 |
|---|---|
| FFmpeg 交叉编译 | 用 DevEco 的 `ohos-clang` + sysroot；`--target-os=linux --arch=aarch64 --enable-cross-compile --disable-programs --disable-doc`；只要 6 个库，静态链接 |
| NAPI 胶水 | `napi_threadsafe_function` 把 SDK 线程的回调转发到 JS 线程（和 Qt 的 `QueuedConnection` 同一个模式） |
| 渲染 | `OH_NativeXComponent` → `OH_NativeWindow` → `eglCreateWindowSurface`；三平面 `GL_RED` 上传可以照搬上游 `VideoWidget::paintGL` |
| 音频 | `AudioRenderer` 的写回调里同步调 `op_audio_read`（这就是选 pull 的原因） |
| 文件 | picker 给 URI → `fileIo.openSync` 拿 fd → `op_open(fd)`（这就是要 fd 的原因） |
| UI | ArkTS 页面：打开 / 播放暂停 / 进度条 / 帧率显示 |

**验收**：真机上播一个 1080p mp4，有声音、能 seek；掉帧率、内存占用有记录。

**降级方案（保证大作业能交付）**：保留一个"平台基线模式"（ArkTS 的 `AVPlayer`），
演示时对比"平台播放器 vs 自研内核"，这本身就是最好的答辩材料。

---

## 阶段 3 · 工程化（1 周）

- `install` 规则 + `op_sdkConfig.cmake`（`find_package(op_sdk)` 可用）
- shared 库 + 导出符号（`OP_API`）+ 版本号
- CI（GitHub Actions）：Linux 构建 + `ctest`；Windows 可选
- 文档：`docs/API.md`（线程模型、所有权、错误码、生命周期图）

---

## 阶段 4 · 能力补强（后续）

- 硬解（D3D11VA / MediaCodec）
- 时间轴按 `AVStream::start_time` 归一化；VFR 用 `AVFrame::duration`
- 字幕 / HDR / 色彩管理（BT.601/709、limited/full range）
- 安卓壳（JNI，比 NAPI 简单，因为接口已经是 C）

---

## 接口设计原则（阶段 1 必须遵守）

1. **只有 C**：`extern "C"`、定长整数、`const char*`、不透明句柄 `op_player*`。
2. **所有权明确**：谁分配谁释放；借用（borrowed）的东西必须写"只在回调期间有效"。
3. **异步但不回调地狱**：`op_open` 立刻返回"已受理"，结果用事件通知。
4. **错误一律返回码**，不抛异常、不静默。
5. **线程契约写进头文件**：哪个线程回调、哪些接口可重入、`destroy` 会不会 join。
6. **扁平事件结构**：FFI 友好，加字段不破坏已有偏移。

---

## 验证清单（每个阶段收尾都跑一遍）

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DFFMPEG_PATH="<本机>"
cmake --build build
ctest --test-dir build --output-on-failure
```

外加：
- `grep -r "#include <Q" core/ include/` 必须为空
- `dumpbin /dependents build/op_core.lib`（或 `ldd`）里不应出现任何 Qt
- 头文件能被 **C 编译器** 和 **C++ 编译器** 各编一次（`cl /Zs` 或 `gcc -fsyntax-only`）
