#include "platform/audioplayer.h"

#include <QAudioSink>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QIODevice>
#include <QDebug>

#include <algorithm>
#include <climits>

/*
 * 把 op_audio_read 包成一个只读 QIODevice。
 *
 * QAudioSink::start(device) 会按声卡节奏在这个设备上调 readData()：
 * 要多少给多少，没数据就返回 0（Qt 会等，不会忙转）。
 */
class AudioIoDevice : public QIODevice
{
public:
    explicit AudioIoDevice(op_player* player, QObject* parent = nullptr)
        : QIODevice(parent), m_player(player) {}

    bool isSequential() const override { return true; }

    // 告诉 Qt "可能还有数据"：返回一个正数，避免它在 bytesAvailable()==0 时
    // 提前判定 EOF。真正的量由 readData 返回 0 来表达。
    qint64 bytesAvailable() const override { return 16 * 1024 + QIODevice::bytesAvailable(); }

protected:
    qint64 readData(char* data, qint64 maxlen) override
    {
        if (!m_player || maxlen <= 0) return 0;
        const int cap = static_cast<int>(std::min<qint64>(maxlen, INT_MAX));
        const int n = op_audio_read(m_player, data, cap);
        // 返回 0 = 暂时没数据（不是 EOF），Qt 会稍后再来要
        return n > 0 ? n : 0;
    }

    qint64 writeData(const char*, qint64) override { return -1; }

private:
    op_player* m_player = nullptr;
};

AudioPlayer::AudioPlayer() = default;

AudioPlayer::~AudioPlayer()
{
    close();
}

bool AudioPlayer::open(int sampleRate, int channels, op_player* player)
{
    if (!player || sampleRate <= 0 || channels <= 0) return false;

    close();

    this->sampleRate = sampleRate;
    this->channels   = channels;
    m_player         = player;

    QAudioFormat fmt;
    fmt.setSampleRate(sampleRate);
    fmt.setChannelCount(channels);
    fmt.setSampleFormat(QAudioFormat::Int16);
    if (!fmt.isValid()) {
        qDebug() << "无效音频格式";
        return false;
    }

    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (!dev.isFormatSupported(fmt)) {
        qDebug() << "音频格式不支持";
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_io = new AudioIoDevice(player);
    if (!m_io->open(QIODevice::ReadOnly)) {
        qDebug() << "AudioIoDevice open 失败";
        delete m_io;
        m_io = nullptr;
        return false;
    }
    m_sink = new QAudioSink(dev, fmt);
    m_sink->setVolume(m_savedVolume);
    m_sink->start(m_io);          // 之后 Qt 自己按声卡节奏来拉数据

    m_isPaused.store(false);
    return true;
}

void AudioPlayer::close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    if (m_io) {
        m_io->close();
        delete m_io;
        m_io = nullptr;
    }
    m_player = nullptr;
}

void AudioPlayer::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sink) m_sink->reset();
}

void AudioPlayer::setPause(bool isPause)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_sink || m_isPaused.load() == isPause) return;
    if (isPause) m_sink->suspend();
    else         m_sink->resume();
    m_isPaused.store(isPause);
}

long long AudioPlayer::getNoPlayMs()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_sink) return 0;

    const qint64 bytes = m_sink->bufferSize() - m_sink->bytesFree();
    const qint64 bytesPerSecond =
        static_cast<qint64>(sampleRate) * (sampleSize / 8) * channels;
    if (bytesPerSecond <= 0) return 0;
    return bytes * 1000 / bytesPerSecond;
}

void AudioPlayer::setPlayer(op_player* p)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_player = p;
}

void AudioPlayer::setVolume(qreal vol)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_savedVolume = vol;
    if (m_sink && !m_isPaused.load()) m_sink->setVolume(vol);
}

double AudioPlayer::getVolume()
{
    return double(m_savedVolume);
}
