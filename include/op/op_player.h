/*
 * ============================================================================
 *  op_player.h —— 播放内核的 C 接口（唯一对外面）
 * ============================================================================
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
#define OP_API_VERSION_MAJOR 1
#define OP_API_VERSION_MINOR 0

//唯一支持的格式
#define OP_PIX_FMT_YUV420P 0

/* --------------------------------------------------------------- 平台档位 */
/*
 * SDK 在**编译时**按目标平台选定一档参数（队列深度 / 历史队列 / 逐帧），
 * 运行时用这两个函数查询。壳靠它决定"逐帧按钮要不要显示"，内核靠它决定
 * "历史队列开不开" —— 两边必须是同一个答案，所以别在壳里自己写 #ifdef，
 * 调 op_platform() / op_is_mobile()，单一真源在 core/platform.h。
 *
 *   op_is_mobile() == 0（Windows / Linux 桌面）
 *       视频帧队列 16、历史 48 帧、逐帧可用、视频包队列 100
 *   op_is_mobile() == 1（Android / 鸿蒙）
 *       视频帧队列 8、无历史、逐帧关闭（op_step_frame 返回 OP_ERR_UNSUPPORTED）、
 *       视频包队列 50
 *
 * 桌面 Linux 算桌面档；没识别出来的平台也按桌面档（宁可多占内存，不少功能）。
 * 枚举值只会往后追加，已有值的含义不会变，所以按数值比较是安全的 ——
 * 但判断"是不是移动端"请直接用 op_is_mobile()。
 */
typedef enum op_platform
{
    OP_PLATFORM_UNKNOWN   = 0,      /* 没识别出来（按桌面档跑） */
    OP_PLATFORM_WINDOWS   = 1,
    OP_PLATFORM_LINUX     = 2,      /* 桌面 Linux（ctest / 开发机） */
    OP_PLATFORM_ANDROID   = 3,
    OP_PLATFORM_HARMONYOS = 4       /* OpenHarmony / HarmonyOS NEXT */
} op_platform_t;

OP_API int32_t op_platform(void);   /* 返回 op_platform_t；进程常量，任意线程可调 */
OP_API int32_t op_is_mobile(void);  /* 1 = 移动档（Android / 鸿蒙），0 = 桌面档 */

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

    /*
     * **点事件**必须带出现在事件里的数值，不能靠回调里再去查询：
     *   POSITION  / SEEK_DONE / COMPLETED → position_ms
     *
     * 为什么不能去掉它：查询接口返回的是"**此刻**的位置"，而回调往往要排队到
     * 壳的 UI 线程（Qt 的 QueuedConnection、鸿蒙的 threadsafe function）。
     * 等 lambda 真正执行时，POSITION 早就被下一条覆盖了；SEEK_DONE 的落点更是
     * 一进回放就被新位置顶掉。壳拿到的会是"另一个时刻的值"。
     *
     * 文件级属性（width/height/duration/has_video/has_audio/is_stream）不放进事件，
     * 用 op_get_width / op_get_height / op_duration_ms / op_has_video ... 查询即可：
     * 它们在一次打开期间不变，什么时候读都一样。
     */
    int32_t     type;           /* op_event_type */
    int32_t     state;          /* OP_EVENT_STATE */
    int32_t     code;           /* OP_EVENT_ERROR：op_result 或 FFmpeg 错误码 */
    int64_t     position_ms;
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

/*
 * 帧的内存助手（可选，不改变"回调期间借用"的契约）。
 *
 * 大部分壳在回调里当场上传纹理就够了；但有些平台必须跨线程交付
 * （Qt 的 GUI 线程、鸿蒙的 JS 线程），而 op_frame 回调一返回就失效，
 * 所以需要"克隆一份带走"。这三个函数就是为此提供的：
 *
 *   op_frame_alloc(dataSize)  分配一个 op_frame；dataSize>0 时同时分配 data
 *                             （调用方自己填 linesize/宽高/内容）
 *   op_frame_clone(src)       深拷贝一份（data 一起拷），用完必须 op_frame_free
 *   op_frame_free(&frame)     释放 clone/alloc 出来的帧；传 nullptr 安全
 *
 * 只对"自己 alloc/clone 出来的帧"调用 free —— **绝不能**释放回调里那个 frame。
 *
 * 注意：op_frame_alloc 的 dataSize 没有默认值，C 里必须显式传（传 0 表示
 * 只分配结构体，data 由调用方自己 malloc）。
 */
OP_API op_frame* op_frame_alloc(int32_t dataSize);
OP_API op_frame* op_frame_clone(const op_frame* src);
OP_API void      op_frame_free(op_frame** frame);
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
 *
 * fd 的约定（**很重要**）：
 *   - fd 归调用方所有，SDK **不会** close 它；但播放期间必须一直开着 ——
 *     demux 线程整个播放过程都在从这个 fd 读字节，关早了就是 EBADF / 花屏。
 *   - SDK 会移动 fd 的读写位置（内部 seek 做格式探测、seek 定位）。播放期间
 *     不要在别处用同一个 fd 读写；要并行读，请 dup() 一份。
 *   - fd_offset：从第几个字节开始（容器嵌在别的文件里、或 picker 给了偏移）。
 *   - fd_size：可读长度；0 = 自动探测（普通文件 = 文件大小 − fd_offset）。
 *   - 只支持可 seek 的普通文件。管道/套接字也能打开，但没有时长、不能 seek ——
 *     这些功能依赖"能跳到指定位置"。
 *   - path 在有 fd 时变成**可选的扩展名提示**（"a.mp4" 能让格式探测少猜几次）；
 *     给 nullptr 也没问题。
 */
typedef struct op_open_options
{
    const char *path;           /* 本地路径或 URL（rtsp:// rtmp:// http:// ...）；有 fd 时可为 NULL */
    int32_t     fd;             /* 文件描述符；-1 表示不用 */
    int64_t     fd_offset;      /* fd 内的起始偏移，通常是 0 */
    int64_t     fd_size;        /* fd 可读长度，0 = 由 SDK 探测（普通文件） */
    int32_t     open_timeout_ms;/* 网络流打开超时，0 = 默认（当前 15s） */
} op_open_options;

/* ================================================================== */
/*  生命周期                                       */
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
/*  打开 / 播放控制                                                     */
/* ================================================================== */

OP_API op_result op_open(op_player *p, const op_open_options *opt );
OP_API op_result op_play (op_player *p);
OP_API op_result op_pause(op_player *p);
OP_API op_result op_stop (op_player *p);      /* 回到 IDLE，p 仍可复用 */
OP_API op_result op_seek_ms(op_player *p, int64_t ms);


/* 逐帧：dir 取 +1（下一帧）或 -1（上一帧，靠回填）。纯音频/流媒体返回 OP_ERR_UNSUPPORTED。
 在逐帧期间，壳的音频播放需要调用方设为静音
 移动档（op_is_mobile()==1）没有历史队列，**整条逐帧关闭**，这里直接返回
 OP_ERR_UNSUPPORTED —— 壳应该用 op_is_mobile() 提前把按钮灰掉，别等点了才报错。 */
OP_API op_result op_step_frame(op_player *p, int32_t dir);
//退出逐帧
OP_API op_result op_exit_frame_step(op_player *p);

/* 倍速，用 ×10 的整数避免 C 接口里出现 double：5~20 表示 0.5x ~ 2.0x。 */
OP_API op_result op_set_speed(op_player *p, int32_t speed_x10);


/* ================================================================== */
/*  音频：**拉模式**                                   */
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

/*
 * 壳回传"已经读走、但设备里还没播出去"的毫秒数
 *
 * 为什么需要它：`op_audio_latency_ms()` 要算"总共还压着多少"，而积压同时躺在两处 ——
 * SDK 的环形缓冲里（自己知道）+ 设备缓冲里（只有壳知道）。不报这一项，音频时钟
 * 会超前，视频会误判"自己落后"然后疯狂丢帧。
 *
 * 怎么算（各平台一行）：
 *   Qt     : bufferSize() − bytesFree()  换算成毫秒
 *   鸿蒙   : getAudioTime() / getWrittenDataSize() 之类的积压量
 *   安卓   : 已写字节 − getPlaybackHeadPosition() × frameSize，再换算成毫秒
 *
 * 什么时候调：设备写回调里顺手报一次即可（每隔几十毫秒一次足够）。
 *   - SDK 按"锚点 + 线性外推"使用这个值，所以**不需要**报得很密；
 *   - seek / flush 设备之后立刻报 0（SDK 在 clear() 后会削掉迟到的旧上报，
 *     但仍建议主动报，响应更快）；
 *   - 暂停时不用报，SDK 会冻结外推。
 */
OP_API op_result op_audio_set_device_pending_ms(op_player *p, int32_t ms);

/* 返回实际读到的字节数；0 = 暂时没数据；< 0 = op_result 错误码 */
OP_API int32_t   op_audio_read(op_player *p, void *buf, int32_t capacity);

/* 缓冲里还有多少字节（调用方据此做回压 / 判断是否该等一下） */
OP_API int32_t   op_audio_pending(op_player *p);

/* 已缓冲但还没播的时长（毫秒）——音频时钟的延迟补偿量 */
OP_API int64_t   op_audio_latency_ms(op_player *p);

/* ================================================================== */
/*  查询（线程安全，任意线程可调）                                        */
/* ================================================================== */

OP_API int64_t   op_position_ms(op_player *p);
OP_API int64_t   op_duration_ms(op_player *p);
OP_API int32_t   op_state_of  (op_player *p);   /* op_state */
OP_API int32_t   op_has_video (op_player *p);
OP_API int32_t   op_has_audio (op_player *p);
OP_API int32_t   op_is_music  (op_player *p);
OP_API int32_t   op_is_stream (op_player *p);
/* 1 = 容器本身是纯音频（mp3/flac/aac…，核心对它关闭逐帧/精确 seek）；
 * 注意和 op_is_music 语义不同：mp4 里只放一条音轨时 op_is_music=1，这里仍是 0。*/
OP_API int32_t   op_is_audio_media(op_player *p);
OP_API int32_t   op_get_width (op_player *p);
OP_API int32_t   op_get_height(op_player *p);
OP_API int32_t   op_get_speedX10(op_player *p);


#ifdef __cplusplus
} /* extern "C" */
#endif


#endif /* OP_PLAYER_H */
