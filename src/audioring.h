#ifndef AUDIORING_H
#define AUDIORING_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "core/iaudiooutput.h"

/*
 * AudioRing —— 单生产者 / 单消费者字节环（T4 Step 1）
 *
 * 为什么是 SPSC：
 *   写的一端永远只有 core 的音频播放线程（AudioThread::playRun → IAudioOutput::write），
 *   读的一端永远只有壳的音频线程（门面的 op_audio_read）。两个线程，不需要通用锁。
 *
 * 为什么计数器和数据分开保护：
 *   - write/read/clear 要 memcpy，持一把 mutex；它们分别是"每个音频帧一次"和
 *     "每次设备回调一次"的量级，竞争可以忽略。
 *   - available()/space() 会被 getNoPlayMs() 高频调用（core 的 getPts() 每轮循环都问），
 *     所以**只读原子计数器、绝不加锁**。
 *
 * 容量必须是 2 的幂：索引用 &(capacity-1)，避免高频取模的除法开销。
 *
 * 对齐：alignBytes 是一"采样帧"的字节数（channels × 2，S16 交错）。读写都向下取整到
 * 它的整数倍 —— 宁可这次少给几个字节，也不能让壳拿到半个采样帧（左右声道错位）。
 */
class AudioRing
{
public:
    explicit AudioRing(int capacityBytes, int alignBytes = 1);  // 容量向上取整到 2 的幂

    ~AudioRing();

    int  write(const uint8_t *data, int bytes); // 返回实际写入（满了返回 0，绝不阻塞）
    int  read (uint8_t *out,  int bytes);       // 返回实际读出
    int  available() const;                     // 可读字节（原子读，无锁）
    int  space()     const;                     // 可写字节
    void clear();                               // 丢积压
    int  capacity()  const;

    AudioRing(const AudioRing &) = delete;
    AudioRing &operator=(const AudioRing &) = delete;

private:
    std::mutex m_dataMutex;
    std::vector<uint8_t> m_buf;
    uint32_t m_mask  = 0;
    int      m_align = 1;

    // 自由递增的读写计数（不是索引）：used = head - tail，uint32 自然回绕
    std::atomic<uint32_t> m_head{0}, m_tail{0};
};

/*
 * AudioRingSink —— 把"设备"换成"环 + 壳来拉"的 IAudioOutput 实现（T4 Step 2）
 *
 * core 侧（生产者）：write()/getFree() 回压，和接真实声卡时一模一样，core 不用改。
 * 壳侧（消费者）：read() 拿 PCM 喂给 QAudioSink / AudioRenderer / AudioTrack。
 *
 * 时钟（B 方案的核心）：
 *   core 用 getNoPlayMs() 算播放位置：clock = 最后写入帧的 pts − getNoPlayMs() × speed。
 *   "还没播"的数据同时躺在**两个**地方：本 ring 里（未读走）+ 设备缓冲里（已读走未播）。
 *   ring 那份 SDK 自己知道；设备那份只有壳知道，所以由壳回传一个数：
 *
 *       sink.setDevicePendingMs(ms)      ← 门面包成 op_audio_set_device_pending_ms
 *
 *   两次上报之间设备一直在播，积压是**线性减少**的，所以这里"锚点 + 外推"，
 *   而不是把上报值当实时值 —— 否则时钟会变阶梯状，A/V 同步一顿一顿。
 *
 * 单位约定（别搞反）：getNoPlayMs() 返回**设备时间**（毫秒）；
 *   core 那边会乘 speed 换成内容时间，ring 侧不要自己乘。
 */
class AudioRingSink : public IAudioOutput
{
public:
    // 测试注入时钟（单调毫秒）。必须在 open() 之前设置。
    using ClockFn = std::function<int64_t()>;

    // 环容量：按 300ms 算，够吸收抖动；再大只会增加延迟
    static constexpr int     kCapacityMs         = 300;
    // 上报值的上限（夹紧用）：正常设备积压不会超过这个量级，超了就是壳算错了
    static constexpr int32_t kMaxDevicePendingMs = 2000;

    explicit AudioRingSink(int capacityBytes = 0); // 0 = 按 open() 的采样率算 300ms
    ~AudioRingSink();

    void setClock(ClockFn fn);

    bool open(int sampleRate, int channels) override;

    //关闭设备并释放句柄；可重复调用
    void close() override;

    //丢掉设备缓冲里还没播放的数据（seek / 切换文件时用，避免先播出旧位置的一小段）
    void clear() override;

    /*
     * 写入 PCM 数据，bytes 应按采样帧对齐（sampleSize/8 * channels）。
     * 返回值：
     *   > 0 : 实际写入的字节数
     *   = 0 : 设备缓冲已满（调用方应等待后重试，不要忙转）
     *   < 0 : 设备错误
     */
    int write(const uint8_t *data, int bytes) override;

    // 读 PCM 出去给壳。**没数据返回 0（不是错误）**，壳据此睡 2~5ms 再来。
    int read(uint8_t *out, int bytes);

    // 设备缓冲还能接收多少字节（回压用）；设备未打开时返回 0
    int getFree() override;

    /*
     * 设备缓冲里还没播放的时长（毫秒）——音频时钟的延迟补偿量。
     * 主时钟 = 最后写入那一帧的 PTS - getNoPlayMs() * speed
     * （暂停期间应保持不变，否则 resume 时时钟会跳）
     */
    long long getNoPlayMs() override;

    void setPause(bool isPause) override;

    // ---- 给门面 / 测试用 ----
    void    setDevicePendingMs(int32_t ms);   // 壳上报：设备里还压着多少毫秒
    int32_t devicePendingMs() const;          // 外推之后的当前值
    int     availableBytes() const;           // ring 里可读字节
    int     frameBytes()  const;              // 一采样帧字节数（channels × 2）
    int64_t bytesPerMs()  const;              // 参考值：48k/2ch = 192

private:
    int64_t nowMs() const;

    std::atomic<int>  m_capacityBytes{0};
    std::atomic<bool> m_isPaused{false};
    std::atomic<int>  m_sampleRate{0};
    std::atomic<int>  m_channels{0};
    AudioRing* m_auRing = nullptr;

    // 只保护"换环"这个动作（open/close）。write/read/getFree/getNoPlayMs 都在
    // open 之后、close 之前被音频线程调用，不加锁 —— 加锁会撞上 getPts() 的高频调用。
    std::mutex m_mutex;

    std::atomic<int32_t> m_devPendingMs{0};   // 壳最近一次上报的锚点值
    std::atomic<int64_t> m_devStampMs{0};     // 该上报的时刻
    std::atomic<int64_t> m_lastClearMs{0};    // 上次 clear() 的时刻（挡过期上报）

    ClockFn m_clock;
};

#endif // AUDIORING_H
