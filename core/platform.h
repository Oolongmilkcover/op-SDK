#ifndef CORE_PLATFORM_H
#define CORE_PLATFORM_H

/*
 * 平台档位（编译期常量）
 *
 * 为什么是**编译期**而不是运行时探测：
 *   档位决定的是"队列多深、历史留不留、逐帧开不开"，这些参数在交叉编译时就已经
 *   确定（目标平台就是编译目标）。运行时再去 uname / 读 /proc 只会得到
 *   "和编进库里的代码对不上"的第二个答案 —— 那比不检测更糟。
 *
 * 判定顺序**不能随便改**。实测（D:/ohos-sdk 的 clang，--target=aarch64-linux-ohos）：
 *   __OHOS__ 和 __linux__ 同时为真；安卓的 __ANDROID__ 同理也带 __linux__。
 *   所以必须先 __ANDROID__、再 __OHOS__、最后才 __linux__。
 *   顺序写反 = 鸿蒙/安卓被当成桌面 Linux，移动档一个都不生效，而且**不报错**，
 *   是本类改动里最难查的一种 bug。
 *
 * 未知平台按**桌面**档处理：宁可多占点内存，也不要悄悄少功能
 * （"少功能"的 bug 只会表现为"按钮点了没反应"，比 OOM 更难定位）。
 * 以后要支持 iOS/macOS：判定链里加分支，并让 kIsMobile 跟着改。
 */

namespace core
{

/*
 * 数值必须和 include/op/op_player.h 的 op_platform_t 一一对应
 * （门面直接 static_cast 过去，op_player.cpp 里有 static_assert 钉住）。
 */
enum class Platform : int
{
    Unknown   = 0,
    Windows   = 1,
    Linux     = 2,
    Android   = 3,
    HarmonyOS = 4
};

#if defined(__ANDROID__)
constexpr Platform kPlatform = Platform::Android;
#elif defined(__OHOS__)
constexpr Platform kPlatform = Platform::HarmonyOS;
#elif defined(_WIN32)
constexpr Platform kPlatform = Platform::Windows;
#elif defined(__linux__)
constexpr Platform kPlatform = Platform::Linux;
#else
constexpr Platform kPlatform = Platform::Unknown;
#endif

/*
 * 只有安卓 / 鸿蒙算移动。**桌面 Linux 不算**：
 *   1. 桌面 Linux 没有 per-app 内存上限，150MB 历史帧不致命；
 *   2. tests/test_framequeue 就是按 16 帧 + 48 帧历史写的，桌面 Linux 是功能
 *      回归基线，把它切到移动档等于把回归测试一起改掉；
 *   3. 嵌入式 ARM Linux 以后真要做，再加显式开关（现在没有这个目标，
 *      加了就是没人跑的死分支）。
 */
constexpr bool kIsMobile = (kPlatform == Platform::Android)
                        || (kPlatform == Platform::HarmonyOS);

constexpr const char *platformName(Platform p)
{
    switch (p)
    {
    case Platform::Windows:   return "Windows";
    case Platform::Linux:     return "Linux";
    case Platform::Android:   return "Android";
    case Platform::HarmonyOS: return "HarmonyOS";
    default:                  return "Unknown";
    }
}

/* ============================ 队列档位 ============================ */

/*
 * 视频帧队列（未来队列）。8 帧 @24fps ≈ 333ms 的预解码余量，只用来吸收
 * "解码抖动 + 关键帧突发"；渲染线程按墙钟节奏消费，队列深度不是播放延迟，
 * 所以 8 帧不会让画面变卡。
 */
constexpr int kVideoFrameQueueSize = kIsMobile ? 8 : 16;

/*
 * 音频帧队列保持 16：一帧 1024 采样 × 2ch × 2B ≈ 4KB，整队才 ≈ 64KB，
 * 砍它省不出东西，反而少一段抗抖动的余量。
 */
constexpr int kAudioFrameQueueSize = 16;

/*
 * 历史队列才是内存大头：entries 里每帧都 clone 住一整帧转换后的 buffer，
 * 1080p 一帧 ≈ 3.1MB，48 帧 ≈ 150MB（720p 也有 66MB），而且正常播放约 2 秒后
 * 就常驻满 —— 这是手机上被系统 kill 的直接原因。历史只服务于"逐帧回退"，
 * 移动端不需要它，所以整条关掉。
 *
 * 注意：这里关的是**运行时**（FrameQueue 的 keep_last=false），历史/逐帧的代码
 * 仍然参与编译 —— 用 #ifdef 把它们挖掉会让桌面和移动变成两份逻辑，得不偿失。
 */
constexpr bool kKeepFrameHistory   = !kIsMobile;
constexpr bool kFrameStepSupported = !kIsMobile;

/*
 * 包队列只砍**视频**：
 *   - 1080p 一个包约 20KB，100 包 ≈ 2MB；50 包仍有 ≈2s（25fps）的抗抖动余量，
 *     本地文件（手机目前主要走 picker/fd）完全够；
 *   - 音频包一个 1~2KB，100 包才 200KB —— 砍它等于拿音频抗抖动余量换 100KB，不划算。
 * 以后真要上 HLS/RTSP，建议把上限改成按**字节**算（比如 8MB），而不是按包数。
 */
constexpr int kVideoPacketQueueSize = kIsMobile ? 50 : 100;
constexpr int kAudioPacketQueueSize = 100;

} // namespace core

#endif // CORE_PLATFORM_H
