# AVIO 学习笔记

> 目标：搞懂 FFmpeg 的 `AVIOContext` 是什么、怎么写一个自己的、以及哪里最容易错。
> 配合仓库 `op-SDK` 的 `core/fdavio.{h,cpp}` 一起看效果最好 —— 那份是"fd → AVIO"的完整实现。

---

## 0. 一句话

**AVIO 是 FFmpeg 的"字节流接口"**：协议层（file/http/rtsp）负责把字节拿出来，
AVIO 负责给格式层（mp4/wav/...）提供"顺序读 + 随机跳"。
自定义 AVIO 就是跳过协议层，直接告诉格式层"字节从哪来"。

---

## 1. 它在 FFmpeg 里的位置

```text
你的程序
   │  avformat_open_input("a.mp4") / av_read_frame()
   ▼
┌─────────────────────────────┐
│ 格式层  libavformat          │  解封装：mp4 / wav / flv ...
│  （AVFormatContext）         │
└─────────────┬───────────────┘
              │ avio_read / avio_seek / avio_size
┌─────────────▼───────────────┐
│ AVIO 层（AVIOContext）       │  32KB 缓冲 + 位置 + 读/写/seek 回调
└─────────────┬───────────────┘
              │ 回调（read_packet / seek）
┌─────────────▼───────────────┐
│ 协议层  file / http / rtsp   │  平时是 FFmpeg 自己实现
└─────────────────────────────┘
```

平时 `avformat_open_input()` 内部会调 `avio_open2()` 走协议层，把文件/网络
打开、包成 `AVIOContext`。**自定义 AVIO = 自己 new 一个 `AVIOContext` 塞给
`AVFormatContext::pb`**，协议层就被绕过了。

---

## 2. 一个 `AVIOContext` 里有什么

| 成员 | 作用 |
|---|---|
| `unsigned char *buffer` | FFmpeg 自己的缓冲区，一般 4KB~64KB |
| `buffer_size` | 缓冲区大小 |
| `buf_ptr` | 缓冲区里的读指针 |
| `buf_end` | 缓冲区里有效数据的末尾 |
| `pos` | 整个流的"虚拟位置"（read 到哪） |
| `read_packet` | 你的读回调 |
| `seek` | 你的 seek 回调 |
| `opaque` | 你的上下文指针，回调里原样传回 |
| `seekable` | `AVIO_SEEKABLE_NORMAL` 表示可以 seek |

`avio_alloc_context()` 的参数顺序是：

```c
AVIOContext *avio_alloc_context(
    unsigned char *buffer, int buffer_size,
    int write_flag,                 // 0=只读，1=只写
    void *opaque,
    int (*read_packet)(void*, uint8_t*, int),
    int (*write_packet)(void*, const uint8_t*, int),
    int64_t (*seek)(void*, int64_t, int));
```

---

## 3. 三个回调的契约（背下来）

### read

```c
int read_packet(void *opaque, uint8_t *buf, int size);
```

- `> 0`：实际读到的字节数（**可以小于 size**，但填满更省事）；
- `0` 或 `AVERROR_EOF`：文件尾；
- `< 0`：错误码，例如 `AVERROR(errno)`。

注意：`read` 返回 0 会被当成 EOF。网络流"暂时没数据"不能返回 0，要么阻塞，
要么返回 `AVERROR(EAGAIN)`（FFmpeg 会传给孩子，通常会导致读失败）——
所以**自定义网络 AVIO 一般阻塞读**，或者用 `AVIO_FLAG_NONBLOCK`。

### write

只读流传 `nullptr` 即可。写的时候语义对称，`0` 表示写不进去。

### seek

```c
int64_t seek(void *opaque, int64_t offset, int whence);
```

- `whence == AVSEEK_SIZE`：**返回总长度**，不是"跳"（`avio_size()` 走这里）。
  不知道长度就返回 `AVERROR(ENOSYS)`。
- `whence == SEEK_SET / SEEK_CUR / SEEK_END`：跳完返回**新位置**。
- `whence` 里可能带 `AVSEEK_FORCE` 标志位，比较前要 `whence &= ~AVSEEK_FORCE`。
- 不支持 seek 就返回 `AVERROR(ESPIPE)`，并把 `pb->seekable = 0`。

**别在回调里"自作主张"修正非法偏移** —— 见第 9 节坑 3。

---

## 4. 最小可运行示例

把 `FILE*` 包成 AVIO，打开媒体文件并打印流信息
（`minimal_file_avio.cpp`）：

```cpp
#include <cerrno>
#include <cstdio>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

static int io_read(void *opaque, uint8_t *buf, int size)
{
    FILE *f = static_cast<FILE *>(opaque);
    const size_t n = std::fread(buf, 1, (size_t)size, f);
    if (n == 0)
        return std::feof(f) ? AVERROR_EOF : AVERROR(errno);
    return (int)n;
}

static int64_t io_seek(void *opaque, int64_t offset, int whence)
{
    FILE *f = static_cast<FILE *>(opaque);

    if (whence == AVSEEK_SIZE) {
        const long cur = std::ftell(f);
        std::fseek(f, 0, SEEK_END);
        const long end = std::ftell(f);
        std::fseek(f, cur, SEEK_SET);
        return end;
    }

    int origin;
    switch (whence) {
    case SEEK_SET: origin = SEEK_SET; break;
    case SEEK_CUR: origin = SEEK_CUR; break;
    case SEEK_END: origin = SEEK_END; break;
    default: return AVERROR(EINVAL);
    }
    if (std::fseek(f, (long)offset, origin) != 0)
        return AVERROR(errno);
    return std::ftell(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) { std::printf("用法: %s <media>\n", argv[0]); return 1; }

    FILE *f = std::fopen(argv[1], "rb");
    if (!f) { std::perror("fopen"); return 1; }

    constexpr int kBufSize = 32 * 1024;
    auto *buf = static_cast<unsigned char *>(av_malloc(kBufSize));

    AVIOContext *pb = avio_alloc_context(buf, kBufSize, 0, f,
                                         io_read, nullptr, io_seek);
    if (!pb) { std::printf("avio_alloc_context 失败\n"); return 1; }
    pb->seekable = AVIO_SEEKABLE_NORMAL;

    AVFormatContext *fmt = avformat_alloc_context();
    fmt->pb = pb;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;      // ★ 少了这行 close 时会双重释放

    if (avformat_open_input(&fmt, argv[1], nullptr, nullptr) < 0) {
        std::printf("avformat_open_input 失败\n");
        return 2;
    }
    avformat_find_stream_info(fmt, nullptr);

    std::printf("时长 %.2f 秒, %u 个流\n",
                (double)fmt->duration / AV_TIME_BASE, fmt->nb_streams);
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        std::printf("  #%u codec_type=%d\n", i,
                    fmt->streams[i]->codecpar->codec_type);

    avformat_close_input(&fmt);   // CUSTOM_IO：不会碰 pb

    pb->opaque = nullptr;         // ★ 先摘掉 FILE*，见第 8 节
    avio_context_free(&pb);       // 释放 buffer 和 pb 本身
    std::fclose(f);
    return 0;
}
```

编译（MSVC 示例，路径按本机 FFmpeg 改）：

```bat
cl /std:c++17 /utf-8 minimal_file_avio.cpp ^
   /I "C:\Program Files\ffmpeg\ffmpeg8.1\include" ^
   /link /LIBPATH:"C:\Program Files\ffmpeg\ffmpeg8.1\lib" ^
   avformat.lib avcodec.lib avutil.lib
```

跑通之后，把 `io_read` / `io_seek` 里加一行 `printf`，你会直观看到
FFmpeg 是怎么先读 32KB、再往回跳、再读的 —— 这是理解 AVIO 最快的方式。

---

## 5. 缓冲到底怎么工作

理解这三条，很多"奇怪行为"就解释得通了：

1. **`avio_read(pb, buf, n)` 先从 `pb->buffer` 里拷**。缓冲空了才调一次
   `read_packet`，而且一次要 `buffer_size` 字节（不是你要的 `n`）。
   这就是为什么回调一次能拿 32KB，而不是 4 字节一次。

2. **`avio_seek` 先看目标在不在当前缓冲里**（short seek）。在的话只挪
   `buf_ptr`，**根本不调你的 seek 回调**；不在才丢弃缓冲、调你的回调。
   所以你会在日志里看到"跳了很多次，但 seek 回调只被调了几次"。

3. **`avio_size()` 就是 `seek(pb, 0, AVSEEK_SIZE)`**；`avio_tell()` 返回
   `pb->pos`（扣掉缓冲里未读的部分）。

推论：**你的 read 回调必须和 seek 回调对同一个"位置概念"负责**。
read 读完 N 字节，内部位置就前进 N；seek 跳完，内部位置就是返回值。
两者不一致（比如 read 里偷偷跳了），就会出现"读到的数据和 `avio_tell` 对不上"。

---

## 6. 接到 avformat 上

```cpp
AVFormatContext *fmt = avformat_alloc_context();  // 必须先 alloc，再挂 pb
fmt->pb = pb;
fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
avformat_open_input(&fmt, "a.mp4", nullptr, nullptr);
```

- 为什么必须自己 `alloc`：`avformat_open_input` 只在 `*ps == NULL` 时才新建
  上下文，你要在它之前把 `pb` 塞进去。
- `AVFMT_FLAG_CUSTOM_IO` 的作用：告诉 `avformat_close_input` **不要**释放 `pb`
  （正常路径下 pb 是它打开的，它负责关）。少了这行 = 双重释放。
- 第二个参数（URL）仍然要传：FFmpeg 会用它猜容器格式（看扩展名），
  但**不会真的去打开**它。传 `"fd"` 这种没有扩展名的名字，就只能靠内容探测。

---

## 7. 生命周期

```text
av_malloc(buffer)
    ↓
avio_alloc_context(...)          ← 失败要自己 av_free(buffer)
    ↓
fmt->pb = pb; flags |= CUSTOM_IO
    ↓
avformat_open_input / av_read_frame ...   （pb 必须一直活着）
    ↓
avformat_close_input(&fmt)       ← 不释放 pb
    ↓
pb->opaque = nullptr
avio_context_free(&pb)           ← 释放 buffer + pb
```

两条铁律：

1. **AVIO 必须活到解封装上下文关闭之后**。它是整条播放链的字节来源，
   demux 线程每次 `av_read_frame` 都可能回调它。
2. **buffer 不要自己 `free`**。`avio_context_free()` 会释放它
   （文档原话："free the IO context and everything associated with it"）。

---

## 8. opaque 的坑（跨版本）

`avio_context_free()` 对 `pb->opaque` 的处理，不同 FFmpeg 版本不一样：
有的会 `av_freep(&pb->opaque)`，有的不会。所以：

```cpp
// 如果 opaque 是自己 av_malloc 的，或者指向栈/静态对象：
pb->opaque = nullptr;   // 先摘掉，避免被它 av_free
avio_context_free(&pb);
```

如果你坚持让 FFmpeg 帮你释放 opaque，就必须保证它是 `av_malloc` 出来的
**独立内存**，不能是 `FILE*`、不能是栈地址、不能是成员指针。

**另一个坑：`av_malloc` 不跑构造函数。** 用 `av_malloc(sizeof(Struct))` 分配
带默认成员初始值的结构体时，那些默认值**不会生效**，读出来是随机值。
要么逐个字段赋值，要么 `new (ptr) Struct;`（placement new，且之后用
`av_free` 而不是 `delete`，前提是析构为空）。

---

## 9. 十二个坑（按踩到的顺序）

| # | 坑 | 现象 | 正确做法 |
|---|---|---|---|
| 1 | `av_malloc` 不跑构造函数 | 成员是随机值，比如 `pos` 巨大 → 一读就 EOF | 逐字段赋值 / placement new |
| 2 | `avio_seek()` 不接受 `SEEK_END` | 返回 `EINVAL` | 先 `avio_size()` 再 `SEEK_SET` |
| 3 | `avio_seek()` 返回的是**请求值**，不是回调的返回值 | 回调里夹住越界 seek → avio 的 `pos` 和 fd 实际位置不一致，后续读错位 | seek 回调不夹上界（越界读交给 read 的 EOF） |
| 4 | read 不按自定义 `size` 截断 | 读到"虚拟文件"之外的数据，解码器当码流解析 | 用 `left = size - pos` 截断 |
| 5 | 忘了 `AVFMT_FLAG_CUSTOM_IO` | close 时 `avformat_close_input` 把你的 pb 也释放 → 崩溃/双重释放 | 挂 pb 时置 flag |
| 6 | 建完 AVIO 没定位到起点 | 从文件头读，前面有垃圾就探测失败 | 建完 `lseek(start, SEEK_SET)` |
| 7 | 探测大小时把位置挪到末尾 | 之后从尾读 | 探测完重新 seek |
| 8 | 不可 seek 时返回 0 长度 | 被当成 0 字节文件 / 探测死循环 | `size = -1` + `AVERROR(ESPIPE)` |
| 9 | read 返回 0 表示"暂时没数据" | 被当成 EOF，播放提前结束 | 暂时没数据就阻塞，或返回 `AVERROR(EAGAIN)` |
| 10 | 在 seek 里 `whence` 直接比较 | 带 `AVSEEK_FORCE` 时匹配失败 | `whence &= ~AVSEEK_FORCE` |
| 11 | 关掉 fd 后继续用 AVIO | `EBADF` / 花屏 | fd 归调用方，播放期间保持打开 |
| 12 | 多线程同时读同一个 AVIO | 偶发读半截 | AVIO 不是线程安全的，一个流一个线程独占 |

---

## 10. 排查套路

**症状 → 可能原因**

| 症状 | 先看什么 |
|---|---|
| `avformat_open_input` 报 `Invalid data found when processing input` | read 返回的内容不对 / 起点没定位 / 返回了 0 导致提前 EOF |
| `Could not find codec parameters` | seek 返回的位置不对（探测阶段跳错地方） |
| 时长是 `NOPTS` / 0 | `AVSEEK_SIZE` 没实现或返回 -1 |
| 能打开、一播就卡住 | read 阻塞但没有中断机制 / 返回 0 被当 EOF 后死循环重试 |
| 退出时崩溃在 `avformat_close_input` | 忘了 `AVFMT_FLAG_CUSTOM_IO`（double free） |
| 退出时崩溃在 `avio_context_free` | opaque 被释放了两次（自己 free 了又让它 free） |

**调试手段**

1. 在 `read`/`seek` 里打日志：`printf("read %d -> %d, seek %lld/%d\n", ...)`，
   跑一个文件就明白访问模式了。
2. 对照 `ffprobe`：同样的文件用 `ffprobe -v trace` 看它怎么探测。
3. 断点放在 `avio_read` / `avio_seek`（内部函数，需要符号）看调用栈。
4. 在 `avformat_open_input` 前后打印 `avio_size(pb)`、`avio_tell(pb)`。

---

## 11. 进阶练手（按难度）

1. **内存 AVIO**：把一段 `std::vector<uint8_t>` 当数据源，read/seek 直接在内存里
   挪指针。好处：测试快，能精确控制"读到一半返回短读"。
2. **日志 AVIO**：包一层现有的 `AVIOContext`，记录每次 read/seek 的大小和偏移，
   用来分析某个格式的探测行为。
3. **限速 AVIO**：read 里按时间片 sleep，模拟慢网络，观察播放器的缓冲策略。
4. **fd AVIO**：就是仓库里的 `core/fdavio.cpp`，可以对照着读。
5. **自己写协议**：`avio_open("myproto://x")` 需要注册 `URLProtocol`，
   比自定义 AVIO 更底层，通常是做新协议时才需要。

---

## 12. 为什么不用 `fd:` / `pipe:` 协议

- FFmpeg 没有可移植的 `fd:` 伪协议；
- `pipe:` 在 Windows 上只认 stdin/stdout（`pipe:0` / `pipe:1`），
  而且要先 `dup2`，鸿蒙/安卓的 picker 给的 fd 用不了；
- 自定义 AVIO 不依赖 URL 语义，跨平台（POSIX 的 `read/lseek`、Windows 的
  `_read/_lseeki64`）都能实现，还能顺便做 offset/size 裁剪。

所以移动端"picker 给 fd"的场景，自定义 AVIO 基本是标准答案。

---

## 13. 自检清单

看完能答上来就算过关：

1. `avio_read` 和你的 `read` 回调是什么关系？一次回调一般读多少？
2. `avio_size()` 内部走哪个回调？返回什么？
3. 为什么 `avio_seek(pb, x, SEEK_END)` 会失败？正确写法？
4. `AVFMT_FLAG_CUSTOM_IO` 少了会发生什么？
5. `pb->buffer` 由谁释放？`opaque` 呢？
6. 自定义 `size` 时，read 和 seek 各自要做什么（一个截断、一个不夹）？
7. 不可 seek 的流应该怎么告诉 FFmpeg？
8. 为什么 `av_malloc` 的结构体不能用默认成员初始值？
