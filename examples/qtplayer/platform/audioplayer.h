#ifndef AUDIOPLAYER_H
#define AUDIOPLAYER_H

#include <QObject>
#include "include/op/op_player.h"
#include <mutex>
#include <atomic>

class QAudioSink;
class AudioIoDevice;

/*
 * Qt 侧的音频出口（pull 模式）。
 *
 * 为什么不是 QThread + write：SDK 的音频是"壳来拉"（op_audio_read），
 * Qt 正好提供了同一个模型 —— 给 QAudioSink 一个 QIODevice，它在自己的音频线程里
 * 调 readData()。我们只要在 readData 里调 op_audio_read 就行：
 *
 *   QAudioSink ──readData()──> AudioIoDevice::readData ──op_audio_read──> SDK 环形缓冲
 *
 * 这样：
 *   - 不需要自己开线程，也没有"跨线程调 QAudioSink::write"的问题；
 *   - 设备要多少拉多少，天然按声卡节奏走（就是 SDK 选 pull 的原因）；
 *   - 没数据时返回 0，Qt 会等待（它自己处理欠载）。
 *
 * 时钟：AudioPlayer::getNoPlayMs() 返回"设备里已写未播"的毫秒数，
 * Player 在 POSITION 事件里把它通过 op_audio_set_device_pending_ms 报给 SDK。
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

    // 创建 QAudioSink 并开始拉流。player 必须活着（由 Player 保证先 close 再 destroy）。
    bool open(int sampleRate, int channels, op_player* player);
    void close();

    // 丢掉设备里还没播的数据（seek / 停止时用）
    void clear();

    void setPause(bool isPause);

    // 设备缓冲里还没播的毫秒数（音频时钟的延迟补偿量）
    long long getNoPlayMs();

    void setPlayer(op_player* p);

    void   setVolume(qreal vol);
    double getVolume();

private:
    QAudioSink*       m_sink = nullptr;
    AudioIoDevice*    m_io   = nullptr;
    std::mutex        m_mutex;
    std::atomic<bool> m_isPaused{false};
    op_player*        m_player = nullptr;
    qreal             m_savedVolume = 0.5;
};

#endif // AUDIOPLAYER_H
