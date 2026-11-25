#include "app/player.h"

#include <QApplication>
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QMetaObject>
#include <QTimer>
#include <QString>
#include <QDebug>
#include <QByteArray>

#ifdef _WIN32
#  include <windows.h>
#  include <timeapi.h>     // timeBeginPeriod / timeEndPeriod（timerapi.h 由 windows.h 之后包含）
#  include <cstdio>
#endif

/*
 * 目的：让 qDebug 在"直接运行"（Ctrl+R，不带调试器）时也能看到。
 *
 * 背景：Windows 上 GUI 子系统程序没有控制台，Qt 默认把 qDebug 写成 OutputDebugString，
 * 只有调试器或 DebugView 能接到 —— 所以 Ctrl+R 运行时 Qt Creator 会提示"无法获取调试输出"，
 * 而 F5 调试时（调试器接住了 OutputDebugString）日志又正常。
 *
 * 两步处理：
 *   1) 把 stdout/stderr 接回**父进程的控制台**（从 cmd 启动时有效，日志直接打在 cmd 里）；
 *   2) 设 QT_FORCE_STDERR_LOGGING=1：没有控制台时也强制写 stderr。
 *      Qt Creator 是用管道抓 stderr 的，所以 Application Output 里就能看到日志。
 *
 * 注意：启动前设这个环境变量比在 Qt Creator 的 Run Environment 里配更省事，
 *       而且对 cmd、Qt Creator、将来 CI 都生效。
 */
int main(int argc, char *argv[])
{
#ifdef _WIN32
    /*
     * 只有"stderr 还是控制台"时才把它接到父进程的控制台。
     * 如果 stderr 已经被重定向到文件/管道（cmd 的 >log 2>&1、Qt Creator、
     * CI 抓日志），再 AttachConsole + freopen("CONOUT$") 会把重定向抢掉，
     * 日志跑到控制台里、文件里一片空白 —— 排查时被这个坑过。
     */
    const HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    const DWORD  hType = (hErr == INVALID_HANDLE_VALUE || hErr == nullptr)
                             ? FILE_TYPE_UNKNOWN : GetFileType(hErr);
    if (hType == FILE_TYPE_CHAR && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
    }
    /*
     * 把本进程的 Windows 时钟分辨率抬到 1ms（退出时还原）。
     *
     * 为什么需要：播放链路里有大量 sleep_for(1ms) 做节流和等待 ——
     *   - DemuxThread::run()     每个包末尾 sleepMs(1)   → 吞吐上限 = 1000 / 分辨率
     *   - AudioThread::decodeRun / playRun 等设备空间时的 sleep_for(1ms)
     *   - VideoRenderThread::sleepUntil() 的 sleep_for(remain - 2ms)
     * Windows 默认分辨率是 15.625ms，此时 sleep_for(1ms) 实际会睡 ~15.6ms，
     * demux 吞吐被压到约 64 包/秒；而 1080p30 + AAC48k 大约需要 77 包/秒
     * （30 视频 + 约 47 音频），供不上就表现为"周期性卡顿 + 声音断续"。
     *
     * 以前这个问题被一个 16ms 的 QTimer 无意中掩盖了：Qt 对 interval <= 20ms 的定时器
     * 会改用多媒体定时器 timeSetEvent()（见 qeventdispatcher_win.cpp），
     * 它的副作用就是把进程时钟分辨率抬到 1ms。那个定时器后来被当成"空转死代码"删掉，
     * 副作用随之消失，卡顿才暴露出来。这里改成显式声明依赖，别再依赖某个定时器碰巧存在。
     */
    timeBeginPeriod(1);
#endif
    qputenv("QT_FORCE_STDERR_LOGGING", "1");

    QApplication a(argc, argv);

    /*
     * --audiotest：最小声卡自检（不经过 SDK / Player）。
     * 用来区分"我们的封装有问题"还是"Qt 后端在这台机器上起不来"。
     */
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "--audiotest") {
        QAudioFormat fmt;
        fmt.setSampleRate(48000);
        fmt.setChannelCount(2);
        fmt.setSampleFormat(QAudioFormat::Int16);
        const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        qDebug() << "[T] dev:" << dev.description()
                 << "formatOk:" << dev.isFormatSupported(fmt);

        auto *sink = new QAudioSink(dev, fmt);
        QIODevice *io = sink->start();
        qDebug() << "[T] after start: io=" << (void *)io
                 << "state=" << sink->state()
                 << "buf=" << sink->bufferSize()
                 << "err=" << sink->error();
        qDebug() << "[T] free =" << sink->bytesFree();

        auto *t = new QTimer(&a);
        QObject::connect(t, &QTimer::timeout, [&a, sink, io, t]() {
            static int n = 0;
            ++n;
            QByteArray silence(8192, 0);
            const qint64 w = io ? io->write(silence) : -1;
            qDebug() << "[T] tick" << n << "state=" << sink->state()
                     << "free=" << sink->bytesFree() << "wrote=" << w;
            if (n >= 5) { t->stop(); a.quit(); }
        });
        t->start(200);
        return a.exec();
    }

    Player w;
    w.show();

    /*
     * 命令行直开：QtPlayer.exe <media>
     * 调试 / 冒烟用（不用手点文件对话框）。playFile 是 private slot，
     * 通过元对象按名字调用即可（Qt 的 slots 天生可反射）。
     */
    if (argc > 1) {
        const QString path = QString::fromLocal8Bit(argv[1]);
        QTimer::singleShot(0, &w, [&w, path]() {
            QMetaObject::invokeMethod(&w, "playFile", Qt::DirectConnection,
                                      Q_ARG(QString, path));
        });
    }
#ifdef _WIN32
    const int rc = a.exec();
    timeEndPeriod(1);
    return rc;
#else
    return a.exec();
#endif
}
