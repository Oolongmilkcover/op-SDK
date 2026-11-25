/*
 * seek 回归测试（需要真实媒体验证，默认 SKIP）
 *
 * 用法：
 *   set OP_SDK_TEST_MEDIA=D:\path\video.mp4
 *   build\tests\test_seek.exe
 *
 * 验的是 2026-11-25 修掉的两个真 bug：
 *   1. 一次 op_seek_ms 收到**两条** SEEK_DONE —— 根因是 run() 用 PlayerContext 的
 *      异步广播副本判断"要不要 seek"，doSeek 结束时推的 CTX_IS_SEEKING=0 还没被
 *      消息循环处理，循环立刻又进了一次 doSeek()。
 *   2. 暂停状态下 seek **不重绘**、落点不生效 —— 第二次 doSeek 把刚 paint 出来的
 *      帧清掉，同时把 demux 位置又挪了一次。
 *
 * 所以这里的断言刻意很硬：暂停中 seek 之后必须"恰好一条 SEEK_DONE + 至少一帧画面
 * + 位置落在目标附近"，恢复播放还要从目标附近继续（不是从 0/旧位置继续）。
 *
 * 没有设置环境变量时打印 SKIP 并返回 0，这样 CI / 无媒体环境不会红。
 */
#include "op/op_player.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
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

struct Ev
{
    int32_t type = 0;
    int64_t pos  = 0;
};

struct Capture
{
    std::mutex              m;
    std::condition_variable cv;
    std::vector<Ev>         events;

    void push(const op_event *ev)
    {
        std::lock_guard<std::mutex> lock(m);
        events.push_back(Ev{ev->type, ev->position_ms});
        cv.notify_all();
    }

    bool waitFor(int32_t type, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            for (const Ev &e : events)
                if (e.type == type) return true;
            return false;
        });
    }

    int count(int32_t type)
    {
        std::lock_guard<std::mutex> lock(m);
        int n = 0;
        for (const Ev &e : events)
            if (e.type == type) ++n;
        return n;
    }

    int64_t lastPos(int32_t type)
    {
        std::lock_guard<std::mutex> lock(m);
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->type == type) return it->pos;
        return -1;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m);
        events.clear();
    }
};

static std::atomic<int> g_frames{0};

static void on_event(void *user, const op_event *ev)
{
    static_cast<Capture *>(user)->push(ev);
}

static void on_video(void *user, const op_frame *f)
{
    (void)user;
    if (f) g_frames.fetch_add(1);
}

int main()
{
    const char *media = std::getenv("OP_SDK_TEST_MEDIA");
    if (!media || !media[0]) {
        std::printf("SKIP: 设置 OP_SDK_TEST_MEDIA=<video> 才会跑真实 seek 用例\n");
        return 0;
    }

    op_player *p = op_create();
    if (!p) { std::printf("op_create 失败\n"); return 1; }

    Capture cap;
    op_set_event_cb(p, on_event, &cap);
    op_set_video_cb(p, on_video, nullptr);

    /* ---------------- 1. 打开 ---------------- */
    std::printf("== 1. open %s\n", media);
    op_open_options opt{};
    opt.fd   = -1;
    opt.path = media;
    CHECK(op_open(p, &opt) == OP_OK);
    CHECK(cap.waitFor(OP_EVENT_OPENED, 5000));
    CHECK(op_duration_ms(p) >= 2000);
    CHECK(op_has_video(p) == 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    /* ---------------- 2. 暂停中 seek ---------------- */
    std::printf("== 2. 暂停中 seek 到 1500ms\n");
    op_pause(p);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    cap.clear();
    g_frames.store(0);

    CHECK(op_seek_ms(p, 1500) == OP_OK);
    CHECK(cap.waitFor(OP_EVENT_SEEK_DONE, 3000));

    // 关键回归：一次 seek 只能有一条 SEEK_DONE
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const int doneCount = cap.count(OP_EVENT_SEEK_DONE);
    std::printf("   SEEK_DONE 条数 = %d，暂停中 seek 后的帧数 = %d\n",
                doneCount, g_frames.load());
    CHECK(doneCount == 1);

    // 暂停中 seek 必须把目标帧画出来（不然进度条动了、屏幕不动）
    CHECK(g_frames.load() >= 1);

    // 落点必须在目标附近
    const int64_t landed = cap.lastPos(OP_EVENT_SEEK_DONE);
    std::printf("   落点 = %lld ms\n", (long long)landed);
    CHECK(landed >= 1200 && landed <= 1800);

    // 查询接口也要跟着更新（壳的进度条读它）
    CHECK(op_position_ms(p) >= 1200 && op_position_ms(p) <= 1800);

    /* ---------------- 3. 恢复播放：从落点继续，而不是从 0 ---------------- */
    std::printf("== 3. 恢复播放\n");
    cap.clear();
    op_play(p);
    CHECK(cap.waitFor(OP_EVENT_POSITION, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const int64_t posAfterResume = cap.lastPos(OP_EVENT_POSITION);
    std::printf("   恢复后位置 = %lld ms\n", (long long)posAfterResume);
    CHECK(posAfterResume >= 1400);

    op_destroy(p);

    if (g_fail == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
