cmake_minimum_required(VERSION 3.21)

project(op-sdk
    VERSION 0.1.0
    DESCRIPTION "Qt-free playback core, packaged as a reusable SDK"
    LANGUAGES CXX
)

# 源码是 UTF-8（含中文注释），Windows 默认代码页 936(GBK)。不显式指定会被按 GBK 解析：
# 先报 C4819，多字节序列里凑出引号/花括号后解析器跑偏，最终在系统头文件里报语法错误。
if(MSVC)
    add_compile_options(/utf-8)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# FFmpeg：封装成 ffmpeg_deps INTERFACE 目标（Windows 走 FFMPEG_PATH，Linux 走 pkg-config）
include(cmake/FFmpeg.cmake)

# ============================ 内核静态库 ============================
# 这里**不允许**出现任何 Qt 依赖。当前版本仍是 C++ 接口（PlayerCore），
# 对外 C 接口在 include/op/op_player.h（设计稿，见 docs/SDK-PLAN.md 阶段 1）。
set(OP_CORE_SOURCES
    core/perfclock.h
    core/log.h
    core/iaudiooutput.h
    core/fdavio.h             core/fdavio.cpp
    core/frameconverter.h     core/frameconverter.cpp
    core/packetqueue.h        core/packetqueue.cpp
    core/framequeue.h         core/framequeue.cpp
    core/decodethread.h       core/decodethread.cpp
    core/demuxthread.h        core/demuxthread.cpp
    core/audiothread.h        core/audiothread.cpp
    core/videodecodethread.h  core/videodecodethread.cpp
    core/videorenderthread.h  core/videorenderthread.cpp
    core/messagequeue.h       core/messagequeue.cpp
    core/messageloopthread.h  core/messageloopthread.cpp
    core/playercore.h         core/playercore.cpp
    core/playercontext.h      core/playercontext.cpp
)

add_library(op_core STATIC ${OP_CORE_SOURCES})

# PUBLIC 的两条：
#   include/  对外头文件（op/op_player.h 是唯一公开面）
#   源码根    内部的 core/xxx.h 用的是 "core/..." 这种写法
target_include_directories(op_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# FFmpeg 是公开依赖：core 的头文件里就有 libav* 类型
target_link_libraries(op_core PUBLIC ffmpeg_deps)

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(op_core PUBLIC Threads::Threads)
endif()

# 关掉代码生成：这个 target 零 Qt，谁往 core/ 里加了 Q_OBJECT 会直接编不过，
# 而不是被 moc 悄悄处理掉、再在 Linux / NDK 上炸。
set_target_properties(op_core PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

# ============================ SDK 门面 ============================
# 门面单独成一个 target，不塞进 op_core，理由：
#   1. op_core 是"内核"—— 只想要内核的人（比如上游 Qt 播放器）不该被迫连带拿到 C 接口；
#   2. 门面是唯一对 CMD_*/EVT_* 做映射的地方，T4 还要往里塞拉模式的音频缓冲，
#      它会比内核长得快，混在一个库里以后不好拆；
#   3. 现在是 STATIC：Windows 上 SHARED 要配 OP_BUILD_SHARED + dllexport，
#      而 MSVC 和 MinGW 两条工具链都得照顾。等阶段 3 要稳定 ABI 时再切。
add_library(op_sdk STATIC
    src/op_player.cpp
    # 公开头文件挂在这里，IDE（Qt Creator / VS）的工程树才列得出来 ——
    # 只写在 install(FILES ...) 里的话，IDE 是看不见的。
    include/op/op_player.h
    src/audioring.h src/audioring.cpp

)

# PUBLIC：调用方要拿到 op_core 的头文件路径和 FFmpeg 依赖
target_link_libraries(op_sdk PUBLIC op_core)

# 公开头文件是 UTF-8 **with BOM**：它的注释全是中文，而 MSVC 默认按系统代码页
# （中文 Windows = 936/GBK）读源文件，不带 BOM 又不加 /utf-8 就会刷 C4819，
# 运气不好还会把注释里的多字节序列解析成引号/花括号。BOM 是唯一"不需要消费方
# 配合开关"的解法 —— 而它极容易在重写文件时被工具丢掉，所以在这里钉一下。
# 只报警不 FATAL：编不编得过是编译器的事，但不该让它静默退化。
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/include/op/op_player.h
     _op_hdr_head OFFSET 0 LIMIT 3 HEX)
if(NOT _op_hdr_head STREQUAL "efbbbf")
    message(WARNING
        "include/op/op_player.h 丢了 UTF-8 BOM（当前开头字节 ${_op_hdr_head}）。"
        "补回来，否则 MSVC 消费者不加 /utf-8 时会刷 C4819。")
endif()

# ============================ 单测（无桌面也能跑） ============================
option(OP_BUILD_TESTS "构建 core 的单测" ON)
if(OP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================ 安装 ============================
# 现在只装 C 接口头 + 静态库；等门面实现完成后再补 shared 库与 version 脚本。
include(GNUInstallDirs)
install(TARGETS op_core ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/op/op_player.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/op)
