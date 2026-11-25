#ifndef AUDIOPLAYER_H
#define AUDIOPLAYER_H

#include <QObject>
#include "include/op/op_player.h"
#include <atomic>
#include <mutex>
#include <vector>

class QAudioSink;
class QIODevice;
class QTimer;

/*
 * Qt 侧音频出口：**SDK 拉模式 → 声卡推模式** 的桥。
 *
 *   core 解码线程 ──write()──> SDK 环形缓冲 ──op_audio_read()──> 本类
 *        ──QIODevice::write()──> QAudioSink ──> 声卡
 *
 * 为什么不用"给 QAudioSink 一个只读 QIODevice、让它来 readData"：
 * 实测 Qt 6.11 的 pull 模式在 readData 返回 0（欠载）之后就不再回调了，
 * 音频只被拉几次就彻底安静 → 环形缓冲写满 → 音频时钟不前进 → 视频冻住。
 *
 * 为什么用 QTimer 而不是 std::thread：
 * QAudioSink 不是线程安全的。实测在**非 GUI 线程**调 bytesFree()，
 * 当 sink 还停在 IdleState（还没写第一批数据）时会永久阻塞 ——
 * 工作线程卡死 → 环形缓冲堵满 → 音频时钟停 → 视频冻住。
 * 放到 GUI 线程的定时器里，所有 QAudioSink 调用都在对象所属线程，
 * 每 10ms 一次 memcpy + write 的开销可以忽略。
 */
class AudioPlayer : public QObject
{
    Q_OBJECT
public:
    int sampleRate = 48000;
    int sampleSize = 16;
    int channels   = 2;

    AudioPlayer();
    ~AudioPlayer();

    bool open(int sampleRate, int channels, op_player* player);  // 第一次 playFile 时调
    void close();

    bool isOpen() const { return m_sink != nullptr; }
    void clear();                 // 丢弃设备里还没播的数据（seek / 停止）
    void setPause(bool isPause);  // suspend / resume
    long long getNoPlayMs();      // 设备里已写未播的毫秒数

    void setPlayer(op_player* p);

    void   setVolume(qreal vol);
    double getVolume();

private slots:
    void pump();

private:
    QAudioSink* m_sink  = nullptr;
    QIODevice*  m_io    = nullptr;
    QTimer*     m_timer = nullptr;

    std::mutex        m_mutex;       // 保护 m_sink/m_io/clear
    std::atomic<bool> m_isPaused{false};
    op_player*        m_player = nullptr;
    qreal             m_savedVolume = 0.5;

    std::vector<unsigned char> m_buf;   // pump 用的固定缓冲（32KB）
};

#endif // AUDIOPLAYER_H
