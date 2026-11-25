#include "platform/audioplayer.h"

#include <QAudioSink>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QIODevice>
#include <QTimer>
#include <QDebug>

#include <algorithm>
#include <climits>

AudioPlayer::AudioPlayer()
{
    m_buf.resize(32 * 1024);
}

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
    qDebug() << "[Audio] device:" << dev.description()
             << "valid:" << !dev.isNull()
             << "formatOk:" << dev.isFormatSupported(fmt);
    if (!dev.isFormatSupported(fmt)) {
        qDebug() << "音频格式不支持";
        return false;
    }

    m_sink = new QAudioSink(dev, fmt);
    m_sink->setVolume(m_savedVolume);
    m_io = m_sink->start();          // 推模式：返回给我们写的 QIODevice
    if (!m_io) {
        qDebug() << "QAudioSink::start 失败:" << m_sink->error();
        delete m_sink;
        m_sink = nullptr;
        return false;
    }

    // 先写一批静音：让 sink 从 IdleState 进入 ActiveState（真实 PCM 还没解出来）
    {
        std::vector<char> silence(8192, 0);
        m_io->write(silence.data(), (qint64)silence.size());
    }

    m_isPaused.store(false);
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &AudioPlayer::pump);
    m_timer->start(10);

    qDebug() << "[Audio] started, bufferSize:" << m_sink->bufferSize()
             << "state:" << m_sink->state() << "free:" << m_sink->bytesFree();
    return true;
}

void AudioPlayer::pump()
{
    if (!m_sink || !m_io) return;
    if (m_isPaused.load(std::memory_order_relaxed)) return;
    if (!m_player) return;

    const int freeBytes = m_sink->bytesFree();
    if (freeBytes <= 0) return;

    int want = std::min<int>(freeBytes, (int)m_buf.size());
    const int frameBytes = channels * (sampleSize / 8);
    if (frameBytes > 0) want -= want % frameBytes;     // 采样帧对齐
    if (want <= 0) return;

    const int got = op_audio_read(m_player, m_buf.data(), want);
    if (got > 0) {
        m_io->write((const char *)m_buf.data(), got);
        static bool logged = false;
        if (!logged) {
            logged = true;
            qDebug() << "[Audio] first PCM write" << got << "bytes";
        }
    }

    static int dbg = 0;
    if (!qgetenv("OP_QT_SEEK_MS").isEmpty() && (++dbg % 100 == 0))
        qDebug() << "[pump-dbg]" << dbg << "free" << freeBytes
                 << "got" << got << "state" << m_sink->state();

    // 把"设备里还没播"的时长报给 SDK（音频时钟的延迟补偿）
    const qint64 bytes = m_sink->bufferSize() - m_sink->bytesFree();
    const qint64 bps   = (qint64)sampleRate * (sampleSize / 8) * channels;
    if (bps > 0)
        op_audio_set_device_pending_ms(m_player, (int32_t)(bytes * 1000 / bps));
}

void AudioPlayer::close()
{
    if (m_timer) {
        m_timer->stop();
        delete m_timer;
        m_timer = nullptr;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    m_io = nullptr;
    m_player = nullptr;
}

void AudioPlayer::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_sink) return;

    /*
     * 注意：QAudioSink::reset() 的语义是"丢掉缓冲 + **停止设备**"，
     * 不是"清空但继续播"。以前这里只调 reset()，结果 playFile 打开新文件时
     * 顺手 clear 一下就把设备停死了（state 变 StoppedState、bytesFree 永远 0），
     * 音频线程写不进、环形缓冲堵满、音频时钟不走 → 视频播一下就冻住。
     * 所以 reset 之后必须重新 start 拿到新的 QIODevice，并补一段静音让它进入 Active。
     */
    const bool wasPaused = m_isPaused.load();
    m_sink->reset();
    m_io = m_sink->start();
    if (!m_io) {
        qDebug() << "[Audio] clear 后 start 失败:" << m_sink->error();
        return;
    }

    std::vector<char> silence(8192, 0);
    m_io->write(silence.data(), (qint64)silence.size());

    if (wasPaused) m_sink->suspend();
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
    const qint64 bps   = (qint64)sampleRate * (sampleSize / 8) * channels;
    if (bps <= 0) return 0;
    return bytes * 1000 / bps;
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
