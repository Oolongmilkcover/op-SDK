# op-SDK

**一个零 Qt、C 接口、可跨平台的播放内核 SDK。**

把 [QtFFmpegPlayer](https://github.com/Oolongmilkcover/QtFFmpegPlayer) 的播放内核（解封装 / 解码 /
帧队列 / 音画同步 / 时钟 / 倍速 / seek / 逐帧）剥出来，做成**任何语言、任何 UI 框架都能调**的库：

- **内核**：C++17 + FFmpeg，`core/` 里没有一行 Qt；
- **接口**：纯 C ABI（[`include/op/op_player.h`](include/op/op_player.h)），能过 NAPI / JNI / ctypes；
- **消费者**：桌面 Qt 播放器（[`examples/qtplayer`](examples/qtplayer)）、鸿蒙（HarmonyOS NEXT）、安卓；
- **发行形态**：静态库（默认）或动态库（`-DOP_BUILD_SHARED=ON`），Windows / Linux / 鸿蒙 / 安卓四平台产物。

> 第一次接手这个仓库的 AI 助手：先读 [`AGENTS.md`](AGENTS.md)（硬约束、已知的坑、任务清单），
> 计划与验收标准在 [`docs/SDK-PLAN.md`](docs/SDK-PLAN.md)。

---

## 特性

| 能力 | 说明 |
|---|---|
| 解封装 / 解码 | FFmpeg 8.x，视频统一转 YUV420P（紧凑缓冲），音频统一 S16 交错 |
| 消息驱动 | 所有控制异步投递，事件在 SDK 自己的消息循环线程回调 |
| 音画同步 | 音频为主时钟；视频按墙钟节奏渲染，落后丢帧、超前等待 |
| seek | 异步；**一次请求恰好一条 `SEEK_DONE`**（早退路径也补发） |
| 逐帧 | `op_step_frame(±1)`，`-1` 靠历史队列回填（移动档关闭，见"平台档位"） |
| 倍速 | 0.5x ~ 2.0x（`speed_x10`），音频走 atempo |
| 视频帧 | `op_frame` 借用视图 + `op_frame_clone` 跨线程带走 |
| 音频 | **拉模式**：`op_audio_read()`，适配 QAudioSink / AudioRenderer / AudioTrack |
| 打开方式 | 路径 / URL，或 **fd + offset + size**（鸿蒙 picker、安卓 content://） |
| 日志 | `op_set_log_cb`，core 的日志进程级广播到壳 |
| 平台档位 | 编译期选择桌面/移动参数，运行时用 `op_platform()` / `op_is_mobile()` 查询 |

---

## 快速开始

依赖：**FFmpeg 8.x**（`avformat avcodec avdevice avutil swresample swscale avfilter`）、CMake ≥ 3.21、C++17。
**不需要 Qt。**

```bash
# Windows（MSVC 2022 + Ninja）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug ^
      -DFFMPEG_PATH="C:/Program Files/ffmpeg/ffmpeg8.1"
cmake --build build
ctest --test-dir build --output-on-failure

# Linux（Ubuntu 24.04）
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
    libavcodec-dev libavformat-dev libavdevice-dev libavutil-dev \
    libswresample-dev libswscale-dev libavfilter-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build && ctest --test-dir build --output-on-failure
```

`ctest` 默认 5 个用例（framequeue / api / audioring / fdavio / seek）。其中 seek 回归需要真实媒体：

```powershell
$env:OP_SDK_TEST_MEDIA="D:\media\test.mp4"   # 或 .mp3
build\tests\test_seek.exe
```

> 这台机器（中文 VS + Ninja）**改过头文件后必须 `cmake --build build --clean-first`**：
> `/showIncludes` 前缀是 GBK，ninja 记不到头文件依赖（`ninja -t deps` 显示 `#deps 0`），
> 否则会拿到"编过了但其实是旧代码"的二进制。详见 [`AGENTS.md`](AGENTS.md)。

---

## 一分钟上手

```c
#include <op/op_player.h>
#include <stdio.h>
#include <string.h>

static void on_event(void *user, const op_event *ev)
{
    switch (ev->type) {
    case OP_EVENT_OPENED:
        printf("时长 %lld ms, 视频=%d 音频=%d\n",
               (long long)op_duration_ms((op_player *)user),
               (int)op_has_video((op_player *)user),
               (int)op_has_audio((op_player *)user));
        break;
    case OP_EVENT_POSITION:      /* 约 150ms 一次，给进度条用 */ break;
    case OP_EVENT_SEEK_DONE:     /* ev->position_ms = 实际落点 */ break;
    case OP_EVENT_COMPLETED:     /* 播到结尾；内核会自动回 0 重播 */ break;
    case OP_EVENT_ERROR:         /* ev->code + ev->message（只在回调期间有效） */ break;
    }
}

int main(void)
{
    op_player *p = op_create();
    op_set_event_cb(p, on_event, p);
    op_set_audio_format(p, 48000, 2);          /* 声明壳能播的 PCM 格式 */

    op_open_options opt;
    memset(&opt, 0, sizeof(opt));
    opt.path = "D:/media/test.mp4";
    opt.fd   = -1;
    op_open(p, &opt);                          /* 异步：结果走 OPENED / ERROR */

    op_play(p);

    /* 音频拉模式：壳在自己的音频线程里循环拉 PCM（这里只是示意） */
    unsigned char pcm[4096];
    for (int i = 0; i < 100; ++i) {
        int n = op_audio_read(p, pcm, sizeof(pcm));
        if (n > 0) { /* 写进声卡 */ }
        /* n == 0 表示"暂时没数据"，别忙转 */
    }

    op_destroy(p);                             /* 会 join 所有线程，返回后不再有回调 */
    return 0;
}
```

完整调用说明（线程模型、帧所有权、音频积压怎么报、FAQ）见 **[docs/API.md](docs/API.md)**。

---

## 平台档位（桌面 / 移动）

参数在**编译期**按目标平台选定（单一真源 [`core/platform.h`](core/platform.h)），运行时用
`op_platform()` / `op_is_mobile()` 查询 —— 壳不要自己写 `#ifdef`，否则"按钮亮着但点了返回
`OP_ERR_UNSUPPORTED`"这种问题会很难解释。

| 档位 | 视频帧队列 | 历史队列 | 逐帧 | 视频包队列 |
|---|---|---|---|---|
| Windows / Linux 桌面 | 16 帧 | 48 帧 | 可用 | 100 包 |
| Android / HarmonyOS | 8 帧 | **关闭** | **关闭**（`OP_ERR_UNSUPPORTED`） | 50 包 |

- 历史队列是大头：1080p 每帧 clone 一整帧 ≈ 3.1MB，48 帧 ≈ **150MB**，正常播放约 2 秒就常驻满，
  手机上就是"被系统 kill"级别的开销，所以移动档整条关掉；
- 帧队列 8 帧 @24fps 仍有 ≈333ms 预解码余量，不影响流畅度；
- 判定链顺序是 `__ANDROID__ → __OHOS__ → _WIN32 → __linux__`：鸿蒙/安卓都同时定义 `__linux__`，
  顺序写反会**静默**把移动端当桌面 Linux；
- `op_create()` 会打一行 `[op_sdk] platform=... frameQueue=... history=... step=...`，真机先看这行。

---

## 打包 / 集成到别的工程

### 构建动态库发行包

```bash
cmake -S . -B build-shared -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DOP_BUILD_SHARED=ON -DFFMPEG_PATH="C:/Program Files/ffmpeg/ffmpeg8.1"
cmake --build build-shared && ctest --test-dir build-shared --output-on-failure
cmake --install build-shared --prefix dist
```

`dist/` 布局：

```text
dist/
├─ include/op/op_player.h          # 唯一公开头文件
├─ bin/op_sdk.dll (+ av*.dll)      # Windows 运行时
├─ lib/op_sdk.lib                  # 导入库
└─ lib/cmake/op_sdk/*              # find_package 配置
```

### 调用方只需要两行

```cmake
find_package(op_sdk CONFIG REQUIRED)
target_link_libraries(app PRIVATE op::op_sdk)
```

`op::op_sdk` 自带头文件路径、库、以及 Windows 的 `OP_USE_SHARED`（dllimport）。
**不需要 FFmpeg 开发包，也看不到 `core/`、`src/`。**

### 手工链接

```bat
:: Windows / MSVC
cl app.c /I dist\include /utf-8 dist\lib\op_sdk.lib
:: 运行时 op_sdk.dll + av*.dll 放 exe 同目录
```

```bash
# Linux
cc app.c -I dist/include -L dist/lib -lop_sdk -Wl,-rpath,'$ORIGIN/lib'
```

### 四个平台的产物

| 平台 | 资产 | 库 |
|---|---|---|
| Windows x64 | `op-sdk-1.0.0-win64.zip` | `op_sdk.dll` + `av*.dll` |
| Linux x64 | `op-sdk-1.0.0-linux-x64.tar.gz` | `libop_sdk.so*` + `libav*.so*` |
| HarmonyOS arm64 | `op-sdk-1.0.0-ohos-arm64.zip` | `libop_sdk.so` + `libav*.so` + `libc++_shared.so` |
| Android arm64 | `op-sdk-1.0.0-android-arm64.zip` | `libop_sdk.so` + `libav*.so` + `libc++_shared.so` |

交叉编译用 [`cmake/toolchains/ohos-arm64.cmake`](cmake/toolchains/ohos-arm64.cmake) /
[`cmake/toolchains/android-arm64.cmake`](cmake/toolchains/android-arm64.cmake)，
`-DFFMPEG_ROOT=<该平台 FFmpeg 前缀>`，移动端再加 `-DOP_PLAIN_SO=ON`。
导出宏、符号检查、单文件 DLL、常见问题见 **[docs/PACKAGING.md](docs/PACKAGING.md)**；
最小消费者见 [`examples/consumer`](examples/consumer)（纯 C，只 include 头文件）。

---

## API 速览

| 分类 | 接口 |
|---|---|
| 生命周期 | `op_create` / `op_destroy` / `op_version` |
| 回调 | `op_set_event_cb` / `op_set_video_cb` / `op_set_log_cb` |
| 打开 | `op_open`（路径或 `fd + fd_offset + fd_size`） |
| 控制 | `op_play` / `op_pause` / `op_stop` / `op_seek_ms` / `op_step_frame` / `op_exit_frame_step` / `op_set_speed` |
| 音频（pull） | `op_set_audio_format` / `op_audio_read` / `op_audio_pending` / `op_audio_latency_ms` / `op_audio_set_device_pending_ms` |
| 查询 | `op_position_ms` / `op_duration_ms` / `op_state_of` / `op_has_video` / `op_has_audio` / `op_is_music` / `op_is_stream` / `op_is_audio_media` / `op_get_width` / `op_get_height` / `op_get_speedX10` |
| 平台 | `op_platform` / `op_is_mobile` |
| 帧管理 | `op_frame_alloc` / `op_frame_clone` / `op_frame_free` |

### 线程与所有权（必读）

- 回调在 **SDK 自己的线程**（消息循环 / 解码 / 渲染）触发，不保证是调用方的 UI 线程 —— 壳要自己排队到主线程；
- **回调里禁止调 `op_destroy` / `op_stop`**：那是自己等自己，会死锁；
- `op_frame` 是**借用视图**，回调返回即失效；要跨线程用就先 `op_frame_clone`，用完 `op_frame_free`；
- `op_audio_read` 返回 **0 = 暂时没数据**（不是 EOF、不是错误），别忙转；
- 设备里"已经读走、还没播出去"的时长要用 `op_audio_set_device_pending_ms` 报回来，
  否则音频时钟会超前，视频判定"自己落后"然后疯狂丢帧；
- `op_destroy` 会 join 所有内部线程，**返回后不会再有任何回调**。

---

## 目录

```text
op-SDK/
├── include/op/op_player.h     # 唯一对外 C 接口（UTF-8 with BOM）
├── core/                      # 内核：零 Qt
│   ├── platform.h            #   编译期平台档位（队列深度 / 历史 / 逐帧）
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
│   ├── iaudiooutput.h        #   音频出口接口（push；对外包成 pull）
│   ├── fdavio.*              #   fd → 自定义 AVIOContext（虚拟文件）
│   └── log.h / perfclock.h   #   日志 / 纳秒时钟
├── src/                       # C 门面 + 音频环形缓冲
│   ├── op_player.cpp         #   pimpl：PlayerCore + 事件映射 + 状态缓存
│   └── audioring.*           #   SPSC 字节环 + AudioRingSink（IAudioOutput）
├── cmake/
│   ├── FFmpeg.cmake          #   FFMPEG_PATH / FFMPEG_ROOT / pkg-config 三种取法
│   ├── op_sdkConfig.cmake.in #   find_package(op_sdk CONFIG) 的包配置
│   └── toolchains/           #   ohos-arm64 / android-arm64 交叉编译
├── tests/                     # framequeue / api / audioring / fdavio / seek（CTest）
├── examples/
│   ├── qtplayer/             # Qt6 播放器壳（动态链接 op_sdk，独立子项目）
│   └── consumer/             # 最小消费者：纯 C，只 include 头文件 + find_package
└── docs/
    ├── API.md                # 使用说明书（调用方法、线程/所有权、FAQ）
    ├── PACKAGING.md          # 打包与多平台发行（导出宏、符号检查、布局）
    ├── SDK-PLAN.md           # 分阶段计划与验收标准
    ├── NOTES.md              # 开发笔记（AudioRing / 自定义 AVIO）
    └── AVIO.md               # AVIO 学习笔记（概念、示例、缓冲、排查表）
```

---

## 测试与当前状态

```bash
ctest --test-dir build --output-on-failure
```

| 项 | 状态 |
|---|---|
| `core/` 零 Qt，`op_core` 静态库 | ✅ |
| C 门面 T1/T2/T3/T4/T5（生命周期 / 事件 / 帧 / 音频 pull / fd） | ✅ |
| `ctest`（framequeue / api / audioring / fdavio / seek） | ✅ 5/5（seek 需 `OP_SDK_TEST_MEDIA`，否则 SKIP） |
| 动态库发行包（Win / Linux / 鸿蒙 / 安卓） | ✅ 已产出并在四平台交叉编译验证 |
| Qt 壳 `examples/qtplayer` | ✅ 只调 `op_*`，动态链接 `op_sdk` |
| 无 Qt 的 console 示例（PPM + PCM 落盘） | ❌ 未做（鸿蒙调试的对照组） |
| 真实媒体的音画同步验收（3s ≈ 60 帧、latency 稳定） | ⏳ 待做 |
| 鸿蒙壳（ArkTS + NAPI + XComponent） | ❌ 未开始 |

**移动端 FFmpeg 是裁剪版**：只保留常用 demuxer/decoder/parser/protocol
（mov/mkv/ts/flv/avi、mp3/aac/wav/flac/ogg、h264/hevc/mpeg4/mjpeg、file/http/rtsp/rtmp），
所以包很小；需要更多格式时按 [`docs/PACKAGING.md`](docs/PACKAGING.md) 里的 `--enable-*` 重编即可。

---

## 文档索引

| 文档 | 内容 |
|---|---|
| [docs/API.md](docs/API.md) | **使用说明书**：调用顺序、线程模型、帧所有权、音频 pull 与积压、FAQ |
| [docs/PACKAGING.md](docs/PACKAGING.md) | 打包/发行：动态库、安装布局、导出符号检查、各平台构建 |
| [docs/SDK-PLAN.md](docs/SDK-PLAN.md) | 分阶段计划与验收标准 |
| [docs/NOTES.md](docs/NOTES.md) | 开发笔记：AudioRing、自定义 AVIO、踩过的坑 |
| [docs/AVIO.md](docs/AVIO.md) | AVIO 入门与排查表 |
| [AGENTS.md](AGENTS.md) | 给 AI 助手的仓库说明（硬约束 / 已知坑 / 任务清单） |

---

## 和上游仓库的关系

- 本仓库是内核的**唯一开发源**；上游 [`QtFFmpegPlayer`](https://github.com/Oolongmilkcover/QtFFmpegPlayer)
  保留一份快照用于构建它自己的 Qt 播放器；
- 内核的 bug 请**先在本仓库修**，再由用户回落到上游（上游 `docs/CORE-FORK.md` 有回落流程）；
- 上游 `docs/problems-and-solutions.md` 第 24~33 条是内核化那一轮踩过的坑，值得一读。
