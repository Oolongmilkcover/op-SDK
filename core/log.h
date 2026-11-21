#ifndef CORE_LOG_H
#define CORE_LOG_H

/*
 * core 侧的极简日志（纯 C++，不依赖 Qt，也不认识 C 接口）
 *
 * 用途：替换原来散落在 core 里的 qDebug()，让 core 不再需要 <QDebug> / QtGlobal。
 *
 * 用法与 qDebug() 完全一致：
 *     logDebug() << "open" << url << "failed!:" << err_buf;
 *     logError() << "resampleInit failed, ret=" << ret;
 *
 * 两个出口（2026-11-21 加的可替换 sink）：
 *   1. 没注册 sink（默认）：写到 stderr，和 Qt 的 qDebug 一样不缓冲；
 *   2. 注册了 sink：拼好的整行交给 sink，**不再写 stderr**。
 *      SDK 门面（src/op_player.cpp）就是在这里把日志接到 op_log_cb 上的。
 *
 * 实现要点：
 *   - 一行日志先在 std::ostringstream 里拼好，析构时整体输出，
 *     避免多个线程（demux / 解码 / 渲染）的 << 互相插进对方半行里；
 *   - **sink 在锁外调用**：用户回调里可能再调 SDK，而那些路径还会打日志 ——
 *     拿着 logMutex 去回调就是自己等自己；
 *   - sink 里再打日志会无限递归，用 thread_local 标志直接丢弃（见 t_insideLogSink）；
 *   - LogLine 不可拷贝，C++17 的强制复制消除保证 logDebug() 返回值直接构造调用点对象。
 *
 * 等级：Debug / Info / Warn / Error，数值 0~3，和 C 接口 op_log_cb 的 level 一一对应。
 * core 里暂时只有 Debug/Warn/Error 三个工厂函数，Info 留给调用方以后用。
 */

#include <cstdio>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>

namespace core
{

enum class Level : int
{
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3
};

// 一行完整日志（不含换行）。只在回调期间有效，sink 里要存就自己拷走。
using LogSink = std::function<void(Level, const char *)>;

inline std::mutex &logMutex()
{
    static std::mutex m;
    return m;
}

// 内联函数里的 static 局部量在整个程序里只有一个实例（C++ 保证），
// 所以这里不需要单独的 .cpp，也没有 ODR 问题。
inline LogSink &logSinkRef()
{
    static LogSink s;
    return s;
}

// sink 里再调 logDebug()/logWarn() 时置位，避免"日志记日志"无限递归。
inline thread_local bool t_insideLogSink = false;

struct ScopedLogFlag
{
    bool &flag;
    explicit ScopedLogFlag(bool &f) : flag(f) { flag = true; }
    ~ScopedLogFlag() { flag = false; }
    ScopedLogFlag(const ScopedLogFlag &) = delete;
    ScopedLogFlag &operator=(const ScopedLogFlag &) = delete;
};

/*
 * 安装/替换 sink；传空函数（{} 或 nullptr）恢复成"只写 stderr"。
 * 可以在任意线程调用。已经在飞的日志持有的是 sink 的副本，不受影响。
 */
inline void setLogSink(LogSink sink)
{
    std::lock_guard<std::mutex> lock(logMutex());
    logSinkRef() = std::move(sink);
}

class LogLine
{
public:
    LogLine(Level level, const char *tag) : m_level(level), m_tag(tag) {}

    ~LogLine()
    {
        // 递归保护放在最前面：sink 内部的日志在建行阶段就该被丢掉，
        // 否则下面还会再拼一遍字符串、再抢一次锁。
        if (t_insideLogSink)
            return;

        // 锁内只拷 sink，锁外再调用（见文件头"sink 在锁外调用"）。
        LogSink sink;
        {
            std::lock_guard<std::mutex> lock(logMutex());
            sink = logSinkRef();
        }

        const std::string text = m_buf.str();

        if (sink) {
            ScopedLogFlag guard(t_insideLogSink);
            sink(m_level, text.c_str());   // 行尾不带 '\n'，prefix 也不带（等级单独给）
            return;
        }

        std::lock_guard<std::mutex> lock(logMutex());
        if (m_tag)
            std::fwrite(m_tag, 1, std::char_traits<char>::length(m_tag), stderr);
        std::fwrite(text.data(), 1, text.size(), stderr);
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }

    LogLine(const LogLine &) = delete;
    LogLine &operator=(const LogLine &) = delete;

    template <class T>
    LogLine &operator<<(const T &value)
    {
        m_buf << value;
        return *this;
    }

private:
    Level       m_level;
    const char *m_tag;      // 只给 stderr 出口用（"[warn] " / "[error] "）
    std::ostringstream m_buf;
};

} // namespace core

inline core::LogLine logDebug() { return core::LogLine(core::Level::Debug, nullptr); }
inline core::LogLine logInfo()  { return core::LogLine(core::Level::Info,  nullptr); }
inline core::LogLine logWarn()  { return core::LogLine(core::Level::Warn,  "[warn] "); }
inline core::LogLine logError() { return core::LogLine(core::Level::Error, "[error] "); }

#endif // CORE_LOG_H
