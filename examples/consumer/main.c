/*
 * ============================================================================
 *  op_consumer —— "只拿到头文件 + 库"的调用方最小示例（C 语言）
 * ============================================================================
 *
 *  这个示例**故意不引用仓库里的任何源码**：它只 include <op/op_player.h>，
 *  然后链接安装出来的 op_sdk（CMake 包 op::op_sdk）。用来证明三件事：
 *
 *    1. 公开面真的只有头文件：core/、src/、FFmpeg 都不需要；
 *    2. 头文件是 C 兼容的（这里就用 C 编译，不是 C++）；
 *    3. 动态库模式的导出/导入宏是对的（Windows 下链接 op_sdk.lib、运行要 op_sdk.dll）。
 *
 *  构建（先安装 SDK）：
 *      cmake -S examples/consumer -B build-consumer -G Ninja ^
 *            -DCMAKE_PREFIX_PATH=<SDK 安装目录>
 *      cmake --build build-consumer
 *
 *  不带参数：只验证 create/version/destroy 和错误码。
 *  带参数：打开一个媒体文件，等 OPENED，打印属性，播 0.5 秒再报位置。
 */

#include <op/op_player.h>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#  include <unistd.h>
static void sleep_ms(int ms) { usleep((unsigned)(ms * 1000)); }
#endif

static int g_opened = 0;
static int g_error  = 0;

static void on_event(void *user, const op_event *ev)
{
    (void)user;
    if (!ev) return;

    if (ev->type == OP_EVENT_OPENED) g_opened = 1;
    if (ev->type == OP_EVENT_ERROR)  g_error  = 1;

    printf("[event] type=%d state=%d code=%d pos=%lld\n",
           (int)ev->type, (int)ev->state, (int)ev->code,
           (long long)ev->position_ms);
}

int main(int argc, char **argv)
{
    int32_t major = 0, minor = 0;
    (void)op_version(&major, &minor);
    printf("op_api %d.%d\n", (int)major, (int)minor);

    /* 空指针必须是"返回错误码"而不是崩：这也是导出边界的基本要求 */
    if (op_has_video(NULL) != 0 || op_duration_ms(NULL) != 0) {
        printf("FAIL: NULL 入参没有安全返回\n");
        return 1;
    }

    op_player *p = op_create();
    if (!p) {
        printf("FAIL: op_create 返回空\n");
        return 1;
    }

    if (argc > 1) {
        op_open_options opt;
        memset(&opt, 0, sizeof(opt));
        opt.path            = argv[1];
        opt.fd              = -1;
        opt.open_timeout_ms = 15000;

        op_set_event_cb(p, on_event, NULL);
        op_set_audio_format(p, 48000, 2);   /* 声明壳能播的 PCM 格式 */

        op_result r = op_open(p, &opt);
        printf("op_open = %d\n", (int)r);
        if (r == OP_OK) {
            /* 事件在 SDK 线程上回调，这里只是简单轮询等它 */
            for (int i = 0; i < 100 && !g_opened && !g_error; ++i)
                sleep_ms(50);

            if (g_opened) {
                printf("duration=%lld ms  video=%d audio=%d music=%d audio_media=%d\n",
                       (long long)op_duration_ms(p),
                       (int)op_has_video(p), (int)op_has_audio(p),
                       (int)op_is_music(p),  (int)op_is_audio_media(p));

                op_play(p);
                sleep_ms(500);
                printf("position=%lld ms state=%d\n",
                       (long long)op_position_ms(p), (int)op_state_of(p));
                if (op_has_audio(p) && op_position_ms(p) == 0) {
                    printf("提示：本示例没有循环 op_audio_read 拉 PCM，音频时钟不会前进；\n"
                           "      真实播放器要按 docs/API.md 的 pull 模式把 PCM 送进声卡。\n");
                }
                op_stop(p);
            } else {
                printf("FAIL: 没等到 OPENED（error=%d）\n", g_error);
            }
        }
    }

    op_destroy(p);
    printf("OK\n");
    return 0;
}
