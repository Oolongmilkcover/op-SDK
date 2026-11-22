/*
 * AudioRing / AudioRingSink 单元测试（T4 Step 1 + Step 2）
 *
 * 覆盖：
 *   1. 容量取整、写读、available/space
 *   2. 绕回（写满→读一半→再写）、按采样帧对齐、满时写返回 0、数据完整性
 *   3. clear
 *   4. Sink: open 后的换算（ring 字节 → 毫秒）、getFree/read
 *   5. devicePending 的"锚点 + 外推 + 夹紧"：线性衰减、不为负、上限、负数上报
 *   6. 暂停冻结 / 恢复
 *   7. clear 之后过期上报被削掉（seek 场景）
 *   8. SPSC 并发：1MB 数据跨线程校验，强制绕回
 *
 * 时钟是注入的（setClock），所以这些用例是确定性的，不依赖真实时间。
 */
#include "audioring.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
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

static int64_t g_now = 0;     // 注入的单调毫秒时钟

static int64_t testClock() { return g_now; }

static uint8_t byteAt(int i) { return (uint8_t)((i * 31 + 7) & 0xFF); }

int main()
{
    /* ================= 1. 基本读写 ================= */
    std::printf("== 1. 容量 / 写读 / 空间\n");
    {
        AudioRing ring(10000, 1);
        CHECK(ring.capacity() == 16384);          // 向上取整到 2 的幂
        CHECK(ring.available() == 0);
        CHECK(ring.space() == ring.capacity());

        std::vector<uint8_t> src(100);
        for (int i = 0; i < 100; ++i) src[i] = byteAt(i);

        CHECK(ring.write(src.data(), 100) == 100);
        CHECK(ring.available() == 100);
        CHECK(ring.space() == ring.capacity() - 100);

        std::vector<uint8_t> dst(64, 0);
        CHECK(ring.read(dst.data(), 40) == 40);
        CHECK(std::memcmp(dst.data(), src.data(), 40) == 0);
        CHECK(ring.available() == 60);
    }

    /* ================= 2. 绕回 / 对齐 / 写满 ================= */
    std::printf("== 2. 绕回 / 对齐 / 写满 / 完整性\n");
    {
        AudioRing ring(1024, 4);                  // 一"帧"4 字节
        CHECK(ring.capacity() == 1024);

        std::vector<uint8_t> src(2048);
        for (int i = 0; i < 2048; ++i) src[i] = byteAt(i);

        // 对齐：写 10 只接受 8；读 3 凑不齐一整帧 → 0；读 6 → 4
        CHECK(ring.write(src.data(), 10) == 8);
        std::vector<uint8_t> small(64);
        CHECK(ring.read(small.data(), 3) == 0);
        CHECK(ring.read(small.data(), 6) == 4);
        CHECK(ring.available() == 4);

        // 填满：再多写就是 0，不能阻塞、不能覆盖
        ring.clear();
        CHECK(ring.write(src.data(), 1024) == 1024);
        CHECK(ring.space() == 0);
        CHECK(ring.write(src.data(), 100) == 0);

        // 绕回：读掉一半，再写入一批（写指针越过缓冲末尾回到开头）
        std::vector<uint8_t> half(512);
        CHECK(ring.read(half.data(), 512) == 512);
        CHECK(ring.write(src.data() + 1024, 512) == 512);   // 这批是 byteAt(1024..1535)
        CHECK(ring.available() == 1024);
        CHECK(ring.space() == 0);

        // 完整性：环里应当是 byteAt(512..1023) + byteAt(1024..1535)
        std::vector<uint8_t> got(1024);
        CHECK(ring.read(got.data(), 1024) == 1024);
        int bad = 0;
        for (int i = 0; i < 512; ++i)
            if (got[i] != byteAt(512 + i)) ++bad;
        for (int i = 0; i < 512; ++i)
            if (got[512 + i] != byteAt(1024 + i)) ++bad;
        CHECK(bad == 0);
    }

    /* ================= 3. clear ================= */
    std::printf("== 3. clear\n");
    {
        AudioRing ring(4096, 2);
        std::vector<uint8_t> buf(100, 0);
        ring.write(buf.data(), 100);
        CHECK(ring.available() == 100);

        ring.clear();
        CHECK(ring.available() == 0);
        CHECK(ring.space() == ring.capacity());
        CHECK(ring.read(buf.data(), 10) == 0);
    }

    /* ================= 4. Sink: 单位换算 ================= */
    std::printf("== 4. Sink: ring 字节 → 毫秒\n");
    {
        AudioRingSink sink;                       // 容量 0 = 按 open 的采样率算 300ms
        sink.setClock(&testClock);
        g_now = 10000;
        CHECK(sink.open(48000, 2));
        CHECK(sink.frameBytes() == 4);            // S16 交错 stereo
        CHECK(sink.bytesPerMs() == 192);          // 48000×2×2/1000

        g_now = 11000;
        std::vector<uint8_t> pcm(19200, 0);       // 192 B/ms → 100ms
        CHECK(sink.write(pcm.data(), (int)pcm.size()) == 19200);
        CHECK(sink.availableBytes() == 19200);
        CHECK(sink.getNoPlayMs() == 100);         // 设备积压为 0，只算 ring

        // 300ms @48k/2ch = 57600 → 向上取整 65536
        CHECK(sink.getFree() == 65536 - 19200);

        std::vector<uint8_t> out(1920);           // 10ms
        CHECK(sink.read(out.data(), 1920) == 1920);
        CHECK(sink.availableBytes() == 19200 - 1920);
        CHECK(sink.getNoPlayMs() == 90);

        // 读空之后：getNoPlayMs 变 0，再读返回 0（不是错误，壳据此等一会儿）
        std::vector<uint8_t> drain(4096);
        while (sink.read(drain.data(), (int)drain.size()) > 0) { }
        CHECK(sink.availableBytes() == 0);
        CHECK(sink.getNoPlayMs() == 0);
        CHECK(sink.read(drain.data(), (int)drain.size()) == 0);
    }

    /* ================= 5. devicePending: 外推与夹紧 ================= */
    std::printf("== 5. devicePending: 外推 / 夹紧\n");
    {
        AudioRingSink sink;
        sink.setClock(&testClock);
        g_now = 10000;
        CHECK(sink.open(48000, 2));

        g_now = 11000;                            // 离 open 已过 1s，夹紧③ 不会误伤
        sink.setDevicePendingMs(80);
        CHECK(sink.devicePendingMs() == 80);
        CHECK(sink.getNoPlayMs() == 80);          // ring 空，只剩设备积压

        g_now = 11030;
        CHECK(sink.devicePendingMs() == 50);      // 线性衰减：80 − 30

        g_now = 11150;
        CHECK(sink.devicePendingMs() == 0);       // 不为负

        g_now = 12000;
        sink.setDevicePendingMs(9999);
        CHECK(sink.devicePendingMs() == AudioRingSink::kMaxDevicePendingMs);

        sink.setDevicePendingMs(-5);
        CHECK(sink.devicePendingMs() == 0);
    }

    /* ================= 6. 暂停冻结 / 恢复 ================= */
    std::printf("== 6. 暂停冻结 / 恢复\n");
    {
        AudioRingSink sink;
        sink.setClock(&testClock);
        g_now = 20000;
        CHECK(sink.open(48000, 2));

        g_now = 21000;
        sink.setDevicePendingMs(100);
        CHECK(sink.devicePendingMs() == 100);

        sink.setPause(true);
        g_now = 26000;                            // 暂停 5 秒
        CHECK(sink.devicePendingMs() == 100);     // 冻结，不衰减

        sink.setPause(false);                     // 重新锚定
        CHECK(sink.devicePendingMs() == 100);
        g_now = 26020;
        CHECK(sink.devicePendingMs() == 80);      // 恢复后继续衰减
    }

    /* ================= 7. 过期上报（seek 后晚到） ================= */
    std::printf("== 7. clear 之后：过期上报被削掉\n");
    {
        AudioRingSink sink;
        sink.setClock(&testClock);
        g_now = 30000;
        CHECK(sink.open(48000, 2));

        g_now = 31000;
        sink.setDevicePendingMs(500);
        CHECK(sink.devicePendingMs() == 500);

        sink.clear();                             // 模拟 seek：ring 清空 + 设备视为已 flush
        CHECK(sink.devicePendingMs() == 0);
        CHECK(sink.availableBytes() == 0);

        // 一条"seek 前发出、seek 后才到"的旧上报：应该被压到 0
        sink.setDevicePendingMs(500);
        CHECK(sink.devicePendingMs() == 0);

        g_now = 31050;                            // 上限随时间放开
        CHECK(sink.devicePendingMs() == 50);
    }

    /* ================= 8. SPSC 并发 ================= */
    std::printf("== 8. SPSC 并发（1MB，强制绕回）\n");
    {
        AudioRing ring(1024, 1);                   // 故意很小，制造频繁回绕
        const int N = 1 << 20;

        std::atomic<int> produced{0};
        int readTotal = 0;
        int badBytes  = 0;

        std::thread producer([&] {
            std::vector<uint8_t> chunk(128);
            int written = 0;
            while (written < N) {
                const int n = std::min((int)chunk.size(), N - written);
                for (int i = 0; i < n; ++i) chunk[i] = byteAt(written + i);
                const int w = ring.write(chunk.data(), n);
                if (w == 0) { std::this_thread::yield(); continue; }
                written += w;
            }
            produced.store(written);
        });

        std::vector<uint8_t> buf(256);
        while (readTotal < N) {
            const int r = ring.read(buf.data(), (int)buf.size());
            if (r == 0) { std::this_thread::yield(); continue; }
            for (int i = 0; i < r; ++i)
                if (buf[i] != byteAt(readTotal + i)) ++badBytes;
            readTotal += r;
        }
        producer.join();

        CHECK(produced.load() == N);
        CHECK(readTotal == N);
        CHECK(badBytes == 0);                      // 顺序与内容都不能错
        CHECK(ring.available() == 0);
    }

    if (g_fail == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::printf("\n%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
