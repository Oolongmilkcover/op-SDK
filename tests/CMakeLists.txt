# FrameQueue 单元测试：直接链 op_core —— 测的就是产品在用的同一份代码，
# 不再单独编译一份 framequeue.cpp（那样测的和产品会慢慢漂开）。
#
# op_core 零 Qt，所以这里也不再链 Qt6::Core；头文件路径和 FFmpeg 依赖都由
# op_core 的 PUBLIC 依赖自动传过来，不用重复写。
# 不创建窗口、不建 QApplication，因此 headless（CI / 无桌面虚拟机）也能直接跑。

add_executable(test_framequeue
    test_framequeue.cpp
)

target_link_libraries(test_framequeue PRIVATE op_core)

add_test(NAME framequeue COMMAND test_framequeue)

# 防御性：万一以后有人往测试里加了 QtGui 相关的东西，无桌面环境也不会因为
# "找不到平台插件"直接失败
set_tests_properties(framequeue PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# C 接口冒烟测试（T1 验收）：create → 查版本 → destroy，且未实现的接口必须
# 明确返回 OP_ERR_UNSUPPORTED。链 op_sdk（而不是 op_core）—— 测的就是调用方的用法。
add_executable(test_api
    test_api.cpp
)

target_link_libraries(test_api PRIVATE op_sdk)

add_test(NAME api COMMAND test_api)

# destroy 要 join 所有线程。万一哪天死锁了，别让 ctest 永远挂着。
set_tests_properties(api PROPERTIES TIMEOUT 30)

# 环形缓冲 / 环形 sink 单测（T4 Step 1+2）：纯 C++，不碰 FFmpeg、不出声、没有媒体文件。
add_executable(test_audioring
    test_audioring.cpp
)

target_link_libraries(test_audioring PRIVATE op_sdk)

# 它测的是 src/ 里的内部类，不是公开接口，所以单独把 src/ 加进 include 路径
target_include_directories(test_audioring PRIVATE ${PROJECT_SOURCE_DIR}/src)

add_test(NAME audioring COMMAND test_audioring)

# 里面有一个 1MB 的 SPSC 并发用例，Debug 下也别给太紧的时间
set_tests_properties(audioring PROPERTIES TIMEOUT 60)

# fd 自定义 AVIO 单测（T5）：造一个临时文件，只用 avio_read/avio_seek/avio_size
# 验证"虚拟文件"的 offset / size / EOF / 越界夹紧。不需要媒体文件，headless 可跑。
add_executable(test_fdavio
    test_fdavio.cpp
)

target_link_libraries(test_fdavio PRIVATE op_core)

add_test(NAME fdavio COMMAND test_fdavio)
