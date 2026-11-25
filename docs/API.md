# op-SDK 使用说明书

> 面向"要调用这个 SDK 的人"：怎么建、怎么开、怎么拿画面和声音、哪里会踩坑。
> 设计原因与踩坑记录在 `NOTES.md`，分阶段计划在 `SDK-PLAN.md`。

---

## 1. 它是什么

一个**零 Qt** 的播放内核（解封装 / 解码 / 帧队列 / 音画同步 / 时钟）+ 一层 C 接口。
同一个 `op_core` + `op_sdk` 同时驱动：

- 桌面 CLI（`op_audio_read` 拉 PCM、`op_video_cb` 拿帧）
- Qt 播放器（`examples/qtplayer`）
- 鸿蒙 / 安卓壳（NAPI / JNI 直接调 C）

构建产物：

| 目标 | 内容 |
|---|---|
| `op_core`（STATIC） | 内核，**零 Qt** |
| `op_sdk`（STATIC） | C 门面 + 环形音频 sink；PUBLIC 依赖 `op_core` 与 FFmpeg |

依赖：C++17、FFmpeg 8.x（avformat/avcodec/avutil/swresample/swscale/avfilter）、CMake ≥ 3.21。

---

## 2. 快速开始

```c
#include "op/op_player.h"

static void on_event(void *user, const op_event *ev) {
    /* 跑在 SDK 的消息循环线程，别直接摸 UI */
    if (ev->type == OP_EVENT_OPENED) {
        /* 文件级信息用查询接口（一次打开期间不变） */
        printf("duration=%lld, %dx%d, video=%d audio=%d\n",
               (long long)op_duration_ms(user),
               op_get_width(user), op_get_height(user),
               op_has_video(user), op_has_audio(user));
    } else if (ev->type == OP_EVENT_SEEK_DONE) {
        /* 点事件的值必须读 ev，别过后再查 */
        printf("seek landed at %lld\n", (long long)ev->position_ms);
    }
}

static void on_video(void *user, const op_frame *f) {
    /* 借用视图：回调返回即失效。要么当场用完，要么 op_frame_clone 带走 */
    uploadTextures(f->data, f->linesize, f->width, f->height);
}

int main(void) {
    op_player *p = op_create();
    op_set_event_cb(p, on_event, p);
    op_set_video_cb(p, on_video, p);
    op_set_audio_format(p, 48000, 2);      // 必须在 open 之前

    op_open_options o = {0};
    o.fd = -1;
    o.path = "a.mp4";
    op_open(p, &o);

    /* 音频线程：拉模式 */
    for (;;) {
        short buf[2048];
        int n = op_audio_read(p, buf, sizeof(buf));
        if (n > 0) device_write(buf, n);
        else       sleep_ms(3);
    }

    op_destroy(p);   // join 所有线程，返回后不再有回调
}
```

---

## 3. 生命周期与线程模型

| 接口 | 在哪个线程跑 |
|---|---|
| `op_create` / `op_destroy` | 调用方任意线程；`op_destroy` 会 join 全部线程 |
| `op_event_cb` | SDK 的**消息循环线程** |
| `op_video_cb` | SDK 的**渲染/解码线程** |
| `op_log_cb` | **打日志的那个线程**（解封装/解码/渲染/音频/消息循环，任意一个） |
| 其它 `op_*` | 任意线程（内部异步投递，包括从回调里调） |

**三条铁律**（违反会崩或死锁）：

1. **回调里禁止调 `op_destroy` / `op_stop`** —— 那是自己等自己。
2. **回调里别直接碰 UI** —— 用平台的排队机制（Qt `QueuedConnection`、鸿蒙 `napi_threadsafe_function`、安卓 `Handler`）。
3. **`op_destroy` 返回后不会再有任何回调**；要保证这一点，销毁前先停掉你自己那些会调 `op_audio_read` 的线程。

---

## 4. 打开

```c
typedef struct op_open_options {
    const char *path;           /* 路径/URL；有 fd 时可为 NULL（只当扩展名提示） */
    int32_t     fd;             /* >=0 时优先用 fd；-1 = 不用 */
    int64_t     fd_offset;      /* 从文件第几个字节开始 */
    int64_t     fd_size;        /* 可读长度；0 = 自动探测 */
    int32_t     open_timeout_ms;/* 网络流打开超时，0 = 默认 */
} op_open_options;
```

- **路径**：本地路径、`rtsp://` / `rtmp://` / `http://` 都行。
- **fd**：鸿蒙 picker / 安卓 `content://` 只能拿 fd。约定：
  - fd 归调用方，SDK **不 close**；但**播放期间必须一直开着**（demux 全程在读）。
  - SDK 会**移动 fd 的读写位置**（探测、seek）；要并行读请自己 `dup()`。
  - 只支持**可 seek 的普通文件**；管道/套接字能打开，但没有时长、不能 seek。
  - `path` 此时只是扩展名提示（`"a.mp4"` 能让格式探测少猜几次）。
- **异步**：返回 `OP_OK` 只表示"受理了"，真正结果看 `OP_EVENT_OPENED` / `OP_EVENT_ERROR`。
  ⚠️ 但内核现在是在消息循环线程里**同步**做 `avformat_open_input` 的：网络流建连那几秒，
  后续的 seek/pause 都会排队。

重复 `op_open` 会先关掉上一个（`op_stop` 之后再打开是安全的）。

---

## 5. 事件

```c
typedef struct op_event {
    int32_t     type;        /* op_event_type */
    int32_t     state;       /* OP_EVENT_STATE */
    int32_t     code;        /* OP_EVENT_ERROR */
    int64_t     position_ms; /* POSITION / SEEK_DONE / COMPLETED */
    const char *message;     /* ERROR，只在回调期间有效 */
} op_event;
```

| 事件 | 何时发 | 你该做什么 |
|---|---|---|
| `OPENED` | 打开成功、首帧就绪 | 用查询接口取时长/宽高/有无音视频；初始化渲染器 |
| `POSITION` | 约 150ms 一次 | 更新进度条（值在 `position_ms`） |
| `STATE` | 状态机变化 | 更新按钮（`state`） |
| `SEEK_DONE` | 一次 seek 落地 | 用 `position_ms` 校正进度条，解除"禁止再 seek" |
| `BUFFERING` | 网络缓冲 | 显示 loading |
| `COMPLETED` | 播到文件尾 | 切下一集 / 停止；内核随后会自动从头重播 |
| `ERROR` | 打开/解码失败 | 弹错误（`code` + `message`）；状态回到 IDLE |

**点事件 vs 查询接口**（重要）：

- 事件里的 `position_ms` 是**那一刻**的值。回调可能排队到 UI 线程，
  到时候 `op_position_ms()` 早就变了 —— 所以点事件必须用 `ev->position_ms`。
- 文件级属性（宽高/时长/有无音视频/是否流）在一次打开期间不变，
  用 `op_get_width` / `op_get_height` / `op_duration_ms` / `op_has_video` /
  `op_has_audio` / `op_is_stream` / `op_is_music` 查询即可，不必塞进事件。

---

## 6. 视频帧

```c
typedef struct op_frame {
    uint8_t *data;        /* 一块紧凑连续缓冲：Y、U、V 依次排列 */
    int32_t  size;        /* data 字节数 */
    int32_t  linesize[4]; /* 每平面每行字节数（按紧凑布局算） */
    int32_t  width, height;
    int64_t  pts_ms;
    int32_t  format;      /* OP_PIX_FMT_YUV420P */
} op_frame;
```

平面偏移（仅 YUV420P）：

```text
Y = data
U = data + linesize[0] * height
V = U    + linesize[1] * (height / 2)
```

**所有权**：

- 回调里那个 `op_frame*` 是**借用视图**，返回即失效。**不要 free，也不要存指针**。
- 需要跨线程交付（Qt GUI 线程、鸿蒙 JS 线程）就 **`op_frame_clone`** 一份带走，用完 `op_frame_free`。
- 也能自己 `op_frame_alloc(size)` 造一帧（比如停止时画黑帧），同样用 `op_frame_free` 释放。

**为什么是紧凑缓冲**：`linesize` 是按这个紧凑布局重算的，不是源 `AVFrame` 的 16/32 字节对齐值。
逐行上传时直接用 `linesize`；想一次上传就按上面的偏移算平面。

---

## 7. 音频：拉模式

```c
op_set_audio_format(p, 48000, 2);   // 必须在 open 之前；默认就是 48000/2
...
int n = op_audio_read(p, buf, cap); // >0 字节数；0 = 暂时没数据；<0 是错误码
```

PCM 格式：**S16 交错**（LRLRLR…）。返回 **0 不是错误**，睡 2~5ms 再来。

```c
/* 每 100~200ms 报一次"设备里还有多少没播"（毫秒） */
op_audio_set_device_pending_ms(p, device_pending_ms);
```

这条是**音画同步的关键**：SDK 只知道自己环形缓冲里压了多少，设备里那份只有你知道。
漏报 → 音频时钟超前 → 视频疯狂丢帧（实测 3 秒只渲染 6 帧）。

各平台怎么算：

| 平台 | 设备积压 |
|---|---|
| Qt | `sink->bufferSize() - sink->bytesFree()` 换算成毫秒 |
| 鸿蒙 | `getAudioTime()` / `getWrittenDataSize()` 之类的差值 |
| 安卓 | `已写字节 − getPlaybackHeadPosition() × frameSize` |

其它：

- `op_audio_pending(p)`：环形缓冲里可读字节（做回压用）。
- `op_audio_latency_ms(p)`：ring 未读 + 设备积压的总毫秒数。
- **没有音量接口**：PCM 已经交给你了，音量由壳自己缩放（或直接用平台音量）。

---

## 8. 状态与查询

```c
int64_t op_position_ms(p);   // 当前播放位置（毫秒）
int64_t op_duration_ms(p);   // 总时长；直播流为 0
int32_t op_state_of(p);      // OP_STATE_*
int32_t op_has_video(p);     // OPENED 之后才有效
int32_t op_has_audio(p);
int32_t op_is_music(p);      // 有音轨且没有视频轨
int32_t op_is_stream(p);
int32_t op_get_width(p);
int32_t op_get_height(p);
int32_t op_get_speedX10(p);  // 倍速 ×10
```

查询接口读的是门面的状态缓存，**任意线程可调**，不会阻塞。
但它是"此刻"的值，不是某个历史事件的快照。

---

## 9. 控制

| 接口 | 说明 |
|---|---|
| `op_play` / `op_pause` | 异步；状态查询要等 `STATE` 事件或轮询 |
| `op_stop` | 回到 IDLE，句柄仍可复用（内部销毁解码线程） |
| `op_seek_ms` | 异步；一次请求**恰好一条** `SEEK_DONE`（含早退路径） |
| `op_step_frame(p, ±1)` | 仅暂停态、有视频、非流媒体；`-1` 靠 48 帧历史回填 |
| `op_exit_frame_step` | 退出逐帧，播放位置对齐到当前显示帧 |
| `op_set_speed(p, 5~20)` | ×10，0.5x~2.0x；流媒体返回 `OP_ERR_UNSUPPORTED` |

所有控制接口都可能返回 `OP_ERR_NOT_READY`（还没 open / 已 stop）或
`OP_ERR_UNSUPPORTED`（流媒体不允许的操作）—— 必须处理，别假设一定成功。

---

## 10. 日志

```c
op_set_log_cb(p, on_log, user);   // level: 0=debug 1=info 2=warn 3=error
```

- 跑在**打日志的那个线程**，不是消息循环线程；`line` 只在回调期间有效。
- 多实例时日志是**进程级广播**：注册在 A 上的回调也会收到 B 的日志。
- 不注册回调时日志写 stderr（不会消失）。

---

## 11. 错误处理

- 所有接口用 `op_result` 返回码，不抛异常。
- 打开失败 → `OP_EVENT_ERROR`（`code` + `message`），状态回 IDLE。
- 不要在错误后继续用旧句柄：先 `op_stop` 再重新 `op_open`。
- `op_destroy(nullptr)` / `op_version(nullptr, nullptr)` 是安全的。

---

## 12. 平台接入要点

**Qt**：
- 事件/视频回调 → `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`。
- 音频：给 `QAudioSink` 一个只读 `QIODevice`，在 `readData()` 里调 `op_audio_read`；
  在 POSITION 事件里把 `bufferSize()-bytesFree()` 报给 SDK。参考 `examples/qtplayer/platform/audioplayer.cpp`。

**鸿蒙**：
- NAPI 用 `napi_threadsafe_function` 把 SDK 线程的回调转回 JS 线程。
- 视频走 `XComponent` + EGL（和 Qt 的三平面 GL 上传同构）。
- 音频在 `AudioRenderer` 的写回调里 `op_audio_read`。
- picker 的 URI 用 `fileIo.openSync` 拿 fd → `op_open`（`fd_offset/size` 按需）。

**安卓**：JNI 直接调 C；音频在 `AudioTrack` 的写入循环里拉；`content://` 拿 fd。

---

## 13. 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| 没声音，`op_has_audio()==0` | 检查 `op_set_audio_format` 是否在 open 前调用、通道数是否 ≤8、WAV/MP3 是否真有音轨 |
| 视频疯狂丢帧 | 没调 `op_audio_set_device_pending_ms`，音频时钟超前 |
| 一 seek 就卡住 | 在 `op_event_cb` 里调了 `op_seek_ms` 之外还调了 `op_stop`？回调里禁止阻塞/销毁 |
| 退出时崩溃 | 先停了"调 op_audio_read 的线程"，再 `op_destroy`；顺序反了就是 use-after-free |
| `op_open` 立即返回但没后续 | 正常，结果通过 `OPENED`/`ERROR` 通知 |
| 画面花屏/斜切 | 逐行上传时用了源 `AVFrame` 的对齐 `linesize`，应该用 `op_frame.linesize` |
| 鸿蒙上 fd 打开失败 | fd 必须可 seek 的普通文件；播放期间不能关 |

---

## 14. 头文件是唯一契约

`include/op/op_player.h` 里写的线程约定、所有权、返回值语义都是**接口的一部分**。
改 SDK 时先改注释、再改实现；调用方只看头文件就该能正确使用。
