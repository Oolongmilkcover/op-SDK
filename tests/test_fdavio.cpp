/*
 * FD AVIO 单元测试（T5）
 *
 * 这个文件同时也是"自定义 AVIO 怎么工作"的可执行示例：
 *   - 造一个临时文件（前 512 字节是垃圾，后面是 10KB 有规律的字节）
 *   - 用 open() 拿 fd，交给 core::openFdAvio() 建 AVIOContext
 *   - 然后像 FFmpeg 那样只用 avio_read / avio_seek / avio_size 操作它
 *
 * 覆盖：
 *   1. offset / 自动探测 size：avio_size 报的是"虚拟文件"长度，不是物理文件长度
 *   2. 读：内容必须和文件里对应偏移的字节一致（证明 offset 真的生效了）
 *   3. seek：SET / END / 回到 0；越界被夹住
 *   4. EOF：读完返回 AVERROR_EOF
 *   5. 显式 size（只暴露文件的一段）
 *   6. offset 超出文件末尾 → 明确失败，带错误文本
 *   7. closeFdAvio 幂等，且**不会**关闭调用方的 fd
 *
 * 不碰 FFmpeg 的解封装，所以不需要媒体文件，headless 也能跑。
 */
#include "core/fdavio.h"

#include <cstdio>
#include <cstring>
#include <string>
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

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
}

static int g_fail = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  [FAIL] %s (line %d)\n", #cond, __LINE__);      \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

static uint8_t byteAt(int i) { return (uint8_t)((i * 37 + 11) & 0xFF); }

/* 写一个文件：junkPrefix 个 0xEE，后面跟 total 个 byteAt(i) */
static bool writePatternFile(const std::string &path, int total, int junkPrefix)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    for (int i = 0; i < junkPrefix; ++i)
        std::fputc(0xEE, f);
    for (int i = 0; i < total; ++i)
        std::fputc(byteAt(i), f);
    return std::fclose(f) == 0;
}

int main()
{
    const std::string path = "test_fdavio_tmp.bin";
    const int kPayload = 10240;
    const int kJunk    = 512;

    if (!writePatternFile(path, kPayload, kJunk)) {
        std::printf("无法创建临时文件 %s\n", path.c_str());
        return 1;
    }

    const int fd = T_OPEN(path.c_str());
    CHECK(fd >= 0);
    if (fd < 0) return 1;

    /* ================= 1. offset + 自动探测 size ================= */
    std::printf("== 1. offset=512 / size 自动探测\n");
    {
        core::FdInput in;
        in.fd     = fd;
        in.offset = kJunk;
        in.size   = 0;                       // 0 = 让 SDK 用 fstat 探测

        std::string err;
        AVIOContext *pb = core::openFdAvio(in, &err);
        CHECK(pb != nullptr);
        if (pb) {
            // 虚拟文件从 offset 开始，所以大小是"物理大小 - offset"
            CHECK(avio_size(pb) == kPayload);

            uint8_t buf[64] = {0};
            CHECK(avio_read(pb, buf, sizeof(buf)) == (int)sizeof(buf));
            bool same = true;
            for (int i = 0; i < 64; ++i)
                if (buf[i] != byteAt(i)) same = false;
            CHECK(same);                     // 第一字节是 pattern[0]，不是 0xEE
        }
        core::closeFdAvio(&pb);
        CHECK(pb == nullptr);
    }

    /* ================= 2. seek：SET / END / 回 0 ================= */
    std::printf("== 2. seek\n");
    {
        core::FdInput in;
        in.fd     = fd;
        in.offset = kJunk;

        AVIOContext *pb = core::openFdAvio(in, nullptr);
        CHECK(pb != nullptr);
        if (pb) {
            CHECK(avio_seek(pb, 4096, SEEK_SET) == 4096);
            uint8_t buf[32] = {0};
            CHECK(avio_read(pb, buf, sizeof(buf)) == (int)sizeof(buf));
            bool same = true;
            for (int i = 0; i < 32; ++i)
                if (buf[i] != byteAt(4096 + i)) same = false;
            CHECK(same);

            // 注意：FFmpeg 的 avio_seek() 只接受 SEEK_SET / SEEK_CUR，
            // 传 SEEK_END 会直接返回 EINVAL。要"从末尾算"得先 avio_size()。
            const int64_t end = avio_size(pb);
            CHECK(end == kPayload);
            CHECK(avio_seek(pb, end - 100, SEEK_SET) == end - 100);
            CHECK(avio_seek(pb, 0, SEEK_SET) == 0);

            // 读到末尾：先读 kPayload-7 字节，再读就应该 EOF
            std::vector<uint8_t> all((size_t)kPayload - 7);
            CHECK(avio_read(pb, all.data(), (int)all.size()) == (int)all.size());
            uint8_t tail[8] = {0};
            CHECK(avio_read(pb, tail, sizeof(tail)) == (int)(7));
            CHECK(avio_read(pb, tail, sizeof(tail)) == AVERROR_EOF);
        }
        core::closeFdAvio(&pb);
    }

    /* ================= 3. 显式 size = 只暴露一段 ================= */
    std::printf("== 3. 显式 size\n");
    {
        core::FdInput in;
        in.fd     = fd;
        in.offset = kJunk;
        in.size   = 2048;

        AVIOContext *pb = core::openFdAvio(in, nullptr);
        CHECK(pb != nullptr);
        if (pb) {
            CHECK(avio_size(pb) == 2048);

            std::vector<uint8_t> buf(2048);
            CHECK(avio_read(pb, buf.data(), (int)buf.size()) == (int)buf.size());
            bool same = true;
            for (int i = 0; i < 2048; ++i)
                if (buf[(size_t)i] != byteAt(i)) same = false;
            CHECK(same);

            uint8_t one = 0;
            CHECK(avio_read(pb, &one, 1) == AVERROR_EOF);

            /*
             * 越过 size 去 seek：avio_seek 返回的是**请求值**（它不采用回调的
             * 返回值，见 ffmpeg aviobuf.c），所以这里断言 999999 而不是 2048。
             * 真正要保证的是下一句：读必须立刻 EOF，不能顺着 fd 读到虚拟文件
             * 后面的数据 —— 那是 fdRead 里按 size 截断保证的。
             */
            CHECK(avio_seek(pb, 999999, SEEK_SET) == 999999);
            CHECK(avio_read(pb, &one, 1) == AVERROR_EOF);
        }
        core::closeFdAvio(&pb);
    }

    /* ================= 4. 整个文件（offset=0） ================= */
    std::printf("== 4. offset=0\n");
    {
        core::FdInput in;
        in.fd = fd;
        AVIOContext *pb = core::openFdAvio(in, nullptr);
        CHECK(pb != nullptr);
        if (pb) {
            CHECK(avio_size(pb) == kPayload + kJunk);
            uint8_t first = 0;
            CHECK(avio_read(pb, &first, 1) == 1);
            CHECK(first == 0xEE);
        }
        core::closeFdAvio(&pb);
    }

    /* ================= 5. 失败路径 ================= */
    std::printf("== 5. 参数错误 / offset 越界\n");
    {
        core::FdInput bad;
        bad.fd = -1;
        std::string err;
        CHECK(core::openFdAvio(bad, &err) == nullptr);
        CHECK(!err.empty());

        core::FdInput past;
        past.fd     = fd;
        past.offset = kPayload + kJunk + 1;   // 超过文件末尾
        err.clear();
        CHECK(core::openFdAvio(past, &err) == nullptr);
        CHECK(!err.empty());
    }

    /* ================= 6. close 幂等，且不动调用方的 fd ================= */
    std::printf("== 6. closeFdAvio 幂等 / 不关 fd\n");
    {
        core::FdInput in;
        in.fd = fd;
        AVIOContext *pb = core::openFdAvio(in, nullptr);
        CHECK(pb != nullptr);
        core::closeFdAvio(&pb);
        core::closeFdAvio(&pb);               // 第二次是空指针，必须安全
        CHECK(pb == nullptr);

        // fd 还归调用方：这里还能正常读（读到什么不重要，能读出字节就行）
#ifdef _WIN32
        char c = 0;
        CHECK(_lseeki64(fd, 0, SEEK_SET) == 0);
        CHECK(_read(fd, &c, 1) == 1);
#else
        char c = 0;
        CHECK(lseek(fd, 0, SEEK_SET) == 0);
        CHECK(read(fd, &c, 1) == 1);
#endif
    }

    T_CLOSE(fd);
    std::remove(path.c_str());

    if (g_fail == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d CHECK(S) FAILED\n", g_fail);
    return 1;
}
