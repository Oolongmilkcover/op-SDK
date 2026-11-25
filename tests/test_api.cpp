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
#include "core/platform.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#  define T_OPEN(p)  _open(p, _O_RDONLY | _O_BINARY)
#  define T_CLOSE(f) _close(f)
#else
#  include <fcntl.h>
#  include <unistd.h>
#  define T_OPEN(p)  ::open(p, O_RDONLY)
#  define T_CLOSE(f) ::close(f)
#endif

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

/* ---------------------------------------------------------------- 日志捕获 */

/*
 * 和事件一样：日志回调发生在 SDK 自己的线程上，捕获侧必须自己加锁。
 * 顺便记下线程 id —— 日志的线程契约是"打日志的那个线程"，不是消息循环线程。
 */
struct LogCapture
{
    struct Line
    {
        int         level = 0;
        std::string text;
    };

    std::mutex                    m;
    std::condition_variable       cv;
    std::vector<Line>             lines;
    std::vector<std::thread::id>  threads;

    void push(int level, const char *text)
    {
        std::lock_guard<std::mutex> lock(m);
        lines.push_back(Line{level, text ? text : ""});
        threads.push_back(std::this_thread::get_id());
        cv.notify_all();
    }

    bool waitForContains(const std::string &needle, int timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            for (const Line &l : lines)
                if (l.text.find(needle) != std::string::npos) return true;
            return false;
        });
    }

    size_t count()
    {
        std::lock_guard<std::mutex> lock(m);
        return lines.size();
    }

    std::thread::id lastThread()
    {
        std::lock_guard<std::mutex> lock(m);
        return threads.empty() ? std::thread::id() : threads.back();
    }
};

static void on_log_cb(void *user, int level, const char *line)
{
    static_cast<LogCapture *>(user)->push(level, line);
}

/* ---------------------------------------------------------------- 临时 WAV */
/*
 * T5 的验收不想依赖仓库里的媒体文件，所以现场合成一个 8kHz / 单声道 / 16bit 的
 * 裸 PCM WAV。prefixBytes 用来模拟"容器前面还有别的数据"（picker 给 offset 的场景）。
 *
 * 手写 RIFF 头是刻意的：这样测试不依赖任何第三方编码器，也顺便说清了
 * fd_offset 到底切在文件的哪个字节上。
 */
static bool writeTinyWav(const std::string &path, int prefixBytes, int ms)
{
    const int sampleRate = 8000;
    const int channels   = 1;
    const int frames     = sampleRate * ms / 1000;
    const int dataBytes  = frames * channels * 2;

    std::vector<uint8_t> d((size_t)prefixBytes + 44 + dataBytes, 0);
    uint8_t *w = d.data() + prefixBytes;

    auto put32 = [](uint8_t *p, uint32_t v) {
        p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
        p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
    };
    auto put16 = [](uint8_t *p, uint16_t v) {
        p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    };

    std::memcpy(w, "RIFF", 4);
    put32(w + 4, (uint32_t)(36 + dataBytes));
    std::memcpy(w + 8, "WAVE", 4);
    std::memcpy(w + 12, "fmt ", 4);
    put32(w + 16, 16);
    put16(w + 20, 1);                       // PCM
    put16(w + 22, (uint16_t)channels);
    put32(w + 24, (uint32_t)sampleRate);
    put32(w + 28, (uint32_t)(sampleRate * channels * 2));
    put16(w + 32, (uint16_t)(channels * 2));
    put16(w + 34, 16);
    std::memcpy(w + 36, "data", 4);
    put32(w + 40, (uint32_t)dataBytes);

    for (int i = 0; i < frames; ++i) {
        const short s = (short)(((i / 40) % 2) ? 2000 : -2000);   // 方波，非零即可
        put16(w + 44 + i * 2, (uint16_t)s);
    }

    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(d.data(), 1, d.size(), f) == d.size();
    return std::fclose(f) == 0 && ok;
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

    /*
     * 平台档位：门面查询接口必须和 core 里的编译期常量一致。
     * 这条是"壳看到的档位 == 内核实际用的档位"的唯一保证 —— 两边一旦漂移，
     * 移动端就会出现"按钮亮着但点了返回 UNSUPPORTED"这类最难解释的现象。
     */
    std::printf("== 1b. op_platform / op_is_mobile 与编译期档位一致\n");
    {
        const int32_t pf = op_platform();
        CHECK(pf == static_cast<int32_t>(core::kPlatform));
        CHECK(op_is_mobile() == (core::kIsMobile ? 1 : 0));

        // 枚举覆盖：不认识的平台不能谎报成某个已知平台
        CHECK(pf >= OP_PLATFORM_UNKNOWN && pf <= OP_PLATFORM_HARMONYOS);

        // 移动端必须"8 帧 + 无历史 + 无逐帧"，桌面必须"16 帧 + 有历史 + 有逐帧"
        CHECK(core::kVideoFrameQueueSize == (core::kIsMobile ? 8 : 16));
        CHECK(core::kKeepFrameHistory == !core::kIsMobile);
        CHECK(core::kFrameStepSupported == !core::kIsMobile);
        CHECK(core::kVideoPacketQueueSize == (core::kIsMobile ? 50 : 100));

        // 本机（构建它的人）必须是已知平台，否则 _WIN32/__OHOS__ 判定链写漏了
        CHECK(pf != OP_PLATFORM_UNKNOWN);
#if defined(_WIN32)
        CHECK(pf == OP_PLATFORM_WINDOWS);
#elif defined(__ANDROID__)
        CHECK(pf == OP_PLATFORM_ANDROID);
#elif defined(__OHOS__)
        CHECK(pf == OP_PLATFORM_HARMONYOS);
#elif defined(__linux__)
        CHECK(pf == OP_PLATFORM_LINUX);
#endif
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
        // 移动档连"逐帧"这个能力都没有，所以是 UNSUPPORTED 而不是 NOT_READY ——
        // 平台属性优先于"现在有没有打开文件"
        CHECK(op_step_frame(p, 1) == (core::kFrameStepSupported ? OP_ERR_NOT_READY
                                                               : OP_ERR_UNSUPPORTED));
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

        // fd 的合法值包含 0，所以这里不能用 fd=0 去测"没给 fd"——
        // 真正的 fd 打开由第 9 节（合成 WAV → fd）端到端验证。
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

    std::printf("== 6. 音频 pull（T4）：没 open 时是「没数据」，不是错误\n");
    {
        CHECK(op_set_audio_format(p, 48000, 2) == OP_OK);
        char buf[16] = {0};
        // 没 open → 没有环 → read 返回 0（暂时没数据），绝不能返回负数：
        // 门面的约定是 <0 才是 op_result 错误码。
        CHECK(op_audio_read(p, buf, sizeof(buf)) == 0);
        CHECK(op_audio_pending(p) == 0);
        CHECK(op_audio_latency_ms(p) == 0);
        // 壳回传设备积压：没 open 时也允许调，值会被忽略
        CHECK(op_audio_set_device_pending_ms(p, 50) == OP_OK);
        CHECK(op_audio_set_device_pending_ms(p, -1) == OP_ERR_INVALID_ARG);
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

    std::printf("== 8. 日志回调（T1+）：core 的日志能到 UI 层\n");
    LogCapture logs;
    {
        CHECK(op_set_log_cb(p, on_log_cb, &logs) == OP_OK);

        // 用同一个"打不开的文件"再触发一次：core 会在 openFile 里
        // logDebug() << "open" << url << "failed!:" << err_buf;
        op_open_options opt{};
        opt.fd   = -1;
        opt.path = "definitely_missing_file_12345.mp4";
        CHECK(op_open(p, &opt) == OP_OK);

        CHECK(logs.waitForContains("failed", 3000));
        CHECK(logs.count() > 0);

        // 契约：日志回调在"打日志的那个 SDK 线程"上，不是调用方 UI 线程
        CHECK(logs.lastThread() != mainThread);
    }

    std::printf("== 9. fd 打开（T5）：合成 WAV → fd + offset → OPENED\n");
    {
        const std::string wavPath = "test_api_tmp.wav";
        const int kPrefix = 100;                 // 模拟容器前面还压着别的东西

        CHECK(writeTinyWav(wavPath, kPrefix, 1000));

        const int fd = T_OPEN(wavPath.c_str());
        CHECK(fd >= 0);
        if (fd >= 0) {
            op_player *q = op_create();
            CHECK(q != nullptr);
            if (q) {
                Capture c2;
                op_set_event_cb(q, on_event_cb, &c2);

                op_open_options opt{};
                opt.fd        = fd;              // 关键：只给 fd
                opt.fd_offset = kPrefix;         // 跳过前面的垃圾字节
                opt.fd_size   = 0;               // 0 = 自动探测
                opt.path      = wavPath.c_str(); // 只当扩展名提示
                CHECK(op_open(q, &opt) == OP_OK);

                CHECK(c2.waitFor(OP_EVENT_OPENED, 5000));
                Recorded r;
                CHECK(c2.find(OP_EVENT_OPENED, &r));
                // OPENED 不再携带文件级属性（壳改用查询接口），这里跟着新契约走
                CHECK(op_has_audio(q) == 1);
                CHECK(op_has_video(q) == 0);
                // 1 秒的 WAV：时长必须接近 1000ms（offset 切错就会读不到 data 块）
                const int64_t d = op_duration_ms(q);
                CHECK(d >= 900 && d <= 1100);

                // 顺带验 T4 的接线：open 之后音频环里应该很快就能拉到 PCM
                bool gotPcm = false;
                for (int i = 0; i < 200 && !gotPcm; ++i) {
                    char pcm[4096];
                    if (op_audio_read(q, pcm, sizeof(pcm)) > 0)
                        gotPcm = true;
                    else
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                CHECK(gotPcm);

                op_destroy(q);
            }
            T_CLOSE(fd);   // fd 归调用方：SDK 全程没关过它，这里由测试来关
        }
        std::remove(wavPath.c_str());
    }

    std::printf("== 10. destroy：必须 join 干净，可重复 create/destroy\n");
    const size_t logCountBeforeDestroy = logs.count();
    op_destroy(p);
    op_destroy(nullptr);   // 空指针必须容错

    // 先摘日志回调、再 join 的效果：销毁之后不能再有任何日志回调打进来
    // （user 指针此时已经无效，晚到一行就是 use-after-free）
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(logs.count() == logCountBeforeDestroy);

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
