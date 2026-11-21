/*
 * C 接口冒烟测试（T1 + T2 验收）
 *
 * 覆盖：
 *   1. version / create / 回调注册 / destroy（含空指针、连开连关）
 *   2. 播放控制的状态前置条件（没 open 时 play/seek/step 返回 OP_ERR_NOT_READY）
 *   3. op_open 的参数校验（空 opt / path 为空 / fd 还不支持）
 *   4. **端到端**：open 一个不存在的文件 → 收到 OP_EVENT_ERROR（message 非空）
 *      → 状态回到 IDLE。这条同时验证 T2 的映射链路和"回调在 SDK 线程上"的契约。
 *   5. 仍是桩的音频 pull（T4）
 *
 * 不需要任何媒体文件、不出声、不上屏，所以和 test_framequeue 一样能 headless 跑。
 */
#include "op/op_player.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  [FAIL] %s (line %d)\n", #cond, __LINE__);      \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

/* ---------------------------------------------------------------- 事件捕获 */

/*
 * 回调是在 SDK 的消息循环线程上触发的，所以捕获侧必须自己加锁；
 * 顺便把"回调发生在哪个线程"也记下来 —— 这是接口契约的一部分。
 */
struct Recorded
{
    int32_t     type = 0;
    int32_t     state = 0;
    int32_t     code = 0;
    int32_t     width = 0;
    int32_t     height = 0;
    int32_t     has_video = 0;
    int32_t     has_audio = 0;
    int32_t     is_stream = 0;
    int64_t     duration_ms = 0;
    int64_t     position_ms = 0;
    std::string message;
};

struct Capture
{
    std::mutex                  m;
    std::condition_variable     cv;
    std::vector<Recorded>       events;
    std::vector<std::thread::id> threads;

    void push(const op_event *ev)
    {
        std::lock_guard<std::mutex> lock(m);
        Recorded r;
        r.type        = ev->type;
        r.state       = ev->state;
        r.code        = ev->code;
        r.width       = ev->width;
        r.height      = ev->height;
        r.has_video   = ev->has_video;
        r.has_audio   = ev->has_audio;
        r.is_stream   = ev->is_stream;
        r.duration_ms = ev->duration_ms;
        r.position_ms = ev->position_ms;
        r.message     = ev->message ? ev->message : "";
        events.push_back(r);
        threads.push_back(std::this_thread::get_id());
        cv.notify_all();
    }

    bool waitFor(int32_t type, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            for (const Recorded &r : events)
                if (r.type == type) return true;
            return false;
        });
    }

    bool find(int32_t type, Recorded *out = nullptr)
    {
        std::lock_guard<std::mutex> lock(m);
        for (const Recorded &r : events) {
            if (r.type == type) {
                if (out) *out = r;
                return true;
            }
        }
        return false;
    }

    bool findState(int32_t state)
    {
        std::lock_guard<std::mutex> lock(m);
        for (const Recorded &r : events)
            if (r.type == OP_EVENT_STATE && r.state == state) return true;
        return false;
    }

    std::thread::id lastThread()
    {
        std::lock_guard<std::mutex> lock(m);
        return threads.empty() ? std::thread::id() : threads.back();
    }
};

static void on_event_cb(void *user, const op_event *ev)
{
    static_cast<Capture *>(user)->push(ev);
}

/* 视频帧回调：本用例不播真文件，只是让"注册过回调"这件事成立 */
static void dummy_video(void *user, const op_frame *f)
{
    (void)user;
    (void)f;
}

int main()
{
    const std::thread::id mainThread = std::this_thread::get_id();

    std::printf("== 1. op_version\n");
    {
        int32_t major = -1, minor = -1;
        CHECK(op_version(&major, &minor) == OP_OK);
        CHECK(major == OP_API_VERSION_MAJOR);
        CHECK(minor == OP_API_VERSION_MINOR);
        CHECK(op_version(nullptr, nullptr) == OP_OK);   // 两个参数都可以是 NULL
    }

    std::printf("== 2. create / 回调注册 / 初始查询\n");
    Capture cap;
    op_player *p = op_create();
    CHECK(p != nullptr);
    if (!p) {
        std::printf("\nop_create 失败，后面的用例没有意义\n");
        return 1;
    }
    CHECK(op_set_event_cb(p, on_event_cb, &cap) == OP_OK);
    CHECK(op_set_video_cb(p, dummy_video, nullptr) == OP_OK);
    CHECK(op_set_log_cb(p, nullptr, nullptr) == OP_OK);   // NULL = 取消订阅

    CHECK(op_set_event_cb(nullptr, nullptr, nullptr) == OP_ERR_INVALID_ARG);
    CHECK(op_state_of(p) == OP_STATE_IDLE);
    CHECK(op_position_ms(p) == 0);
    CHECK(op_duration_ms(p) == 0);
    CHECK(op_has_video(p) == 0);
    CHECK(op_has_audio(p) == 0);
    CHECK(op_is_music(p) == 0);
    CHECK(op_is_stream(p) == 0);

    std::printf("== 3. 没 open 时，播放控制必须报 NOT_READY\n");
    {
        CHECK(op_play(p) == OP_ERR_NOT_READY);
        CHECK(op_pause(p) == OP_ERR_NOT_READY);
        CHECK(op_seek_ms(p, 1000) == OP_ERR_NOT_READY);
        CHECK(op_step_frame(p, 1) == OP_ERR_NOT_READY);
        CHECK(op_set_speed(p, 10) == OP_ERR_NOT_READY);
    }

    std::printf("== 4. op_open 的参数校验\n");
    {
        CHECK(op_open(p, nullptr) == OP_ERR_INVALID_ARG);

        // path 和 fd 都没给 → INVALID_ARG（旧代码在这里 std::string(nullptr) 崩）
        op_open_options opt{};
        opt.fd   = -1;
        opt.path = nullptr;
        CHECK(op_open(p, &opt) == OP_ERR_INVALID_ARG);

        // 只给 fd → fd 打开还没做（T5），要明说"不支持"，不能静默丢掉 fd
        opt.fd = 0;
        CHECK(op_open(p, &opt) == OP_ERR_UNSUPPORTED);
    }

    std::printf("== 5. 端到端：打不开的文件 → ERROR 事件 → 回到 IDLE\n");
    {
        op_open_options opt{};
        opt.fd   = -1;
        opt.path = "definitely_missing_file_12345.mp4";
        CHECK(op_open(p, &opt) == OP_OK);                 // 受理（异步）
        CHECK(op_state_of(p) == OP_STATE_OPENING);        // 立刻可见

        // 打开失败由 core 在消息循环线程里发现，再以 EVT_ERROR 回来
        CHECK(cap.waitFor(OP_EVENT_ERROR, 3000));
        Recorded err;
        CHECK(cap.find(OP_EVENT_ERROR, &err));
        CHECK(!err.message.empty());                      // core 的 "dt.openFile failed"

        // 状态机：ERROR 之后必须回到 IDLE，别让查询接口一直报 PLAYING
        CHECK(op_state_of(p) == OP_STATE_IDLE);
        CHECK(op_duration_ms(p) == 0);
        CHECK(op_has_video(p) == 0);

        // 契约：回调发生在 SDK 自己的线程上，不是调用方线程
        CHECK(cap.findState(OP_STATE_OPENING));
        CHECK(cap.lastThread() != mainThread);
    }

    std::printf("== 6. 仍是桩的接口：音频 pull（T4）\n");
    {
        CHECK(op_set_audio_format(p, 48000, 2) == OP_ERR_UNSUPPORTED);
        char buf[16] = {0};
        CHECK(op_audio_read(p, buf, sizeof(buf)) < 0);    // < 0 是错误码
        CHECK(op_audio_pending(p) == 0);
        CHECK(op_audio_latency_ms(p) == 0);
    }

    std::printf("== 7. 参数校验的边界值\n");
    {
        CHECK(op_seek_ms(p, -1) == OP_ERR_INVALID_ARG);
        CHECK(op_step_frame(p, 2) == OP_ERR_INVALID_ARG);
        CHECK(op_step_frame(p, 0) == OP_ERR_INVALID_ARG);
        CHECK(op_set_speed(p, 0) == OP_ERR_INVALID_ARG);
        CHECK(op_set_speed(p, 21) == OP_ERR_INVALID_ARG);
        CHECK(op_set_audio_format(p, 0, 2) == OP_ERR_INVALID_ARG);
        CHECK(op_set_audio_format(p, 48000, 0) == OP_ERR_INVALID_ARG);
    }

    std::printf("== 8. destroy：必须 join 干净，可重复 create/destroy\n");
    op_destroy(p);
    op_destroy(nullptr);   // 空指针必须容错

    // 连跑两轮：漏线程 / 漏 join 的话，进程退出时会 terminate 或卡死
    for (int i = 0; i < 2; ++i) {
        op_player *q = op_create();
        CHECK(q != nullptr);
        CHECK(op_state_of(q) == OP_STATE_IDLE);
        op_destroy(q);
    }

    if (g_fail == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
