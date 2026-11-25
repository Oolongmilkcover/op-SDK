#include "audioring.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace
{

/* 向上取整到 2 的幂；太小给 256B（下限只用来挡 0/负数），上限 64MB
 * （防止采样率/声道传错时一次吃掉几十 GB） */
int roundUpPow2(int bytes)
{
    if (bytes < 256)      bytes = 256;
    if (bytes > (1 << 26)) bytes = 1 << 26;

    uint32_t v = (uint32_t)bytes - 1;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return (int)(v + 1);
}

} // namespace

/* ==================================================================== */
/*  AudioRing                                                           */
/* ==================================================================== */

AudioRing::AudioRing(int capacityBytes, int alignBytes)
    : m_align(alignBytes > 0 ? alignBytes : 1)
{
    const int cap = roundUpPow2(capacityBytes);
    m_buf.assign((size_t)cap, 0);
    m_mask = (uint32_t)(cap - 1);
}

AudioRing::~AudioRing() = default;

int AudioRing::write(const uint8_t *data, int bytes)
{
    if (!data || bytes <= 0) return 0;

    std::lock_guard<std::mutex> lock(m_dataMutex);

    int n = std::min(bytes, space());
    if (m_align > 1) n -= n % m_align;      // 只写整帧
    if (n <= 0) return 0;

    const uint32_t cap   = m_mask + 1;
    const uint32_t head  = m_head.load(std::memory_order_relaxed);
    const int      idx   = (int)(head & m_mask);
    const int      first = std::min(n, (int)(cap - (uint32_t)idx));

    std::memcpy(m_buf.data() + idx, data, (size_t)first);
    if (n > first) std::memcpy(m_buf.data(), data + first, (size_t)(n - first));

    m_head.store(head + (uint32_t)n, std::memory_order_release);
    return n;
}

int AudioRing::read(uint8_t *out, int bytes)
{
    if (!out || bytes <= 0) return 0;

    std::lock_guard<std::mutex> lock(m_dataMutex);

    int n = std::min(bytes, available());
    if (m_align > 1) n -= n % m_align;      // 只读整帧
    if (n <= 0) return 0;

    const uint32_t cap   = m_mask + 1;
    const uint32_t tail  = m_tail.load(std::memory_order_relaxed);
    const int      idx   = (int)(tail & m_mask);
    const int      first = std::min(n, (int)(cap - (uint32_t)idx));

    std::memcpy(out, m_buf.data() + idx, (size_t)first);
    if (n > first) std::memcpy(out + first, m_buf.data(), (size_t)(n - first));

    m_tail.store(tail + (uint32_t)n, std::memory_order_release);
    return n;
}

int AudioRing::available() const
{
    const uint32_t head = m_head.load(std::memory_order_acquire);
    const uint32_t tail = m_tail.load(std::memory_order_acquire);
    return (int)(head - tail);              // uint32 自然回绕
}

int AudioRing::space() const
{
    return capacity() - available();
}

void AudioRing::clear()
{
    std::lock_guard<std::mutex> lock(m_dataMutex);
    // 从读端看就是"全部丢掉"：把尾巴追到头（写端不动，生产者不用配合）
    m_tail.store(m_head.load(std::memory_order_acquire), std::memory_order_release);
}

int AudioRing::capacity() const
{
    return (int)(m_mask + 1);
}

/* ==================================================================== */
/*  AudioRingSink                                                       */
/* ==================================================================== */

AudioRingSink::AudioRingSink(int capacityBytes)
    : m_clock([] {
          using namespace std::chrono;
          static const auto origin = steady_clock::now();
          return (int64_t)duration_cast<milliseconds>(steady_clock::now() - origin).count();
      })
{
    m_capacityBytes.store(capacityBytes);   // 0 = 由 open() 按采样率自动算 kCapacityMs
    m_lastClearMs.store(nowMs(), std::memory_order_relaxed);
    m_devStampMs.store(nowMs(), std::memory_order_relaxed);
}

AudioRingSink::~AudioRingSink()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    delete m_auRing;
    m_auRing = nullptr;
}

void AudioRingSink::setClock(ClockFn fn)
{
    if (fn) m_clock = std::move(fn);
    // 换了时钟，之前的时间戳就没意义了，全部重新锚定
    m_lastClearMs.store(nowMs(), std::memory_order_relaxed);
    m_devStampMs.store(nowMs(), std::memory_order_relaxed);
}

int64_t AudioRingSink::nowMs() const
{
    return m_clock ? m_clock() : 0;
}

bool AudioRingSink::open(int sampleRate, int channels)
{
    if (sampleRate <= 0 || channels <= 0 || channels > 8) return false;

    const int frame = channels * 2;          // S16 交错：一帧 = 声道数 × 2 字节

    int cap = m_capacityBytes.load(std::memory_order_relaxed);
    if (cap <= 0)                            // 没指定就按 kCapacityMs 算
        cap = (int)((int64_t)sampleRate * frame * kCapacityMs / 1000);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        delete m_auRing;                     // 重复 open（换文件）时把旧环整个换掉
        m_auRing = new AudioRing(cap, frame);
    }

    m_sampleRate.store(sampleRate);
    m_channels.store(channels);
    m_isPaused.store(false);
    m_devPendingMs.store(0, std::memory_order_relaxed);
    m_devStampMs.store(nowMs(), std::memory_order_relaxed);
    m_lastClearMs.store(nowMs(), std::memory_order_relaxed);
    return true;
}

void AudioRingSink::close()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        delete m_auRing;
        m_auRing = nullptr;
    }
    m_sampleRate.store(0);
    m_channels.store(0);
    m_isPaused.store(false);
    m_devPendingMs.store(0, std::memory_order_relaxed);
    // 注意：不清 m_capacityBytes —— 那是"调用方指定的容量"，重开时要继续用
}

void AudioRingSink::clear()
{
    if (m_auRing) m_auRing->clear();

    /*
     * 设备那一半也必须归零。协议是"seek 后壳先 flush 设备、再上报新积压"，
     * 这里清掉锚点，并靠 devicePendingMs() 里"不能超过 clear 以来这段时间"的夹紧，
     * 挡住"seek 前发出、seek 后才到"的过期上报 —— 否则时钟会被拽回旧位置。
     */
    m_devPendingMs.store(0, std::memory_order_relaxed);
    m_lastClearMs.store(nowMs(), std::memory_order_relaxed);
}

int AudioRingSink::write(const uint8_t *data, int bytes)
{
    if (!m_auRing || !data || bytes <= 0) return 0;
    if (m_isPaused.load(std::memory_order_acquire)) return 0;
    // 满了返回实际写入（可能是 0），绝不阻塞：core 靠 getFree()/返回值回压
    return m_auRing->write(data, bytes);
}

int AudioRingSink::read(uint8_t *out, int bytes)
{
    /*
     * 没数据返回 0（不是错误），壳据此睡 2~5ms 再来。
     * 这里**不要返回负数**：门面的 op_audio_read 把 <0 解释成 op_result 错误码，
     * -1 会被当成 OP_ERR_INVALID_ARG，语义全乱。
     */
    if (!m_auRing || !out || bytes <= 0) return 0;
    if (m_isPaused.load(std::memory_order_acquire)) return 0;
    return m_auRing->read(out, bytes);
}

int AudioRingSink::getFree()
{
    return m_auRing ? m_auRing->space() : 0;
}

long long AudioRingSink::getNoPlayMs()
{
    if (!m_auRing) return 0;

    const int sr = m_sampleRate.load(std::memory_order_relaxed);
    const int ch = m_channels.load(std::memory_order_relaxed);
    if (sr <= 0 || ch <= 0) return 0;

    const int64_t bytesPerSec = (int64_t)sr * ch * 2;
    const int64_t ringMs = (int64_t)m_auRing->available() * 1000 / bytesPerSec;

    /*
     * 返回**设备时间**：ring 里还没被读走的 + 壳上报的"已读走但设备还没播"的。
     * core 的 getPts() 会自己乘 speed 换成内容时间，这里别乘。
     */
    return (long long)(ringMs + devicePendingMs());
}

void AudioRingSink::setPause(bool isPause)
{
    if (m_isPaused.load(std::memory_order_acquire) == isPause) return;

    /*
     * 先把"按旧状态外推出来的当前值"固化，再切标志位。
     *
     * 顺序反了会有两个坑：
     *   - 先切标志：暂停瞬间外推停止，读出来的是**锚点**而不是当前值 → 积压突然变大，时钟倒退；
     *   - 恢复时不重新锚定：暂停那几秒会被当成"设备播掉了这么多"，积压瞬间归零 → 时钟跳。
     */
    const int32_t frozen = devicePendingMs();
    m_devPendingMs.store(frozen, std::memory_order_release);
    m_devStampMs.store(nowMs(), std::memory_order_relaxed);
    m_isPaused.store(isPause, std::memory_order_release);
}

void AudioRingSink::setDevicePendingMs(int32_t ms)
{
    if (ms < 0) ms = 0;
    if (ms > kMaxDevicePendingMs) ms = kMaxDevicePendingMs;

    m_devStampMs.store(nowMs(), std::memory_order_relaxed);
    m_devPendingMs.store(ms, std::memory_order_release);
}

int32_t AudioRingSink::devicePendingMs() const
{
    const int32_t anchor = m_devPendingMs.load(std::memory_order_acquire);
    if (anchor <= 0) return 0;

    int64_t ms = anchor;

    // ① 暂停期间设备不播，积压冻结；播放中按真实时间线性减少
    if (!m_isPaused.load(std::memory_order_acquire)) {
        const int64_t age = nowMs() - m_devStampMs.load(std::memory_order_acquire);
        if (age > 0) ms -= age;
    }

    // ② 不能为负（设备播完就是 0，外推不能把它推成负数）
    if (ms < 0) ms = 0;

    /*
     * ③ 不能超过"上次 clear 以来经过的时间"。
     *    这条专门挡过期上报：seek 时 clear() 视为设备已 flush，此后设备里最多
     *    只能压着这段时间新写进去的量。少了它，一条晚到的旧上报就能把时钟拽回 seek 之前。
     */
    const int64_t sinceClear = nowMs() - m_lastClearMs.load(std::memory_order_acquire);
    if (ms > sinceClear) ms = sinceClear;
    if (ms < 0) ms = 0;

    return (int32_t)ms;
}

int AudioRingSink::availableBytes() const
{
    return m_auRing ? m_auRing->available() : 0;
}

int AudioRingSink::frameBytes() const
{
    const int ch = m_channels.load(std::memory_order_relaxed);
    return ch > 0 ? ch * 2 : 0;
}

int64_t AudioRingSink::bytesPerMs() const
{
    const int sr = m_sampleRate.load(std::memory_order_relaxed);
    const int ch = m_channels.load(std::memory_order_relaxed);
    if (sr <= 0 || ch <= 0) return 0;
    // 48000×2×2/1000 = 192（整数精确）；44100 时是 176.4，所以只是"参考值"，
    // 真正换算一律用 bytes×1000/(sr×ch×2)，别拿这个值做反向整除。
    return (int64_t)sr * ch * 2 / 1000;
}
