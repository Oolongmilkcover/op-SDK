# op-SDK

把 [QtFFmpegPlayer](https://github.com/Oolongmilkcover/QtFFmpegPlayer) 的播放内核剥离出来，
做成**能被非 Qt 程序复用**的 SDK。

- **内核**：解封装、解码、帧队列、音画同步、时钟、倍速 —— 纯 C++17 + FFmpeg，**零 Qt**
- **消费者**：桌面的 Qt 播放器（上游仓库）、鸿蒙（HarmonyOS NEXT，课程大作业）、之后是安卓
- **当前**：内核可编译、单测通过；对外 C 接口只有设计稿，门面还没实现

> **给 AI 助手**：动手之前先读 [`AGENTS.md`](AGENTS.md)（硬约束、当前状态、第一阶段任务），
> 细节和验收标准在 [`docs/SDK-PLAN.md`](docs/SDK-PLAN.md)。

---

## 状态

| 项 | 状态 |
|---|---|
| `core/`（27 个文件，零 Qt） | ✅ 可编译 |
| `op_core` 静态库 | ✅ |
| `tests/test_framequeue`（FrameQueue 状态机，headless） | ✅ 通过 |
| `include/op/op_player.h`（C 接口） | ⚠️ 设计稿，**无实现** |
| C 门面 / console 示例 / 鸿蒙壳 | ❌ 未开始 |

---

## 构建

```bash
# Windows（MSVC 2022 + Ninja）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug ^
      -DFFMPEG_PATH="C:/Program Files/ffmpeg/ffmpeg8.1"
cmake --build build
build\tests\test_framequeue.exe

# Linux（Ubuntu 24.04）
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
    libavcodec-dev libavformat-dev libavdevice-dev libavutil-dev \
    libswresample-dev libswscale-dev libavfilter-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build && ctest --test-dir build --output-on-failure
```

依赖：FFmpeg 8.x（`avformat avcodec avdevice avutil swresample swscale avfilter`）、CMake ≥ 3.21。
**不需要 Qt。**

---

## 目录

```text
op-SDK/
├── AGENTS.md                 # 给 AI 助手的作业说明（先读这个）
├── include/op/op_player.h    # 唯一的对外 C 接口（设计稿）
├── core/                     # 内核：零 Qt
│   ├── playercore.*          #   入口：postMessage / 事件回调 / start-stop
│   ├── messagequeue.*        #   消息定义（CMD_* / EVT_* / CTX_*）
│   ├── messageloopthread.*   #   消息循环线程
│   ├── playercontext.h       #   状态广播（只给 UI，不能当状态源）
│   ├── demuxthread.*         #   解封装 + 异步 seek + 逐帧回填
│   ├── decodethread.*        #   解码基类（send/recv、编解码器生命周期）
│   ├── videodecodethread.*   #   视频解码 + 像素格式统一
│   ├── videorenderthread.*   #   节奏 / 同步 / 丢帧 / 逐帧步进
│   ├── audiothread.*         #   音频解码 + atempo 倍速 + 音频时钟
│   ├── framequeue.*          #   未来帧环 + 历史窗口（逐帧核心）
│   ├── packetqueue.*         #   有界包队列
│   ├── frameconverter.*      #   swscale → YUV420P（加锁，跨线程共用）
│   ├── iaudiooutput.h        #   音频出口接口（push；对外要包成 pull）
│   ├── log.h / perfclock.h   #   日志 / 纳秒时钟
├── tests/                    # FrameQueue 单测（CTest 注册）
├── cmake/FFmpeg.cmake        # ffmpeg_deps（Win: FFMPEG_PATH / Linux: pkg-config）
└── docs/SDK-PLAN.md          # 分阶段计划与验收标准
```

---

## 和上游仓库的关系

- 本仓库是内核的**唯一开发源**；上游 `QtFFmpegPlayer` 保留一份快照用于构建自己的 Qt 播放器。
- 内核的 bug 请**先在本仓库修**，再由用户回落到上游（上游的 `docs/CORE-FORK.md` 有回落流程）。
- 上游的 `docs/problems-and-solutions.md` 第 24~33 条是内核化那一轮踩过的坑，值得一读。
