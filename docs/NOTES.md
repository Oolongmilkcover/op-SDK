# 开发笔记

> 这里只记**"为什么这么设计"**：推演过的选择、踩过的坑、以及不写下来下次一定会忘的账。
> 分阶段计划与验收标准在 `docs/SDK-PLAN.md`，给助手的作业说明在 `AGENTS.md`。

## 索引

| 日期 | 主题 | 相关代码 |
|---|---|---|
| 2026-11-22 | AudioRing：把 push 的声卡换成 pull 的环 | `src/audioring.h` `.cpp`、`tests/test_audioring.cpp` |
| 2026-11-23 | 自定义 AVIO：把文件描述符变成 FFmpeg 的输入 | `core/fdavio.h` `.cpp`、`tests/test_fdavio.cpp` |

---

## 2026-11-22 · AudioRing：把 push 的声卡换成 pull 的环

> T4 Step 1/2。本文对应的代码：`src/audioring.h`、`src/audioring.cpp`、`tests/test_audioring.cpp`。

### 1. 要解决的问题

内核的音频出口 `IAudioOutput` 是 **push** 模型：core 的解码线程往设备里写，
设备通过 `getFree()` 告诉它"还剩多少空间"，通过 `getNoPlayMs()` 告诉它"已经写进来
但还没播出去的有多少毫秒"。桌面的 `QAudioSink` 就是这个形状。

而鸿蒙的 `AudioRenderer`、安卓的 `AudioTrack` 都是 **pull**：设备（或它背后的
回调线程）主动来要数据，壳在自己的音频线程里 `read` 一块、塞给设备。

矛盾点：**不能为了 pull 重写 `AudioThread`**。音频解码、重采样、atempo 倍速、
时钟计算全在 core 里，重写一遍等于把内核复制成两份。

于是有了这套"假设备"：

```text
core 音频播放线程
    │  IAudioOutput::write()   （push，和接真声卡时一模一样）
    ▼
[ AudioRingSink ]  ── 内部一个 SPSC 字节环 AudioRing ──
    │  read()                 （pull，壳在自己的音频线程里调）
    ▼
QAudioSink / AudioRenderer / AudioTrack
    │
    └── setDevicePendingMs(ms)   壳回传"设备里还压着多少毫秒"
```

**core 一行都没改**：`IAudioOutput` 还是那个接口，`AudioThread` 根本不知道
对面是声卡还是一个环。这是整个 T4 能只做"包装"的前提。

### 2. 关键设计

#### 2.1 SPSC，而不是通用环形缓冲

写的一端**永远**只有 core 的音频播放线程（`AudioThread::playRun`），
读的一端**永远**只有壳的音频线程（门面的 `op_audio_read`）。两个线程，
没有第三个。

这决定了不需要无锁队列那套 CAS/序号/重试：数据区用一把 `std::mutex` 包住
`memcpy` 就够了——写是"每个音频帧一次"，读是"每次设备回调一次"，量级都很低。

真正的巧处在**计数器和数据分开保护**：

| 操作 | 频率 | 代价 |
|---|---|---|
| `write` / `read` / `clear` | 每音频帧一次 / 每次设备回调一次 | 持锁 + memcpy |
| `available()` / `space()` | **很高**：`getNoPlayMs()` 会被 core 的 `getPts()` 每轮循环调用 | **只读原子量，绝不加锁** |

如果 `available()` 也要抢那把手锁，`getPts()` 就会和音频线程天天撞车，
而它偏偏在 demux 的关键路径上。所以 `m_head` / `m_tail` 是 `std::atomic<uint32_t>`，
读侧只要两个 `memory_order_acquire` 的 load 就能算出来。

#### 2.2 自由递增的 uint32 计数 + 2 的幂容量

`head` / `tail` **不是**数组下标，而是从 0 开始一直加、加到 2³² 自然回绕的计数：

```cpp
used  = head - tail;        // uint32 自然回绕，不用处理"跨过 0"
index = head & m_mask;      // m_mask = capacity - 1
```

这样同时解决三件事：

1. **空/满无歧义**：经典环形缓冲用"`head == tail` 既可能是空也可能是满"，
   这里因为计数永不回绕清零，`used` 就是真实字节数，`used == 0` 一定是空。
2. **没有取模除法**：容量是 2 的幂，`& (cap - 1)` 代替 `% cap`。音频路径上
   每帧都要算，除法不能留。
3. **一次最多复制两段**：`first = min(n, cap - idx)`，剩余部分从数组头续上。

容量由 `roundUpPow2()` 向上取整，并夹在 `[256B, 64MB]`——下限只是挡 0/负数，
上限是防止采样率/声道传错时一次吃掉几十 GB。

#### 2.3 时钟：**两处积压**，锚点外推，三重夹紧

这是 B 方案的全部内容，也是最容易做错的地方。

core 的音频时钟是：

```text
clock = 最后写入那一帧的 pts − getNoPlayMs() × speed
```

关键在于"还没播出去"的字节**同时躺在两个地方**：

- **ring 里还没被壳读走的** —— SDK 自己知道，`available() × 1000 / (sr×ch×2)`；
- **壳已经读走、但设备还没播的** —— 只有壳知道，必须回传。

少算第二项，时钟就会超前，视频会判定"自己落后音频"然后疯狂丢帧
（实测过：3 秒只渲染 6 帧）。所以：

```cpp
getNoPlayMs() = ringMs + devicePendingMs()
```

第二个问题是：**壳的上报是离散的，而设备是连续播放的**。
如果把上报值当实时值用，时钟会变成阶梯状，A/V 同步一顿一顿。
所以 `setDevicePendingMs()` 存的只是"锚点"：

```cpp
devicePendingMs() = clamp(anchor − (now − stamp), 0, now − lastClear)
```

三重夹紧，每一重都对应一个真实故障：

| 夹紧 | 挡的是什么 |
|---|---|
| `≥ 0` | 设备早播完了，外推不能把它推成负数（时钟倒退） |
| 暂停期间不外推 | 暂停时设备不播，积压应该冻结；否则恢复时积压瞬间归零，时钟跳 |
| `≤ now − lastClear` | **过期上报**：seek 前发出、seek 后才到的旧值，会把时钟拽回 seek 之前 |

第三重是最妙的：它没有引入"seek 序列号 / 上报代际"这类机制（那要改 C 接口），
而是把"seek 时 `clear()` 视为设备已 flush"当作一个**单调约束**——clear 之后
设备里最多只能压着这段时间新写进去的量。一条迟到的旧上报，数值上会被这个
上界削掉，不需要任何版本号。

#### 2.4 暂停的顺序：① 固化 → ② 重锚 → ③ 切标志

暂停是唯一会**改变外推方向**的输入，慢一步就是 A/V 跳一下。`setPause()` 只能这么写：

```cpp
void AudioRingSink::setPause(bool isPause)
{
    if (m_isPaused.load() == isPause) return;

    const int32_t frozen = devicePendingMs();   // ① 按**旧**状态外推出来的"现在"
    m_devPendingMs.store(frozen);               // ② 把"现在"固化成新锚点
    m_devStampMs.store(nowMs());                //    时间戳锚到这一刻
    m_isPaused.store(isPause);                  // ③ 最后才切状态
}
```

暂停和恢复走的是同一段代码，但两者固化/重锚的对象不同：

- **暂停时**：① 还在按"播放中"外推（标志没切），所以 `frozen` 是暂停那一刻的
  **当前值**，不是锚点；② 把它定为锚点、时间戳定为现在；③ 切 true 之后
  `devicePendingMs()` 不再做减法，读到的一直是这个 `frozen`。
- **恢复时**：`devicePendingMs()` 因为仍在暂停态返回 `frozen`；② 再把时间戳锚到
  恢复这一刻；③ 切 false 之后外推从零开始算。

三种做错的后果：

| 做错 | 后果 |
|---|---|
| 先切标志、再读 `frozen`（①②③ 顺序颠倒） | 读的是**锚点**而不是当前值 → 积压突然变大 → `getNoPlayMs()` 变大 → 时钟**倒退** |
| 只切标志，不重锚时间戳 | 恢复时 `now − stamp` 把整段暂停时间都算成"设备播掉了" → 积压夹到 0 → 时钟**前跳**（跳掉整个暂停时长） |
| 暂停期间继续外推（不固化） | 积压一路掉到 0，恢复瞬间时钟前跳 N 秒，然后视频疯狂追帧 |

核心一句话：**暂停期间设备确实不播，但墙上的时钟一直在走**。所以要先把"暂停前
外推到的值"冻住，再把时间基准挪到"切状态的那一刻"——两件事的顺序不能反，
而且必须一起做。

测试第 6 组就是验这个：暂停时积压 100ms，时钟往前跳 **5 秒**，断言 `devicePendingMs()`
仍然是 100；恢复后再走 20ms，断言变成 80（只减了这 20ms，而不是把暂停的 5 秒也算进去）。

### 3. 一堆小决定（每条都对应一个具体的坑）

| 决定 | 为什么 |
|---|---|
| `write()` 满了返回 0，**绝不阻塞** | core 的音频线程阻塞了就是死锁；它有 1ms 轮询 + `getFree()` 回压 |
| `read()` 没数据返回 **0，不是 -1** | 门面的 `op_audio_read` 把 `<0` 当 `op_result`，-1 正好是 `OP_ERR_INVALID_ARG`，语义全乱 |
| 读写都按 `align = channels × 2` 向下取整 | 宁可这次少给几字节，也不能让壳拿到半个采样帧——左右声道会错位 |
| `clear()` 只把 `tail` 追到 `head` | 从读端看就是"全部丢掉"，写端（生产者）完全不用配合 |
| `open()` 里 `delete` 旧环再 `new` 新环 | 换文件时采样率可能变，环容量和帧对齐都得跟着变 |
| `close()` **不清** `m_capacityBytes` | 那是"调用方显式指定的容量"，重开时要继续用它；只有"按采样率自动算"的那次才该重算 |
| `getNoPlayMs()` 返回**设备时间**，不乘 speed | core 的 `getPts()` 会乘 `speed` 换成内容时间；两边都乘就乘了两次 |
| `bytesPerMs()` 只是参考值 | 48k/2ch 是整数 192，但 44.1k 是 176.4；换算一律用 `bytes × 1000 / (sr × ch × 2)`，别拿它做反向整除 |
| `setClock()` 给测试注入单调时钟 | 单测不能靠 `sleep` 验外推，会把测试拖到秒级且不稳定 |
| 构造后立刻记一次 `m_lastClearMs` / `m_devStampMs` | 否则第一秒内的夹紧上界是个巨大的随机值（未初始化） |

### 4. 测试覆盖（`tests/test_audioring.cpp`，8 组）

| # | 验的是 |
|---|---|
| 1 | 容量 / 写读 / space |
| 2 | 绕回 / 帧对齐 / 写满 / 数据完整性（逐字节比对） |
| 3 | `clear()` |
| 4 | ring 字节 → 毫秒（含 44.1k 这种非整数 `bytesPerMs`） |
| 5 | 设备积压的外推与三重夹紧 |
| 6 | 暂停冻结 / 恢复重锚 |
| 7 | `clear()` 之后过期上报被削掉 |
| 8 | **SPSC 并发**：1MB 强制绕回，生产者/消费者各一线程，校验字节序列与总量 |

第 8 组是唯一的多线程用例，它证明"计数器原子 + 数据加锁"这个混合方案在真实
竞争下不丢字节、不读脏。

### 5. 后来接上了（T4 Step 3/4，2026-11-23 补）

写这篇时它还是"零件"，现在已经接完，过程里有两点值得记：

- **采样率打通**：`CMD_OPEN_OBJ` 增加 `outSampleRate/outChannels` →
  `DemuxThread::openFile(..., sr, ch)` → `AudioThread::open(stream, out, sr, ch)`，
  在 `resampleInit()` 和 `m_out->open(sr, ch)` **之前**赋值；`AudioThread::open`
  现在拒绝 `sr <= 0 || ch <= 0`。
- **门面接线**：`op_player` 持有 `AudioRingSink`（声明在 `PlayerCore` **之前**，
  保证比 core 活得久），`op_open` 把它当 `CMD_OPEN_OBJ::out` 传进去；
  `read/availableBytes/getNoPlayMs` 分别对上 `op_audio_read/pending/latency_ms`；
  新增 `op_audio_set_device_pending_ms` 给壳回传设备积压。

**踩到一个真 bug**：`op_player::outSampleRate/outChannels` 的初始值是 0，而头文件
承诺默认 48000/2。于是"没调过 `op_set_audio_format` 的播放器"在 `AudioThread::open`
里被 `sr<=0` 拒绝，整条音频静默降级 —— 表现是 `OPENED` 里 `has_audio=0`，
看起来像"这个文件没有音轨"。是 T5 的 fd 端到端测试（合成 WAV → 必须能拉到 PCM）
把它抓出来的。默认值必须真的是 48000/2。

**验收边界**：单测能证明"接线通、确实能拉到 PCM"，但 AGENTS 里那两条
（10 秒 PCM 用 `ffplay` 播、带音轨视频 3 秒 ≈ 60 帧）要真实媒体文件和人耳/肉眼，
headless ctest 替代不了 —— 它们验的是时钟，不是接线。

---

## 2026-11-23 · 自定义 AVIO：把文件描述符变成 FFmpeg 的输入

> T5。代码：`core/fdavio.h` `.cpp`、`core/demuxthread.cpp` 的 `openFile()`、
> `tests/test_fdavio.cpp`。AVIO 本身的概念和最小示例见配套的《AVIO学习笔记》
> （独立文档，不在本仓库）；这里只记**本项目里的决定和踩过的坑**。

### 1. 问题：picker 只给 fd

鸿蒙 picker 给 URI、安卓给 `content://`，NDK 侧一般只能 `open()` 出一个 fd。
`avformat_open_input()` 只认路径/URL，只有三条路：

| 方案 | 代价 |
|---|---|
| 先把 fd 拷进沙箱、再按路径打开 | 几十 MB 的视频白拷一遍，移动端很明显 |
| 用 `pipe:` 或自定义协议名 | 不可移植；Windows 的 `pipe:` 只认 stdin/stdout，鸿蒙/安卓的 fd 用不了 |
| **自己造 AVIOContext** | 多写两百行，但没有拷贝、没有平台依赖 |

选第三条。核心思路：AVIO 就是 FFmpeg 的"字节流接口"，把 fd 包成它，
解封装层根本不知道对面不是文件。

### 2. 结构：三层各管一件事

```text
op_open_options{fd, offset, size}
        │  门面：翻译成 core::FdInput（栈上，openFile 返回即失效）
        ▼
DemuxThread::openFile(..., const FdInput*)
        │  建 AVIO、挂 pb、置 CUSTOM_IO；整个播放期间持有
        ▼
core::openFdAvio()  ←→  fdRead / fdSeek  ←→  调用方的 fd
```

- `core/fdavio.*` 只懂"fd → 字节流"（读、seek、大小），不知道 MP4 是什么；
- `DemuxThread` 只懂"把 AVIO 挂到 `fmt_ctx` 上、活到关闭、及时释放"；
- 门面只懂"把 C 结构体翻译成 `FdInput`"。

这样以后要支持"内存 buffer 当输入"（比如从网络下载到内存再播），
只需要再写一个 `openMemAvio()`，另外两层不用动。

### 3. 设计要点

#### 3.1 "虚拟文件"：offset / size 的映射

```text
物理文件:  [0 ...... offset ...... offset+size ...... EOF)
虚拟文件:             [0 ................ size)
```

`fdRead` / `fdSeek` 里所有位置都做"减 offset"，`AVSEEK_SIZE` 返回 `size` 而不是
物理文件大小。这样 `fd_offset=100` 的场景（容器嵌在别的文件里、picker 给了偏移）
对 FFmpeg 就是"一个从 0 开始的普通文件"。

size 的探测：优先 `fstat`（普通文件，且不动 fd 位置），不是普通文件才退化成
`lseek(0, SEEK_END)`；仍失败就按"不可 seek"处理。

#### 3.2 读要截断，seek **不夹**上界

看起来对称，实际必须不对称：

- **read 必须在 `start+size` 处停**。`fd_size` 的语义是"只暴露文件的一段"，
  越过它读到的就是别人的数据，解码器会把它当码流解析。
- **seek 不能夹上界**。`avio_seek()` 会把**它请求的 offset** 记进 `s->pos`
  并原样返回，而不是采用回调的返回值（见 `libavformat/aviobuf.c`）。
  回调私自夹住，avio 认为的位置和底层 fd 就对不上，后续读从错的地方开始。
  越过末尾是合法的，读完自然是 EOF，边界由 read 挡。

#### 3.3 不可 seek 的降级

管道/套接字：`size = -1`、seek 回调对非 `AVSEEK_SIZE` 返回 `AVERROR(ESPIPE)`、
`pb->seekable = 0`。能打开、能读，但没有时长、不能 seek、不能逐帧 ——
这些都是"能跳到指定位置"才成立的功能。宁可降级也不要假装能 seek。

#### 3.4 opaque 的释放：跨版本兼容

`avio_context_free()` 的文档是 "free the IO context and everything associated
with it"，不同版本对 `pb->opaque` 的处理不一致（有的 `av_freep(&pb->opaque)`，
有的不动）。所以：

```cpp
FdIo *c = (FdIo*)pb->opaque;
pb->opaque = nullptr;    // 先摘掉
av_free(c);              // 自己释放
avio_context_free(&pb);  // 它只释放 buffer 和自身
```

先摘空再释放，两种行为都不会双重释放、也不会泄漏。
另外 `av_malloc` **不跑构造函数**，`FdIo` 的成员默认值不生效，必须逐字段赋值
（漏了 `pos` 就表现为"一读就 EOF"）。

#### 3.5 失败路径集中收口

`openFile()` 有 5 条失败路径：alloc context 失败、`avformat_open_input` 失败、
`avformat_find_stream_info` 失败、找不到流、解码器打开失败。每条都要释放 AVIO，
漏一条就是"打开失败后泄漏"（而且只在特定格式上出现，很难查）。
所以集中成一个幂等的 `releaseFdAvio()`，在 5 条路径 + `close()` + 析构里都调。

释放顺序固定：**先 `avformat_close_input()`，再 `closeFdAvio()`**。
AVIO 必须活到解封装上下文关闭之后 —— demux 线程整个播放期间都在从它读。

### 4. 踩到的坑（全部由测试失败暴露）

| # | 坑 | 表现 | 正确做法 |
|---|---|---|---|
| 1 | `av_malloc` 不跑构造函数 | `pos` 是随机值，第一次读就 EOF，整组测试全红 | 逐个字段赋值，或 placement new |
| 2 | `avio_seek()` 不接受 `SEEK_END` | 直接 `EINVAL` | 先 `avio_size()` 再 `SEEK_SET` |
| 3 | `avio_seek()` 返回请求值而不是回调返回值 | 回调夹上界 → avio 与 fd 位置不一致 | seek 只挡下界，不夹上界 |
| 4 | read 不按 `size` 截断 | `fd_size` 形同虚设，读到虚拟文件之外 | `left = size - pos`，`left<=0` 返回 `AVERROR_EOF` |
| 5 | 忘了 `AVFMT_FLAG_CUSTOM_IO` | `avformat_close_input` 顺手把 `pb` 也释放 → 双重释放 | 挂 `pb` 时同时置 flag |
| 6 | 建完 AVIO 没定位到 offset | 从文件头读，WAV 头前的垃圾让探测失败 | 建完 `lseek(start, SEEK_SET)` |
| 7 | 探测 size 把 fd 挪到末尾 | 不重新定位就从尾读 | 探测后统一 seek 一次 |
| 8 | 探测 size 失败时返回 0 而不是 -1 | 当成 0 字节文件 / 探测死循环 | 不可 seek 用 -1，并让 seek 返回 `ESPIPE` |
| 9 | 试图在 seek 回调里"修"非法偏移 | 位置越修越乱 | 越界交给 read 的 EOF |
| 10 | 顺手 `close(fd)` | 播放中途 `EBADF` | fd 归调用方，SDK 只读不关 |
| 11 | 在回调里访问别的线程正在写的数据 | 偶发读到半截 | AVIO 不是线程安全的；我们只在 open/demux 两段独占 |

### 5. 测试怎么设计的（为什么不用真 mp4）

两个层次，都不需要仓库里放媒体文件：

1. **`tests/test_fdavio.cpp`**：直接对 AVIO 断言。造一个"512 字节垃圾 + 10KB
   有规律数据"的临时文件，用 `avio_read` / `avio_seek` / `avio_size` 验证
   offset、size、EOF、越界、幂等释放。它同时是"AVIO 怎么用"的可执行文档。
2. **`tests/test_api.cpp` 第 9 节**：现场合成一个"100 字节垃圾 + 1 秒 8kHz 单声道
   PCM"的 WAV，只给 `fd + fd_offset=100`，断言 `OPENED` 的 `has_audio=1`、
   `duration≈1000ms`，并且真的能 `op_audio_read` 出 PCM。
   这一条同时验了 T5 和 T4 的接线。

### 6. 写进 C 接口的契约

`op_open_options` 的注释里补了三条，都是"调用方不知道就会踩"的：

- **fd 归调用方，SDK 不 close**；但**播放期间必须一直开着**（demux 线程全程在读）。
- SDK 会**移动 fd 的读写位置**（探测、seek）。播放期间别在别处用同一个 fd，
  要并行读就 `dup()` 一份。
- 只支持可 seek 的普通文件；管道能打开但没有时长、不能 seek。
- `path` 在有 fd 时变成**可选的扩展名提示**（`"a.mp4"` 让探测少猜几次），可传 NULL。
