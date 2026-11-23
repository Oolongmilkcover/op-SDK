#include "core/fdavio.h"

#include "core/log.h"

#include <cerrno>
#include <cstring>

// Linux/Android 上 off_t 默认可能是 32 位，大文件会截断；必须在包含系统头之前定义。
#ifndef _WIN32
#  ifndef _FILE_OFFSET_BITS
#    define _FILE_OFFSET_BITS 64
#  endif
#endif

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#  include <sys/stat.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace core
{
namespace
{

/*
 * AVIO 的 opaque：FFmpeg 只在回调里把它还给我们，它自己不解释内容。
 *
 * 用 av_malloc 分配（不是 new）：不同 FFmpeg 版本里 avio_context_free() 对 opaque
 * 的处理不一致（有的版本会顺手 av_freep(&pb->opaque)），所以 closeFdAvio() 会先把
 * opaque 摘成 nullptr 再自己释放 —— 这样两种行为都不会双重释放。
 */
struct FdIo
{
    int     fd    = -1;
    int64_t start = 0;    // 本次"虚拟文件"的 0 对应 fd 的哪个绝对偏移
    int64_t size  = -1;   // 虚拟文件长度；-1 = 不可 seek / 未知
    int64_t pos   = 0;    // 虚拟文件内的当前读位置
};

int rawRead(int fd, uint8_t *buf, int n)
{
#ifdef _WIN32
    return _read(fd, buf, static_cast<unsigned>(n));
#else
    return static_cast<int>(::read(fd, buf, static_cast<size_t>(n)));
#endif
}

int64_t rawSeek(int fd, int64_t off, int origin)
{
#ifdef _WIN32
    return static_cast<int64_t>(_lseeki64(fd, off, origin));
#else
    return static_cast<int64_t>(::lseek(fd, static_cast<off_t>(off), origin));
#endif
}

std::string sysError(const char *what)
{
    return std::string(what) + ": " + std::strerror(errno);
}

/*
 * 读回调：FFmpeg 要 buf_size 字节。
 *
 * 返回值约定：>0 实际读到；0 或 AVERROR_EOF 表示文件尾；<0 是错误。
 * 这里**循环读到满**（除非 EOF/错误）：普通文件一般一次就够，但网络文件系统、
 * 信号打断（EINTR）都可能只返回一部分，交给 avio 的 buf 是固定长度，读不满
 * 也不会错，只是会多走几轮 —— 循环一下更省事，也避免探测阶段因为短读误判。
 */
int fdRead(void *opaque, uint8_t *buf, int bufSize)
{
    FdIo *c = static_cast<FdIo *>(opaque);

    /*
     * 有显式 size 时，读到虚拟文件末尾就停 —— 不能顺着 fd 继续读下去。
     * 这条不是防御性编程：fd_size 的语义就是"只暴露文件的一段"（比如容器嵌在
     * 一个大文件里），越过它读到的就是别人的数据，解码器会当成码流解析。
     * 一开始漏了这条，test_fdavio 的"显式 size"用例就是在这里挂的。
     */
    if (c->size >= 0) {
        const int64_t left = c->size - c->pos;
        if (left <= 0)
            return AVERROR_EOF;
        if ((int64_t)bufSize > left)
            bufSize = (int)left;
    }

    int total = 0;
    while (total < bufSize) {
        const int n = rawRead(c->fd, buf + total, bufSize - total);
        if (n > 0) {
            total += n;
            continue;
        }
        if (n == 0)
            break;                       // EOF
        if (errno == EINTR)
            continue;                    // 被信号打断，重试
        return total > 0 ? total : AVERROR(errno);
    }
    c->pos += total;
    return total;
}

/*
 * seek 回调。whence 里可能带 AVSEEK_FORCE（允许越过缓冲区边界），
 * 它对我们没有意义 —— 我们本来就直接操作 fd，不经过 avio 的缓冲。
 */
int64_t fdSeek(void *opaque, int64_t offset, int whence)
{
    FdIo *c = static_cast<FdIo *>(opaque);

    if (whence & AVSEEK_SIZE) {
        return c->size >= 0 ? c->size : AVERROR(ENOSYS);
    }
    whence &= ~AVSEEK_FORCE;

    if (c->size < 0)
        return AVERROR(ESPIPE);          // 管道/套接字：不支持 seek

    int64_t target = 0;
    switch (whence) {
    case SEEK_SET:
        target = c->start + offset;
        break;
    case SEEK_CUR:
        // 用我们自己的 pos，而不是回问 fd：流一旦被别处动过位置，回问会读到别人的状态
        target = c->start + c->pos + offset;
        break;
    case SEEK_END:
        target = c->start + c->size + offset;
        break;
    default:
        return AVERROR(EINVAL);
    }

    /*
     * 只挡住"跳到虚拟文件之前"，**不夹上界**。
     *
     * 为什么不夹越界：avio_seek() 会直接返回它请求的 offset、并把这个值记到
     * s->pos 里，而不是用回调的返回值。回调私自夹住的话，avio 认为的位置和底层
     * fd 的实际位置就对不上了 —— 后续读会从错的地方开始。
     * 越过末尾是合法的（读完就是 EOF），真正的边界由 fdRead 按 size 挡。
     */
    if (target < c->start)
        target = c->start;

    const int64_t got = rawSeek(c->fd, target, SEEK_SET);
    if (got < 0)
        return AVERROR(errno);
    c->pos = got - c->start;
    return c->pos;
}

/* 探测文件大小；不可 seek 时返回 -1。 */
int64_t probeSize(int fd)
{
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(fd, &st) == 0 && (st.st_mode & _S_IFREG))
        return static_cast<int64_t>(st.st_size);
#else
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
        return static_cast<int64_t>(st.st_size);
#endif
    // 不是普通文件（管道、字符设备、某些 Android content:// 的转接）：
    // 再试一次 SEEK_END，仍失败就按"不可 seek"处理。
    return rawSeek(fd, 0, SEEK_END);
}

} // namespace

AVIOContext *openFdAvio(const FdInput &in, std::string *err)
{
    auto fail = [err](const std::string &msg) -> AVIOContext * {
        if (err)
            *err = msg;
        logError() << "[fdavio] " << msg;
        return nullptr;
    };

    if (in.fd < 0)
        return fail("fd 无效（<0）");

    const int64_t start = in.offset > 0 ? in.offset : 0;

    // ---- 1. 确定"虚拟文件"的长度 ----
    int64_t size = -1;
    if (in.size > 0) {
        size = in.size;                                  // 调用方说了算
    } else {
        const int64_t end = probeSize(in.fd);
        if (end >= 0) {
            if (end < start)
                return fail("fd_offset 超出文件末尾");
            size = end - start;
        }
        // end < 0：不可 seek。size 保持 -1，读还能读，但没有时长/seek。
    }

    // ---- 2. 分配 opaque 和 avio 缓冲 ----
    // 注意：av_malloc 不跑构造函数，结构体里的默认成员初始值**不会**生效，
    // 所以这里必须逐个赋值（漏了 pos 就会被当成随机值，读起来直接 EOF）。
    FdIo *c = static_cast<FdIo *>(av_malloc(sizeof(FdIo)));
    if (!c)
        return fail("av_malloc(FdIo) 失败");
    c->fd    = in.fd;
    c->start = start;
    c->size  = size;
    c->pos   = 0;

    constexpr int kBufSize = 32 * 1024;
    auto *buf = static_cast<unsigned char *>(av_malloc(kBufSize));
    if (!buf) {
        av_free(c);
        return fail("av_malloc(avio buffer) 失败");
    }

    /*
     * write_flag = 0：只读。
     * 第 7 个参数是 seek 回调；传 nullptr 的话 FFmpeg 认为完全不可 seek。
     * 这里始终传，靠内部 size<0 返回 ESPIPE 来表达"不可 seek"——
     * 这样管道在探测阶段能明确知道"seek 不支持"，而不是死循环重试。
     */
    AVIOContext *pb = avio_alloc_context(buf, kBufSize, 0, c, fdRead, nullptr, fdSeek);
    if (!pb) {
        av_free(buf);
        av_free(c);
        return fail("avio_alloc_context 失败");
    }

    pb->seekable = size >= 0 ? AVIO_SEEKABLE_NORMAL : 0;

    // ---- 3. 把底层 fd 摆到起点 ----
    if (size >= 0) {
        if (rawSeek(in.fd, start, SEEK_SET) < 0)
            return fail(sysError("seek 到 fd_offset 失败"));
    }
    // size < 0（管道）：不动位置，从 fd 当前所在处开始读，offset 被忽略。

    return pb;
}

void closeFdAvio(AVIOContext **pb)
{
    if (!pb || !*pb)
        return;

    /*
     * 先把 opaque 摘下来自己释放，再交给 avio_context_free：
     * 有的 FFmpeg 版本会 av_freep(&pb->opaque)，有的不会 —— 摘成 nullptr 之后
     * 两种都不会双重释放，也不会泄漏。buffer 由 avio_context_free 负责释放。
     */
    FdIo *c = static_cast<FdIo *>((*pb)->opaque);
    (*pb)->opaque = nullptr;
    av_free(c);

    avio_context_free(pb);
    *pb = nullptr;
}

} // namespace core
