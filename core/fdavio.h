#ifndef CORE_FDAVIO_H
#define CORE_FDAVIO_H

/*
 * 用文件描述符喂 FFmpeg 的 AVIO（T5）
 *
 * 为什么需要它：
 *   鸿蒙 picker 给的是 URI、安卓给的是 content://，NDK 侧能拿到的通常只有一个 fd。
 *   avformat_open_input() 只认路径/URL，所以要么先把 fd 拷进沙箱（几十 MB 的视频
 *   白拷一遍），要么自己实现 AVIOContext —— 后者就是这里做的事。
 *
 * AVIO 是什么（一句话）：
 *   FFmpeg 把所有输入输出抽象成 AVIOContext：里面是一个"读/写回调 + seek 回调 +
 *   一段缓冲区"。自定义 AVIO 就是告诉 FFmpeg"字节从哪来、怎么跳"，剩下的事情
 *   （协议解析、探测格式、解封装）它照常做。
 *
 * 用法（三步，顺序不能错）：
 *     1. AVFormatContext* fmt = avformat_alloc_context();
 *     2. fmt->pb = core::openFdAvio(in, &err);
 *        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;        // 关键：让 avformat_close_input 别释放它
 *     3. avformat_open_input(&fmt, "fd", nullptr, nullptr);
 *   播放全程都要让这个 AVIOContext 活着（demux 线程一直在从它读），
 *   到 close 时先 avformat_close_input，再 core::closeFdAvio()。
 */

#include <cstdint>
#include <string>

struct AVIOContext;

namespace core
{

/*
 * 一次 fd 打开的参数。
 *   fd     : 调用方持有的文件描述符（SDK 不 close 它）
 *   offset : 从文件的第 offset 字节开始读（容器嵌在别的文件里、或 picker 给了偏移）
 *   size   : 可读长度；0 = 自动探测（普通文件 = 文件大小 - offset）
 */
struct FdInput
{
    int     fd     = -1;
    int64_t offset = 0;
    int64_t size   = 0;
};

/*
 * 建一个从 fd 读字节的 AVIOContext。
 * 成功返回非空；失败返回 nullptr 并把原因写进 err（err 可为 nullptr）。
 *
 * 注意：
 *   - 返回的指针要用 core::closeFdAvio() 释放，**不要**直接 avio_context_free()
 *     （opaque 里有一块我们自己 av_malloc 的上下文，不同 FFmpeg 版本对它的处理不同）；
 *   - 只能用于可 seek 的普通文件。管道/套接字能打开，但 seek 会失败，
 *     时长、seek、逐帧这些功能都不可用（见 .cpp 里的说明）。
 */
AVIOContext *openFdAvio(const FdInput &in, std::string *err);

// 释放 openFdAvio() 建出来的 AVIOContext（传 nullptr 安全）。
void closeFdAvio(AVIOContext **pb);

} // namespace core

#endif // CORE_FDAVIO_H
