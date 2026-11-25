# examples —— SDK 的消费者示例

这个目录放"用 `op-SDK` 做出来的东西"，**不是 SDK 本身**。

```text
examples/
└── qtplayer/     ← Qt 播放器壳（只放 UI/平台层，内核与门面直接用主仓库的）
```

## qtplayer 是什么

它是**参考消费者**：一个 Qt6 播放器（`app/` 界面 + `platform/` 平台层），从上游
`QtFFmpegPlayer` 复制过来，用来演示/试验"怎么把这个 SDK 接进真实的图形程序"。

上游仓库里的**推流端（`op_pusher` / mini-OBS）没有搬进来**：它和播放器、和这个 SDK 都没有关系
（自带 `capturesource/encoder/muxer`，用的是自己的 `dropqueue` 而不是 core 的队列），
放在示例里只会让人误以为"SDK 还要管推流"。需要的话去上游仓库取。

**它自己不再持有内核，也没有第二份门面**：

| 层 | 从哪来 |
|---|---|
| `core/`（解封装/解码/队列/时钟） | **主仓库**的 `core/`，编成 `op_core` |
| `include/op/op_player.h` + `src/`（C 门面） | **主仓库**的，编成 `op_sdk` |
| `cmake/FFmpeg.cmake` | **主仓库**的，定义 `ffmpeg_deps` |
| `app/` `platform/` `resources/` | 本目录（壳自己的东西） |

做法就一行 `add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. …)`，见 `qtplayer/CMakeLists.txt`
里"内核与门面：用主仓库的，不在这里复制"那段。

**当前状态：已适配 C 接口（2026-11-25）。**
`app/player.cpp` 只调 `op_*`（`op_create / op_open / op_play / op_pause / op_seek_ms …`），
回调是 static 成员 + `PlayerCtx::self` 回到对象；视频帧在回调里 `op_frame_clone` 后
`QueuedConnection` 交给 GUI；音频是 `QAudioSink` + 只读 `QIODevice`（`readData` 里
`op_audio_read`），并在 POSITION 事件里回传设备积压。播放列表只属于壳，core 不再参与。

## 为什么不在根 `CMakeLists.txt` 里 `add_subdirectory(examples/qtplayer)`

1. **自引用**：这个子项目靠 `add_subdirectory(../..)` 把仓库根加进来；根再反过来加它，
   就是"自己包含自己"，CMake 直接递归报错。
2. **Qt6 vs 零 Qt**：它需要 Qt6，而本仓库的硬约束是"`core/` 与 `op_core` 里一行 Qt 都没有"
   （见 `AGENTS.md` 第 4 节第 1 条）。并进来会把 Linux CI、鸿蒙交叉编译全拖住。

所以它是**独立子项目**：自己的 `project()`、自己的 `CMakeLists.txt`、自己的构建目录、自己的 kit。

## 怎么打开 / 构建

Qt Creator：`Open File or Project` → 选 **`examples/qtplayer/CMakeLists.txt`**（不是仓库根那个）
→ 选一个 Qt 6 的 kit（本机 `Desktop Qt 6.11.3 MinGW 64-bit`）。

命令行（本机验证过的参数）：

```powershell
cmake -S examples/qtplayer -B examples/qtplayer/build -G "MinGW Makefiles" `
      -DCMAKE_CXX_COMPILER="C:/Qt/Tools/mingw1310_64/bin/g++.exe" `
      -DCMAKE_C_COMPILER="C:/Qt/Tools/mingw1310_64/bin/gcc.exe" `
      -DCMAKE_PREFIX_PATH="C:/Qt/6.11.3/mingw_64" `
      -DFFMPEG_PATH="C:/Program Files/ffmpeg/ffmpeg8.1"
cmake --build examples/qtplayer/build -j 4
```

产物：`QtPlayer.exe`（4~12 MB，看工具链）。
`FFMPEG_PATH` 由主仓库的 `cmake/FFmpeg.cmake` 使用，含义和 SDK 那边一致。

**调试用的两个入口**（2026-11-25 加的）：

```text
QtPlayer.exe <media>     直接打开并播放（不用点文件对话框）
QtPlayer.exe --audiotest 最小声卡自检：只建 QAudioSink、写静音，
                         打印 state/bufferSize/bytesFree，用来区分
                         "Qt 音频后端坏了"还是"我们的封装有问题"
```

另外支持几个环境变量：

| 变量 | 作用 |
|---|---|
| `OP_QT_NO_AUDIO=1` | 整场跳过音频设备初始化（判断"卡住是不是被音频时钟拖的"） |
| `OP_QT_SEEK_MS=5000` | 打开 1.2 秒后自动 seek 一次，并打印 `[seek-test]` 的位置/落点（排查 seek） |
| `OP_QT_SEEK_EVERY=400` | 每 400ms 自动 seek 一次（压测 seek 路径与内存；配合任务管理器看 RSS） |

> 如果你之前用"自带 core 的旧版 CMakeLists"配过这个目录，**先删掉 `examples/qtplayer/build/`
> 再重新配置** —— 源清单和 include 路径全变了，旧 cache 会指向已删除的 `core/*.cpp`。

## 适配时要做的（把 `app/` 改成走 C 接口）

1. **只依赖 `op_sdk`**：`CMakeLists.txt` 已经链了 `op_sdk`（它 PUBLIC 带着 `op_core` 和
   `ffmpeg_deps`）。等 `app/` 不再 include `core/*.h` 之后，调用方就真正只看见 C 接口了。
2. **`app/player.cpp` 换成 C 接口**：现在的 `CMD_OPEN` / `CMD_PAUSE` / 信号槽换成
   `op_create / op_set_event_cb / op_set_video_cb / op_open / op_play / op_pause / op_seek_ms …`。
3. **回调要排队**：`op_event_cb` / `op_video_cb` 跑在 **SDK 的消息循环线程**上，不能直接碰 QWidget。
   用 `QMetaObject::invokeMethod(this, …, Qt::QueuedConnection)` 转发（等价于原来的
   `Qt::QueuedConnection` 信号）。`test_api.cpp` 里有个"断言回调不在调用方线程"的用例可参考。
4. **`op_video_cb` 里不要存 `op_frame` 指针**：那是借用视图，回调返回即失效；要在回调里当场
   `glTexSubImage2D`/`memcpy`。帧底下的 `AVFrame` 由 SDK 释放，调用方**不许** free。
   注意 `op_frame` 是**紧凑连续缓冲**（`data/size/linesize[4]`），平面偏移见头文件注释。
5. **音量交给壳自己**：SDK 故意没有 `op_set_volume`（见头文件"播放控制"那段）。
6. **音频改成 pull**：`QAudioSink` 的 `QIODevice::readData()` 里调 `op_audio_read`，并把
   `bufferSize() − bytesFree()` 通过 `op_audio_set_device_pending_ms` 报回去。
   为什么必须报、漏了会怎样：见 `AGENTS.md` 第 8 节 T4 的"陷阱"表。
   现在 `platform/audioplayer.cpp` 是 push 形态（`IAudioOutput` 实现），改的时候整个换掉。

## 复制过来时已经同步过的两处（不是适配，是"壳的拷贝比 SDK 旧"）

| 位置 | 原因 | 改法 |
|---|---|---|
| `app/player.cpp` 的 `new CMD_OPEN_OBJ(...)` | T4 给 `CMD_OPEN_OBJ` 加了 `fd/offset/size` + 输出格式四个参数 | 传 `(-1, 0, 0, path, cb, out, 48000, 2)` |
| `app/player.cpp` 的 `case EVT_STATE_CHANGED:` | `b31fe2f` 删掉了这个"从来没被发过"的枚举 | 整段删掉（状态由门面合成，壳侧不需要处理） |

## 别做的事

- ❌ 不要把 `core/`、`include/`、`src/`、`cmake/` 再复制回这个目录（那样就有两份实现，必然漂开）。
- ❌ 不要把 Qt6 依赖、`Q_OBJECT`、`#include <Q...>` 带进主仓库的 `core/` 或 `include/op/`。
- ❌ 不要提交 `build/`、`.qtcreator/`、`*.user`（根 `.gitignore` 已经覆盖）。
