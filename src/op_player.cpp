/*
 * op_player.cpp —— C 接口的门面（T1 骨架 + T2 事件映射）
 *
 * 这一层只干三件事：
 *   1. 把不透明句柄 op_player* 变成"活着的 PlayerCore + PlayerContext"；
 *   2. 把 C 的调用翻译成 CMD_* 消息投进 core 的消息队列；
 *   3. 把 core 的 EVT_* 翻译成 op_event，同时把"最近已知状态"缓存下来给查询接口读。
 *
 * 为什么用 pimpl，而不是把 PlayerCore 直接暴露给调用方：
 *   PlayerCore 里有 std::function / std::mutex / std::thread，它们的布局跟着 STL
 *   实现走（MSVC 和 MinGW 就不一样），何况 core 头文件还会把 libav* 的类型拖出来。
 *   op_player 是个不透明指针，调用方只把它当地址用，所以换编译器、换 STL、
 *   换 FFmpeg 版本都不会让调用方编不过 —— 这才是"C 接口"的意义。
 *
 * 当前状态（2026-11-21）：
 *   T1   create / destroy / version / 回调注册 / open / play / pause / stop /
 *        seek / step / speed        —— 已实现
 *   T2   EVT_* → op_event + 状态缓存 + 查询接口 —— 已实现
 *   T3   视频帧（videoCB）            —— 已连通，等真机/真文件验收
 *   T4   音频 pull（环形缓冲）        —— 仍是桩
 *   T5   fd 打开                      —— 仍是桩
 */

#include "op/op_player.h"

#include <atomic>
#include <cstdlib>
#include <new>
#include <string>

#include "core/log.h"
#include "core/messagequeue.h"
#include "core/playercore.h"
#include "core/playercontext.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

namespace
{

/*
 * 门面私有消息 id：把"在消息循环线程上发一个事件"变成一条消息。
 *
 * 为什么需要它：STATE 这种事件内核里没有数据源（EVT_STATE_CHANGED 从没人发过），
 * 只能由门面合成；但按契约，事件必须在**消息循环线程**上触发，而调用 op_play()
 * 的往往是壳的 UI 线程。core 的 PlayerCore 对未识别的 what 会原样转回事件回调
 * （见 playercore.cpp 的 default 分支），于是这里投一条私有 id 的消息过去，
 * 就等于请求"在消息循环线程上执行"——把投递当线程切换用。
 */
enum : int { OP_MSG_EMIT = 0x4F50 };   // 'OP'

/*
 * 统一处理"这个接口还没实现"。
 *
 * C 接口最怕的是**静默成功** —— 调用方调了 op_play、拿回 OP_OK，然后一直等画面，
 * 最后以为是自己的 bug。所以桩必须返回明确的 OP_ERR_UNSUPPORTED，并且打一行日志
 * 指出到底是谁没实现。
 */
op_result notImplemented(const char *what)
{
    logWarn() << "[op_sdk] 尚未实现：" << what;
    return OP_ERR_UNSUPPORTED;
}

std::function<void(AVFrame*)> videoCB(op_video_cb cb, void* user_data) {
    return [cb, user_data](AVFrame* avFrame) {
        if (!avFrame) return;

        /*
         * 这个 lambda 拿到的是"所有权"：core 的 painter 交出来之后不再管
         * （VideoRenderThread::renderFrame 里 clone 完就撒手，repaintPts 里
         *  命中的那帧也直接 paint 不释放）。所以**每条出口都必须 av_frame_free**，
         * 包括 cb 为空、分配失败这些早退路径 —— 漏一条就是每帧漏一个 AVFrame。
         */
        if (!cb || !avFrame->data[0]) {
            av_frame_free(&avFrame);
            return;
        }

        const AVPixelFormat fmt = (AVPixelFormat)avFrame->format;
        const int buffer_size = av_image_get_buffer_size(fmt, avFrame->width,
                                                         avFrame->height, 1);
        if (buffer_size <= 0) {          // 格式/尺寸非法，别拿负数去 malloc
            av_frame_free(&avFrame);
            return;
        }

        // 分配 op_frame 结构体本身（calloc：linesize 这种数组要先清零）
        op_frame* opFrame = (op_frame*)calloc(1, sizeof(op_frame));
        if (!opFrame) {
            av_frame_free(&avFrame);
            return;
        }

        opFrame->data = (uint8_t*)malloc((size_t)buffer_size);
        if (!opFrame->data) {
            free(opFrame);
            av_frame_free(&avFrame);
            return; // 内存不足
        }

        // 拷贝数据：将多平面的 AVFrame 数据紧凑地拷贝到连续的 data 缓冲区中
        if (av_image_copy_to_buffer(
                opFrame->data, buffer_size,
                (const uint8_t* const*)avFrame->data, avFrame->linesize,
                fmt, avFrame->width, avFrame->height, 1) < 0) {
            free(opFrame->data);
            free(opFrame);
            av_frame_free(&avFrame);
            return;
        }
        opFrame->size = buffer_size;

        /*
         * linesize 必须按**这块紧凑 buffer 的布局**重新算，不能抄源 AVFrame 的：
         * 源的 linesize 是按 16/32 字节对齐过的（比如宽 854 → 864），而这块 buffer
         * 是 align=1 紧凑的（854）。抄源值等于告诉消费方"按 864 读 854 的行"，
         * 逐行上传就会读越界、画面斜切。
         */
        av_image_fill_linesizes(opFrame->linesize, fmt, avFrame->width);

        //填充元数据
        opFrame->format  = (int32_t)fmt;
        opFrame->pts_ms  = avFrame->pts;
        opFrame->width   = avFrame->width;
        opFrame->height  = avFrame->height;

        //触发回调
        cb(user_data, opFrame);

        // SDK 同步调用，回调返回后立刻释放。调用者必须在回调内把数据拷贝走，不能只存指针。
        free(opFrame->data);
        free(opFrame);
        av_frame_free(&avFrame);
    };
}

} // namespace

/*
 * 不透明句柄的真身。
 *
 * 成员顺序不是随手排的：析构按**声明逆序**进行，而 PlayerCore 的析构会走 stop()
 * → join 所有线程，这期间 DemuxThread 还会读 PlayerContext（DemuxThread::setPause
 * 里读 getIsSeeking）。所以 ctx 必须声明在 core 之前，保证它比 core 活得久。
 */
struct op_player
{
    PlayerContext ctx;
    PlayerCore    core;

    /* ------------------------------------------------------------------
     * 最近已知状态（T2）
     *
     * 只有消息循环线程写；查询接口在任意线程读，所以用原子量而不是锁。
     * **不要**改成"查询时现读 PlayerContext"：那是异步广播副本，比真实状态慢
     * 一个完整的消息往返（上游因此吃过五种 bug），拿它当状态源会读出过期值。
     * ------------------------------------------------------------------ */
    std::atomic<int64_t> durationMs{0};
    std::atomic<int64_t> positionMs{0};
    std::atomic<int32_t> state{OP_STATE_IDLE};
    std::atomic<int32_t> width{0};
    std::atomic<int32_t> height{0};
    std::atomic<int32_t> hasVideo{0};
    std::atomic<int32_t> hasAudio{0};
    std::atomic<int32_t> isStream{0};

    op_event_cb eventCb   = nullptr;
    void       *eventUser = nullptr;
    op_video_cb videoCb   = nullptr;
    void       *videoUser = nullptr;
    op_log_cb   logCb     = nullptr;
    void       *logUser   = nullptr;
};

namespace
{

/* 把一条事件交给用户回调。只允许在消息循环线程上调用。 */
void deliver(op_player *p, op_event &ev)
{
    if (p->eventCb) p->eventCb(p->eventUser, &ev);
}

/* 在消息循环线程上发一个事件：任意线程可调，顺序就是入队顺序。 */
void emitOnLoop(op_player *p, const op_event &ev)
{
    /*
     * 只发"纯数据"事件（STATE 这类 message 为空的）。
     * 带 message 的事件（ERROR）必须直接 deliver：message 指向的 std::string
     * 活不过一次入队，攒到下一个消息循环时早就悬空了。
     */
    op_event *heap = new op_event(ev);
    p->core.postMessage(OP_MSG_EMIT, 0, 0, heap,
                        [](void *o) { delete static_cast<op_event *>(o); });
}

/* 更新状态缓存；真的变了才发事件（否则每秒会刷一堆重复 STATE）。 */
void setState(op_player *p, int32_t next)
{
    if (p->state.exchange(next) == next) return;

    op_event ev{};
    ev.type  = OP_EVENT_STATE;
    ev.state = next;
    emitOnLoop(p, ev);
}

/* 是否处于"能接受播放控制"的状态：还没 open / 已经 stop 时不算。 */
bool isReady(const op_player *p)
{
    switch (p->state.load()) {
    case OP_STATE_OPENING:
    case OP_STATE_READY:
    case OP_STATE_PLAYING:
    case OP_STATE_PAUSED:
    case OP_STATE_ENDED:
        return true;
    default:
        return false;
    }
}

/* core 事件 → op_event + 缓存。**跑在消息循环线程上。** */
void onCoreMessage(op_player *p, PlayerMessage &msg)
{
    switch (msg.what)
    {
    case OP_MSG_EMIT:
        // 门面自己投过来的事件（已经在消息循环线程上），obj 由消息循环统一释放
        if (msg.obj) {
            op_event *ev = static_cast<op_event *>(msg.obj);
            deliver(p, *ev);
        }
        return;

    case PlayerEvent::EVT_PREPARED: {
        const int32_t durMs = msg.arg1;                      // arg1 = 时长 ms
        const int32_t w = (msg.arg2 >> 16) & 0xFFFF;         // arg2 = (宽<<16)|高
        const int32_t h = msg.arg2 & 0xFFFF;

        p->durationMs.store(durMs);
        p->width.store(w);
        p->height.store(h);
        p->positionMs.store(0);

        /*
         * 有无音视频 / 是不是网络流：core 在 openFile 末尾是**先推 CTX_**、
         * 再推 EVT_PREPARED 的，而 CTX_* 不转给门面。所以这里读 PlayerContext
         * 的快照 —— 同一条线程、同一个顺序，此刻它已经是最新值。
         */
        const int32_t hasV = p->ctx.getHasVideo() ? 1 : 0;
        const int32_t hasA = p->ctx.getHasAudio() ? 1 : 0;
        p->hasVideo.store(hasV);
        p->hasAudio.store(hasA);
        p->isStream.store(p->ctx.getIsStreamMedia() ? 1 : 0);

        op_event ev{};
        ev.type        = OP_EVENT_OPENED;
        ev.duration_ms = durMs;
        ev.width       = w;
        ev.height      = h;
        ev.has_video   = hasV;
        ev.has_audio   = hasA;
        ev.is_stream   = p->isStream.load();
        deliver(p, ev);

        /*
         * 报 PLAYING 而不是 READY：内核 open 成功后自己 setPause(false)、
         * 解码/渲染/音频线程立刻跑起来 —— 它确实已经在播了，报 READY 是骗人。
         * 想"停在第一帧"，要在 OPENED 回调里调 op_pause。
         */
        setState(p, OP_STATE_PLAYING);
        return;
    }

    case PlayerEvent::EVT_POSITION: {
        p->positionMs.store(msg.arg1);                       // 约 150ms 一次
        op_event ev{};
        ev.type        = OP_EVENT_POSITION;
        ev.position_ms = msg.arg1;
        deliver(p, ev);
        return;
    }

    case PlayerEvent::EVT_SEEK_DONE: {
        // arg1/arg2 是 int64 的**高/低 32 位**（见 demuxthread.cpp 的 doSeek）
        const int64_t pos = static_cast<int64_t>(
            (static_cast<uint64_t>(static_cast<uint32_t>(msg.arg1)) << 32)
            | static_cast<uint32_t>(msg.arg2));
        p->positionMs.store(pos);

        op_event ev{};
        ev.type        = OP_EVENT_SEEK_DONE;
        ev.position_ms = pos;
        deliver(p, ev);
        return;
    }

    case PlayerEvent::EVT_COMPLETED: {
        op_event ev{};
        ev.type        = OP_EVENT_COMPLETED;
        ev.position_ms = p->durationMs.load();
        deliver(p, ev);
        setState(p, OP_STATE_ENDED);
        return;
    }

    case PlayerEvent::EVT_ERROR: {
        op_event ev{};
        ev.type    = OP_EVENT_ERROR;
        ev.code    = msg.arg1;                               // op_result 或 FFmpeg 码
        ev.message = msg.obj ? static_cast<std::string *>(msg.obj)->c_str() : "";
        deliver(p, ev);                                      // message 只在回调期间有效

        // 打开/解码失败后回到 IDLE，别让查询接口一直报 PLAYING
        p->durationMs.store(0);
        p->positionMs.store(0);
        setState(p, OP_STATE_IDLE);
        return;
    }

    case PlayerEvent::EVT_BUFFERING: {
        op_event ev{};
        ev.type      = OP_EVENT_BUFFERING;
        ev.has_video = msg.arg1 ? 1 : 0;                     // 该字段在这里表示 1/0
        deliver(p, ev);
        return;
    }

    default:
        /*
         * EVT_STATE_CHANGED（内核从没发过）、EVT_DISABLEBTN / EVT_CLEARSCREEN /
         * EVT_PLAYNEXT（上游 UI 专用）—— 全部丢掉：状态由门面合成，
         * 那几个按钮/清屏动作该由壳自己按事件语义决定。
         */
        return;
    }
}

} // namespace

/* ================================================================== */
/*  生命周期                                                           */
/* ================================================================== */

OP_API op_player *op_create(void)
{
    op_player *p = new (std::nothrow) op_player();
    if (!p) {
        logError() << "[op_sdk] op_create: 内存不足";
        return nullptr;
    }

    p->core.setPlayerContext(&p->ctx);

    /*
     * 事件回调必须在 start() 之前注册：
     *   - 晚注册会漏掉启动早期的事件；
     *   - core 的 m_callBack 为空时它内部是直接 operator() 的（守卫已补，但
     *     "永远不为空"才是干净的状态）。
     * 用户没调 op_set_event_cb 时，deliver() 里的判空让事件只更新缓存、不回调。
     */
    p->core.setEventCallBack([p](PlayerMessage &msg) { onCoreMessage(p, msg); });
    p->core.start();

    return p;
}

OP_API void op_destroy(op_player *p)
{
    if (!p) return;

    /*
     * 显式 stop() 再 delete：stop() 会 join 消息循环（→ 此后不再有事件回调）以及
     * demux / 解码 / 渲染 / 音频线程，返回后进程里没有线程还拿着 p。
     * ~PlayerCore 里还会再调一次 stop()，那是幂等的（看 m_stopped）。
     */
    p->core.stop();
    delete p;
}

OP_API op_result op_version(int32_t *major, int32_t *minor)
{
    // 两个参数都可以是 NULL：调用方常常只关心 major
    if (major) *major = OP_API_VERSION_MAJOR;
    if (minor) *minor = OP_API_VERSION_MINOR;
    return OP_OK;
}

OP_API op_result op_set_event_cb(op_player *p, op_event_cb cb, void *user)
{
    if (!p) return OP_ERR_INVALID_ARG;
    /*
     * 只记指针：core 侧的回调在 op_create 里已经注册好了，映射逻辑永远在跑，
     * 这样"先 open 后注册回调"也不会漏掉事件（只是那之前的事件只进缓存）。
     */
    p->eventCb   = cb;          // cb == NULL 就是取消订阅
    p->eventUser = user;
    return OP_OK;
}

OP_API op_result op_set_video_cb(op_player *p, op_video_cb cb, void *user)
{
    if (!p) return OP_ERR_INVALID_ARG;
    p->videoCb   = cb;
    p->videoUser = user;
    /*
     * 注意：painter 是在 op_open 那一刻按当时的 videoCb 打包装进 CMD_OPEN_OBJ 的，
     * 所以"播放中换视频回调"要重新 open 才生效（帧回调的打包里有 cb 的拷贝）。
     */
    return OP_OK;
}

OP_API op_result op_set_log_cb(op_player *p, op_log_cb cb, void *user)
{
    if (!p) return OP_ERR_INVALID_ARG;
    p->logCb   = cb;
    p->logUser = user;
    // TODO(T1+)：core/log.h 现在是直接写 stderr 的（没留 sink 钩子）。
    // 要么给 core::LogLine 加一个可替换的 sink，要么这个接口就只保留字段。
    return OP_OK;
}

/* ================================================================== */
/*  打开 / 播放控制                                                     */
/* ================================================================== */

OP_API op_result op_open(op_player *p, const op_open_options *opt)
{
    if (!p) return OP_ERR_INVALID_ARG;
    if (!opt) return OP_ERR_INVALID_ARG;

    /*
     * fd 打开是 T5 的活：core 的 CMD_OPEN 只认 CMD_OPEN_OBJ 里的 path，arg1 里的
     * fd 根本没人读。以前把 fd 塞进 arg1 = 静默丢弃，调用方只能看到"打开失败"，
     * 所以这里明说"还不支持"。
     */
    if (opt->fd >= 0) return OP_ERR_UNSUPPORTED;

    /*
     * path 和 fd 二选一（见头文件）。两个都没给时这里必须判空 ——
     * 直接把 nullptr 喂给 std::string 的构造函数是未定义行为，多半当场崩。
     */
    if (!opt->path || opt->path[0] == '\0') return OP_ERR_INVALID_ARG;

    // 上一个文件的缓存不能带过来（时长/宽高/音视频标志/流标志）
    p->durationMs.store(0);
    p->positionMs.store(0);
    p->width.store(0);
    p->height.store(0);
    p->hasVideo.store(0);
    p->hasAudio.store(0);
    p->isStream.store(0);

    /*
     * 视频回调可以为空（纯音频文件根本没有帧要送）；videoCB 内部对空 cb 会直接
     * 释放 AVFrame，所以这里不需要"必须先注册视频回调"这种前置条件。
     */
    CMD_OPEN_OBJ *obj = new CMD_OPEN_OBJ(std::string(opt->path),
                                         videoCB(p->videoCb, p->videoUser), nullptr);
    p->core.postMessage(PlayerCmd::CMD_OPEN, 0, 0, obj,
                        [](void *o) { delete static_cast<CMD_OPEN_OBJ *>(o); });

    // 受理就算 OPENING：真结果由后续 OPENED / ERROR 事件落地
    setState(p, OP_STATE_OPENING);
    return OP_OK;
}

OP_API op_result op_play(op_player *p)
{
    if (!p) return OP_ERR_INVALID_ARG;
    if (!isReady(p)) return OP_ERR_NOT_READY;
    // core 的 CMD_PLAY 是空实现，播放/暂停只有 CMD_PAUSE 一条路（arg1=目标状态）
    p->core.postMessage(PlayerCmd::CMD_PAUSE, 0);
    setState(p, OP_STATE_PLAYING);
    return OP_OK;
}

OP_API op_result op_pause(op_player *p)
{
    if (!p) return OP_ERR_INVALID_ARG;
    if (!isReady(p)) return OP_ERR_NOT_READY;
    p->core.postMessage(PlayerCmd::CMD_PAUSE, 1);
    setState(p, OP_STATE_PAUSED);
    return OP_OK;
}

OP_API op_result op_stop(op_player *p)
{
    if (!p) return OP_ERR_INVALID_ARG;
    p->core.postMessage(PlayerCmd::CMD_STOP);   // 异步：消息排在队尾
    p->durationMs.store(0);
    p->positionMs.store(0);
    p->width.store(0);
    p->height.store(0);
    p->hasVideo.store(0);
    p->hasAudio.store(0);
    p->isStream.store(0);
    setState(p, OP_STATE_IDLE);
    return OP_OK;
}

OP_API op_result op_seek_ms(op_player *p, int64_t ms)
{
    if (!p) return OP_ERR_INVALID_ARG;
    if (ms < 0) return OP_ERR_INVALID_ARG;
    if (!isReady(p)) return OP_ERR_NOT_READY;

    // core 的 CMD_SEEK 期望 int64 拆成高/低 32 位（playercore.cpp 里再拼回去）
    const uint32_t hi = static_cast<uint32_t>(static_cast<uint64_t>(ms) >> 32);
    const uint32_t lo = static_cast<uint32_t>(ms & 0xFFFFFFFF);
    p->core.postMessage(PlayerCmd::CMD_SEEK,
                        static_cast<int>(hi), static_cast<int>(lo));
    return OP_OK;
}

OP_API op_result op_step_frame(op_player *p, int32_t dir)
{
    if (!p) return OP_ERR_INVALID_ARG;
    if (dir != 1 && dir != -1) return OP_ERR_INVALID_ARG;
    if (!isReady(p)) return OP_ERR_NOT_READY;
    // 纯音频没有"帧位置"，直播流的位置也不由客户端决定 —— 都别受理
    if (!p->hasVideo.load() || p->isStream.load()) return OP_ERR_UNSUPPORTED;

    p->core.postMessage(dir == 1 ? PlayerCmd::CMD_STEP_FORWARD
                                 : PlayerCmd::CMD_STEP_BACKWARD);
    return OP_OK;
}

OP_API op_result op_exit_frame_step(op_player *p)
{
    if (!p) return OP_ERR_INVALID_ARG;
    if (!isReady(p)) return OP_ERR_NOT_READY;
    p->core.postMessage(PlayerCmd::CMD_STEP_STOP);
    return OP_OK;
}

OP_API op_result op_set_speed(op_player *p, int32_t speed_x10)
{
    if (!p) return OP_ERR_INVALID_ARG;
    // 0.5x~2.0x：越界是调用方的错，别悄悄钳住（core 侧 AudioThread 会钳，但那不该是契约）
    if (speed_x10 < 5 || speed_x10 > 20) return OP_ERR_INVALID_ARG;
    if (!isReady(p)) return OP_ERR_NOT_READY;
    // 直播流不能改倍速（core 也会挡，但挡了不回报，接口这边先说清楚）
    if (p->isStream.load()) return OP_ERR_UNSUPPORTED;

    p->core.postMessage(PlayerCmd::CMD_SET_SPEED, speed_x10);   // arg1 就是 ×10 的值
    return OP_OK;
}

/* ================================================================== */
/*  音频：拉模式（T4，仍是桩）                                          */
/* ================================================================== */

OP_API op_result op_set_audio_format(op_player *p, int32_t sample_rate, int32_t channels)
{
    if (!p) return OP_ERR_INVALID_ARG;
    if (sample_rate <= 0 || channels <= 0) return OP_ERR_INVALID_ARG;
    /*
     * TODO(T4)：只记下来。设备（和 swr 的重采样目标）是在 open 那一刻配好的，
     * 所以这个值必须在 op_open 之前生效 —— 这也是头文件里写"必须提前调"的原因。
     */
    return notImplemented("op_set_audio_format");
}

OP_API int32_t op_audio_read(op_player *p, void *buf, int32_t capacity)
{
    if (!p || !buf || capacity <= 0) return OP_ERR_INVALID_ARG;
    /*
     * TODO(T4)：从内部环形缓冲取数据（由 IAudioOutput 的 write() 灌进去）。
     * 返回 0 表示"暂时没数据"，调用方睡 2~5ms 再来 —— 现在返回 UNSUPPORTED，
     * 好让它知道是"没实现"而不是"没数据"。
     */
    return static_cast<int32_t>(notImplemented("op_audio_read"));
}

OP_API int32_t op_audio_pending(op_player *p)
{
    if (!p) return OP_ERR_INVALID_ARG;
    // TODO(T4)：返回环形缓冲里可读的字节数。桩返回 0（= 空），至少不会让调用方忙转。
    notImplemented("op_audio_pending");
    return 0;
}

OP_API int64_t op_audio_latency_ms(op_player *p)
{
    if (!p) return OP_ERR_INVALID_ARG;
    // TODO(T4)：缓冲里还没被取走的时长。桩返回 0，音画同步会偏，但不会崩。
    notImplemented("op_audio_latency_ms");
    return 0;
}

/* ================================================================== */
/*  查询（读缓存，任意线程可调）                                        */
/* ================================================================== */

OP_API int64_t op_position_ms(op_player *p)
{
    return p ? p->positionMs.load() : 0;
}

OP_API int64_t op_duration_ms(op_player *p)
{
    return p ? p->durationMs.load() : 0;
}

OP_API int32_t op_state_of(op_player *p)
{
    return p ? p->state.load() : OP_STATE_IDLE;
}

OP_API int32_t op_has_video(op_player *p)
{
    return p ? p->hasVideo.load() : 0;
}

OP_API int32_t op_has_audio(op_player *p)
{
    return p ? p->hasAudio.load() : 0;
}

OP_API int32_t op_is_stream(op_player *p)
{
    return p ? p->isStream.load() : 0;
}

OP_API int32_t op_is_music(op_player *p)
{
    if (!p) return 0;
    // 纯音频 = 有音轨且没有视频轨（OPENED 之后才有意义，之前都是 0）
    return (p->hasAudio.load() && !p->hasVideo.load()) ? 1 : 0;
}
