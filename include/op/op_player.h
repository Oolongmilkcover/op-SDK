/*
 * ============================================================================
 *  op_player.h —— 播放内核的 C 接口（唯一对外面）
 * ============================================================================
 *
 *  这份头文件放这里是给新仓库（SDK）当起点的：QtPlayer 自己**不编译、不链接**它。
 *
 *  【实现状态 · 2026-11-21】**T1 + T2 已实现；T3 已连通待真文件验收；T4/T5 还是桩**。
 *  已能用：create / destroy / version / 回调注册（事件 / 视频帧 / 日志）/ open /
 *  play / pause / stop / seek / step / speed，以及 EVT_* → op_event 的映射与状态缓存
 *  （op_state_of / position_ms / duration_ms / has_video / has_audio / is_music / is_stream）。
 *  仍是桩：音频 pull（T4）、fd 打开（T5）—— 返回 OP_ERR_UNSUPPORTED 并打一行日志，
 *  不会静默成功。下面每段的标题标了它属于哪个任务、现在是什么状态：
 *
 *      T1 骨架        create / destroy / version / 回调 / 播放控制    已实现
 *      T1+ 日志       op_set_log_cb ← core::setLogSink（进程级广播）  已实现
 *      T2 事件        EVT_* → op_event + 状态缓存 + 查询              已实现
 *      T3 视频帧      op_video_cb + op_frame（紧凑缓冲视图）           已连通
 *      T4 音频 pull   op_audio_read / pending / latency_ms            桩
 *      T5 fd 打开     op_open_options.fd（自定义 AVIO）                桩
 *
 *  为什么是 C 而不是 C++：
 *    鸿蒙只能经 NAPI 调 C 函数、安卓要走 JNI，C++ 对象（std::function / 虚表 /
 *    std::atomic）过不了这些边界；而且 C 接口才有 ABI 可言（同一份 .dll/.so
 *    能被不同编译器、不同版本的调用方使用）。
 *
 *  设计上的三个关键决定（都是为鸿蒙/安卓提前定的）：
 *    1. 音频是 **pull**（`op_audio_read`），不是 push 回调。
 *       鸿蒙 AudioRenderer / 安卓 AudioTrack 都是"设备来要数据"，让调用方在自己的
 *       音频线程里拉，最接近原生；SDK 内部仍然可以保留 push 的 IAudioOutput，
 *       只是外面包一层环形缓冲。
 *    2. `op_open` 支持 **fd**。鸿蒙 picker 给的是 URI、安卓是 content://，
 *       NDK 侧基本只能拿到文件描述符；只收路径的话两个平台都要在外面"先拷进沙箱"。
 *    3. 视频帧是 **借用视图**（`op_frame`），不跨边界传 AVFrame*。
 *       AVFrame 里有引用计数、有 FFmpeg ABI，不该泄给调用方。
 *
 *  **音量不归 SDK 管**（2026-11-21 定）：音频是 pull 模式，PCM 已经交到调用方手里，
 *  音量由壳自己处理（对样本做缩放，或用平台播放器/系统音量）。所以这里没有
 *  op_set_volume —— 为什么这么定，见"播放控制"那一段的注释。
 *
 *  还没定稿、要在真机上收敛的两处：
 *    - 事件结构体是否需要"复合事件"（当前一次 OPENED 同时带时长和宽高，够用）；
 *    - 是否需要 `op_audio_read` 之外再提供"设备延迟"接口（当前给了 latency_ms）。
 *
 *  【编码】本文件是 UTF-8 **带 BOM**。注释里全是中文，而 MSVC 默认按系统代码页
 *  （中文 Windows = 936/GBK）解析源文件：不带 BOM 又不加 /utf-8 时，每个消费方
 *  都会吃到 C4819，运气不好还会把注释里的多字节序列解析成引号/花括号。
 *  BOM 是唯一"不需要消费方配合开关"的解法 —— 别把它当垃圾删掉。
 */

#ifndef OP_PLAYER_H
#define OP_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 导出宏 */
#if defined(_WIN32)
#  if defined(OP_BUILD_SHARED)
#    define OP_API __declspec(dllexport)
#  elif defined(OP_USE_SHARED)
#    define OP_API __declspec(dllimport)
#  else
#    define OP_API
#  endif
#else
#  if defined(OP_BUILD_SHARED)
#    define OP_API __attribute__((visibility("default")))
#  else
#    define OP_API
#  endif
#endif

/* --------------------------------------------------------------- 版本号 */
#define OP_API_VERSION_MAJOR 0
#define OP_API_VERSION_MINOR 1

/* --------------------------------------------------------------- 错误码 */
typedef enum op_result
{
    OP_OK              =   0,
    OP_ERR_INVALID_ARG =  -1,   /* 参数非法（空指针、越界值） */
    OP_ERR_OPEN_FAILED =  -2,   /* 打不开：文件不存在 / 格式不支持 / 网络失败 */
    OP_ERR_NO_STREAM   =  -3,   /* 打开了但没有可用的音视频流 */
    OP_ERR_UNSUPPORTED =  -4,   /* 当前平台/编码不支持 */
    OP_ERR_NOT_READY   =  -5,   /* 还没 open / 已经 stop */
    OP_ERR_BUSY        =  -6,   /* 上一个操作还没完成（比如 seek 进行中） */
    OP_ERR_IO          =  -7,   /* 读写设备/网络失败 */
    OP_ERR_INTERNAL    =  -8
} op_result;

/* --------------------------------------------------------------- 播放状态 */
typedef enum op_state
{
    OP_STATE_IDLE      = 0,     /* 还没打开任何文件 */
    OP_STATE_OPENING   = 1,     /* open 已受理，结果稍后以事件通知 */
    OP_STATE_READY     = 2,     /* 打开成功，首帧就绪，可以播 */
    OP_STATE_PLAYING   = 3,
    OP_STATE_PAUSED    = 4,
    OP_STATE_STOPPED   = 5,
    OP_STATE_ENDED     = 6      /* 播到结尾 */
} op_state;

/* --------------------------------------------------------------- 事件类型 */
typedef enum op_event_type
{
    OP_EVENT_OPENED     = 1,    /* duration_ms / width / height / has_video / has_audio / is_stream */
    OP_EVENT_POSITION   = 2,    /* position_ms：约 150ms 一次，供进度条用 */
    OP_EVENT_STATE      = 3,    /* state：状态机变化 */
    OP_EVENT_SEEK_DONE  = 4,    /* position_ms：实际落点（可能和请求值不同，要对齐关键帧） */
    OP_EVENT_BUFFERING  = 5,    /* has_video：1 开始缓冲 / 0 结束（网络流才有意义） */
    OP_EVENT_COMPLETED  = 6,    /* 播到文件尾（内核随后会自动回 0 重播；想停在结尾就在这里的回调里调 op_pause） */
    OP_EVENT_ERROR      = 7     /* code + message */
} op_event_type;

/*
 * 事件：**扁平结构**（不用 union / 嵌套），两个原因：
 *   - FFI 友好：NAPI / JNI 逐字段读最简单，不用处理联合体；
 *   - 以后加字段不会改变已有字段的偏移，二进制兼容更容易保。
 * 哪个字段在哪个事件里有效，见上面每个枚举后面的注释。
 */
typedef struct op_event
{
    int32_t     type;           /* op_event_type */
    int32_t     state;          /* OP_EVENT_STATE */
    int32_t     code;           /* OP_EVENT_ERROR：op_result 或 FFmpeg 错误码 */
    int32_t     width;
    int32_t     height;
    int64_t     duration_ms;
    int64_t     position_ms;
    int32_t     has_video;      /* OPENED：有无视频；BUFFERING：1/0 */
    int32_t     has_audio;
    int32_t     is_stream;      /* 网络流（RTSP/RTMP/HLS...）时为 1 */
    const char *message;        /* 错误文本，**只在回调期间有效**，不归调用方释放 */
} op_event;

/* --------------------------------------------------------------- 视频帧 */
/*
 * 视频帧视图：**数据归 SDK 所有**，只在 `op_video_cb` 回调期间有效。
 * 调用方要么在回调里同步用完（比如直接 glTexSubImage2D 上传），
 * 要么自己 memcpy 走。回调返回后这块内存随时可能被复用。
 *
 * 所有权只说一遍：**调用方什么都不用释放，也不许释放**。
 * 底下的 AVFrame 由 SDK 在回调返回后自己 av_frame_free（内核里 painter 接走帧的
 * 所有权就是这个约定），所以既不要 free(data)，也不要把指针存起来之后再用。
 */
typedef struct op_frame
{
    /*
     * 一块**紧凑连续的缓冲**：Y、U、V 依次排列，行与行之间没有对齐填充。
     * 平面偏移（仅 format=0 / YUV420P 时成立）：
     *     Y = data
     *     U = data + linesize[0] * height
     *     V = U    + linesize[1] * (height / 2)
     * 想要"三平面各自起点"的用法，按上面三行算；这里不额外给指针数组，
     * 是因为壳大多数时候一次上传整块（或按 offset 传 GPU）。
     */
    uint8_t       *data;
    int32_t        size;        /* data 的字节数（= av_image_get_buffer_size(...,1)） */
    int32_t        linesize[4]; /* 每平面**每行字节数，按上面这种紧凑布局算**（Y 一般是 width，
                                   不是源 AVFrame 那个 16/32 字节对齐后的值） */
    int32_t        width;
    int32_t        height;
    int64_t        pts_ms;      /* 显示时间戳（毫秒） */
    int32_t        format;      /* 0 = YUV420P（本版本只有这一种） */
} op_frame;

/* --------------------------------------------------------------- 回调 */
/* 事件回调：在 SDK 的消息循环线程上触发。禁止阻塞、禁止在回调里调 op_destroy/op_stop。 */
typedef void (*op_event_cb)(void *user, const op_event *ev);

/* 视频帧回调：在 SDK 的解码/渲染线程上触发，每帧一次。同样禁止阻塞。 */
typedef void (*op_video_cb)(void *user, const op_frame *frame);

/*
 * 日志回调：level 0=debug 1=info 2=warn 3=error；line 是**一整行、不带换行**的
 * UTF-8 文本，只在回调期间有效（要留就当场拷走）。
 *
 * 线程：在**打日志的那个 SDK 线程**上触发 —— 可能是解封装、视频解码、渲染、
 * 音频或消息循环线程中的任意一个，**不保证**是消息循环线程。壳要更新 UI 就自己
 * 排队（鸿蒙走 napi_threadsafe_function，Qt 走 QueuedConnection）。
 *
 * 规则（和事件回调一样）：回调里禁止阻塞、禁止做重活（写文件 / 加锁 / 跨进程通信）；
 * 禁止在回调里调 op_destroy / op_stop（那是自己等自己，会死锁）。
 *
 * 归属：日志出口在 core 里是**进程级**的，SDK 不知道某一行属于哪个 op_player
 * （core 的线程拿不到 player 指针）。所以多实例时，注册在 A 上的回调也会收到 B 的
 * 日志 —— 单播放器场景完全精确，多播放器时请自己按内容区分或只在一个实例上注册。
 */
typedef void (*op_log_cb)(void *user, int level, const char *line);

/* --------------------------------------------------------------- 句柄 */
typedef struct op_player op_player;

/* --------------------------------------------------------------- 打开参数 */
/*
 * 路径和 fd 二选一：fd >= 0 时优先用 fd（鸿蒙/安卓的文件选择器场景）。
 * fd 归调用方所有，SDK 不负责关闭它。
 */
typedef struct op_open_options
{
    const char *path;           /* 本地路径或 URL（rtsp:// rtmp:// http:// ...） */
    int32_t     fd;             /* 文件描述符；-1 表示不用 */
    int64_t     fd_offset;      /* fd 内的起始偏移，通常是 0 */
    int64_t     fd_size;        /* fd 可读长度，0 = 由 SDK 探测（普通文件） */
    int32_t     open_timeout_ms;/* 网络流打开超时，0 = 默认（当前 15s） */
} op_open_options;

/* ================================================================== */
/*  生命周期                                    状态：已实现（T1 骨架）   */
/*                                                                    */
/*  线程约定（很重要）：                                                */
/*   - op_create / op_destroy：在调用方的任意一个线程调用；op_destroy  */
/*     会 join 内部所有线程，返回后不会再有任何回调。                    */
/*   - **回调里不要调 op_destroy / op_stop**：那是自己等自己，会死锁。   */
/*   - 其它接口（play/pause/seek/...）都是异步投递，可以从任意线程调，   */
/*     包括从回调里调（这不是死锁，但会排队到下一个消息循环）。           */
/* ================================================================== */

OP_API op_player *op_create(void);
OP_API void       op_destroy(op_player *p);

/* 版本（major/minor 可为 NULL） */
OP_API op_result  op_version(int32_t *major, int32_t *minor);

/* 注册回调；cb 传 NULL 表示取消。user 会原样回传给回调。 */
OP_API op_result  op_set_event_cb(op_player *p, op_event_cb cb, void *user);
OP_API op_result  op_set_video_cb(op_player *p, op_video_cb cb, void *user);
OP_API op_result  op_set_log_cb  (op_player *p, op_log_cb   cb, void *user);

/* ================================================================== */
/*  打开 / 播放控制                      状态：已实现（T1/T2；fd 待 T5）   */
/* ================================================================== */

/*
 * 异步打开：立刻返回 OP_OK 表示"已受理"，真正结果通过
 * OP_EVENT_OPENED 或 OP_EVENT_ERROR 通知。重复 open 会先关掉上一个。
 *
 * 实现提示（T1，别被"异步"骗了）：内核现在的 CMD_OPEN 是在 SDK 的消息循环线程里
 * 同步做 avformat_open_input 的 —— 网络流建连那几秒里，整条消息循环（连同 seek /
 * pause）都在排队。要做到"真的立刻返回"，得把 open 挪到工作线程去；
 * 在此之前，OP_OK 准确的含义是"消息进队了"。
 *
 * 打开成功后内核会马上开始推进（解码/渲染/音频线程起来），想"停在第一帧等人按播放"，
 * 要在 OPENED 回调里自己调 op_pause —— 目前没有"打开但不自启"的开关。
 */
OP_API op_result op_open(op_player *p, const op_open_options *opt );

OP_API op_result op_play (op_player *p);
OP_API op_result op_pause(op_player *p);
OP_API op_result op_stop (op_player *p);      /* 回到 IDLE，p 仍可复用 */

OP_API op_result op_seek_ms(op_player *p, int64_t ms);

/* 逐帧：dir 取 +1（下一帧）或 -1（上一帧，靠回填）。纯音频/流媒体返回 OP_ERR_UNSUPPORTED。 */
OP_API op_result op_step_frame(op_player *p, int32_t dir);
OP_API op_result op_exit_frame_step(op_player *p);

/* 倍速，用 ×10 的整数避免 C 接口里出现 double：5~20 表示 0.5x ~ 2.0x。 */
OP_API op_result op_set_speed(op_player *p, int32_t speed_x10);

/*
 * 音量：**故意没有这个接口**（2026-11-21 定，交给壳）。
 *
 * 音频是 pull 的，PCM 已经读走了。SDK 内部再存一份音量，就会和平台设备自己的音量
 * 叠乘（系统音量 20% × SDK 的 50% = 实际 10%），用户看到的是"调了没反应"或者
 * "怎么突然这么小声"，而且调音量的曲线该由壳决定，SDK 猜不准。
 *
 * 壳侧要做的事：对 op_audio_read 拿到的 S16 交错样本做一次缩放即可；
 * 如果直接用平台播放器（QAudioSink / AudioRenderer / AudioTrack）的音量接口，
 * 连缩放都不用。
 */

/* ================================================================== */
/*  音频：**拉模式**                               状态：桩（未实现，T4）   */
/*                                                                    */
/*  调用方在自己的音频线程里循环 op_audio_read()，把拿到的 PCM 写进     */
/*  平台的 AudioRenderer / AudioTrack。返回 0 表示暂时没数据（别忙转，   */
/*  睡 2~5ms 再试）。缓冲由 SDK 内部维护，并据此估算音频延迟。           */
/*                                                                    */
/*  PCM 格式：S16 交错（LRLRLR...），采样率/声道数由 op_set_audio_format */
/*  指定，默认 48000 / 2。                                             */
/* ================================================================== */

/*
 * 必须在 op_open **之前**调用：设备（和内部重采样器）是在 open 那一刻按这组参数
 * 配置好的，open 之后再改，要么静默不生效，要么得把整条音频链路重启一遍。
 * 默认 48000 / 2。
 */
OP_API op_result op_set_audio_format(op_player *p, int32_t sample_rate, int32_t channels);

/* 返回实际读到的字节数；0 = 暂时没数据；< 0 = op_result 错误码 */
OP_API int32_t   op_audio_read(op_player *p, void *buf, int32_t capacity);

/* 缓冲里还有多少字节（调用方据此做回压 / 判断是否该等一下） */
OP_API int32_t   op_audio_pending(op_player *p);

/* 已缓冲但还没播的时长（毫秒）——音频时钟的延迟补偿量 */
OP_API int64_t   op_audio_latency_ms(op_player *p);

/* ================================================================== */
/*  查询（线程安全，任意线程可调）             状态：已实现（T2 缓存）   */
/* ================================================================== */

OP_API int64_t   op_position_ms(op_player *p);
OP_API int64_t   op_duration_ms(op_player *p);
OP_API int32_t   op_state_of  (op_player *p);   /* op_state */
OP_API int32_t   op_has_video (op_player *p);
OP_API int32_t   op_has_audio (op_player *p);
OP_API int32_t   op_is_music  (op_player *p);
OP_API int32_t   op_is_stream (op_player *p);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ============================================================================
 *  最小使用示例（伪代码，两种语言的形态）
 * ============================================================================
 *
 *  // ---- 桌面 / 任意 C++ 程序 ----
 *  op_player* p = op_create();
 *  op_set_event_cb(p, on_event, nullptr);
 *  op_set_video_cb(p, on_frame, nullptr);          // 回调里上传纹理
 *  op_set_audio_format(p, 48000, 2);
 *  op_open_options o = {0}; o.path = "a.mp4"; o.fd = -1;
 *  op_open(p, &o);
 *  // 音频线程：
 *  //   while (running) {
 *  //       int n = op_audio_read(p, buf, sizeof(buf));
 *  //       if (n > 0) device.write(buf, n); else sleep_ms(3);
 *  //   }
 *
 *  // ---- 鸿蒙（ArkTS + NAPI）----
 *  //   1) XComponent 拿到 OH_NativeWindow → eglCreateWindowSurface
 *  //   2) 视频回调里做三平面 glTexSubImage2D（和现在 VideoWidget::paintGL 同一套）
 *  //   3) AudioRenderer 的写回调里（JS 线程）调 op_audio_read，把 PCM 写回去
 *  //   4) 文件选择器给的 URI → fileIo.openSync 拿 fd → op_open(fd)
 *
 * ============================================================================
 *  和现有 C++ 内核的对应关系（迁出后照着接）
 * ============================================================================
 *
 *   op_create / op_destroy   →  new PlayerCore / delete PlayerCore（含 PlayerContext）
 *   op_set_event_cb          →  PlayerCore::setEventCallBack
 *   op_set_video_cb          →  DemuxThread::openFile 里的 PaintFrame 回调
 *   op_open                  →  CMD_OPEN（obj = 路径 + 回调 + IAudioOutput）
 *   op_play                  →  CMD_PAUSE(arg1=0)
 *                               （**注意**：core 里的 CMD_PLAY 是空实现，别去"补"它 ——
 *                                实际播放/暂停只有 CMD_PAUSE 一条路，arg1 表示目标状态）
 *   op_pause / op_stop       →  CMD_PAUSE(arg1=1) / CMD_STOP
 *   op_seek_ms               →  CMD_SEEK
 *   op_step_frame            →  CMD_STEP_FORWARD / CMD_STEP_BACKWARD
 *   op_set_speed             →  CMD_SET_SPEED
 *   op_audio_read            →  新增：内部 IAudioOutput 换成环形缓冲，对外拉
 *   EVT_* → op_event         需要一层映射（arg1/arg2 的打包要拆成字段）
 */

#endif /* OP_PLAYER_H */
