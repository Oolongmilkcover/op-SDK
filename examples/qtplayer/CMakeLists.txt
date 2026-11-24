cmake_minimum_required(VERSION 3.21)

project(QtPlayer
    VERSION 0.1
    DESCRIPTION "op-SDK 的消费者示例：Qt6 播放器壳（内核与门面都来自主仓库，本目录只放 UI/平台层）"
    LANGUAGES CXX
)

# 源码是 UTF-8（含中文注释），而 Windows 默认代码页是 936(GBK)。
# 链 Qt 的目标能顺带拿到 Qt INTERFACE 选项里的 /utf-8；**不链 Qt 的目标拿不到**（比如 op_pusher），
# 于是 MSVC 按 GBK 去解 UTF-8 源码：先报 C4819，多字节序列里凑出引号/花括号后解析器跑偏，
# 最终在系统头文件（ucrt/time.h、ctime）里报语法错误 —— 看起来毫不相关。
if(MSVC)
    add_compile_options(/utf-8)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 只查找 Qt6 组件。这里刻意不写最低版本（发行版 Qt 可能是 6.4）。
find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Multimedia
    OpenGL
    OpenGLWidgets
)

# ============================ 内核与门面：用主仓库的，不在这里复制 ============================
#
# 本目录**只有** UI（app/）和平台层（platform/），加上一个 console 推流端（pusher/）。
# core/、include/op/、src/ 全部来自主仓库 —— 复制一份出来的代价是两份会慢慢漂开，
# 而主仓库的 AGENTS.md 明确规定"本仓库是内核的唯一开发源"。
#
# 路径：本文件在 examples/qtplayer/，所以 ../.. 就是 op-SDK 仓库根。
# 构建目录塞进示例自己的 build 里（${CMAKE_BINARY_DIR}/op-sdk），不污染源码树。
#
# OP_BUILD_TESTS 必须先关掉：主仓库的单测目标（test_framequeue 等）和示例工程
# 没有任何关系，而且 ctest 归主仓库管，不该在示例里再挂一遍。
set(OP_BUILD_TESTS OFF CACHE BOOL "示例工程里不编主仓库的单测" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. ${CMAKE_BINARY_DIR}/op-sdk)

# **顺序很重要**：qt_standard_project_setup() 会把 CMAKE_AUTOMOC/AUTOUIC 设成 ON，
# 而这个变量会沿目录继承到子目录。必须等主仓库的 target 都定义完之后再打开，
# 否则 op_core/op_sdk 也会挂上 moc 步骤 —— 那两个 target 的硬约束是"零 Qt"。
qt_standard_project_setup()

# qt_standard_project_setup() 只负责 MOC / UIC，**不开 AUTORCC**
# （见 Qt6CoreMacros.cmake 里的 `foreach(auto_set IN ITEMS MOC UIC)`）。
# 少了这行 resources/res.qrc 不会编译进二进制：图标和 Basic.shader 全丢，
# 运行时不报错但界面一片空白，所以必须显式打开。
set(CMAKE_AUTORCC ON)

# 保险：主仓库的两个 target 明确不带 Qt 代码生成
set_target_properties(op_core op_sdk PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

# ============================ 源码清单（按层） ============================
# app/       Qt 壳：窗口、控制栏、标题栏、播放列表、自定义滑条
set(OP_APP_SOURCES
    app/main.cpp
    app/player.h            app/player.cpp            app/player.ui
    app/ctrlbar.h           app/ctrlbar.cpp           app/ctrlbar.ui
    app/topmenu.h           app/topmenu.cpp           app/topmenu.ui
    app/playlist.h          app/playlist.cpp          app/playlist.ui
    app/myslider.h          app/myslider.cpp
)

# platform/  平台适配：Qt Multimedia 音频输出、QOpenGLWidget 视频渲染目标
set(OP_PLATFORM_SOURCES
    platform/audioplayer.h  platform/audioplayer.cpp
    platform/videowidget.h  platform/videowidget.cpp
)

# ============================ 播放器可执行文件 ============================
qt_add_executable(QtPlayer
    MANUAL_FINALIZATION
    ${OP_APP_SOURCES}
    ${OP_PLATFORM_SOURCES}
    resources/res.qrc
)

# 源码里的 include 一律写 "core/xxx.h" / "platform/xxx.h" / "app/xxx.h"，
# AUTOUIC 生成的 ui_*.h 也会 #include "app/ctrlbar.h"，所以把示例根目录加进来。
# （"core/xxx.h" 由 op_core 的 PUBLIC include 解析到主仓库的 core/，这里不用管。）
target_include_directories(QtPlayer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 链接 op_sdk：它 PUBLIC 链着 op_core（内核）和 ffmpeg_deps（FFmpeg），
# 所以这里不用再单独写 ffmpeg_deps。
#
# 现状说明：app/ 目前还在直接使用 core 的 C++ 类（PlayerCore / DemuxThread / IAudioOutput…），
# 那是"改造成 C 接口"之前的形态。等 app/ 改成只调 op_* 之后，这里链 op_sdk 依然成立，
# 可以顺手把对 op_core 头文件的依赖删干净 —— 见 examples/README.md 的适配步骤。
target_link_libraries(QtPlayer PRIVATE
    op_sdk
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::OpenGL
    Qt6::OpenGLWidgets
)

# Windows：main.cpp 里用 timeBeginPeriod/timeEndPeriod 把进程时钟分辨率抬到 1ms
# （demux / 音频线程依赖 1ms 精度的 sleep，默认 15.625ms 会把 demux 吞吐压到 ~64 包/秒）。
if(WIN32)
    target_link_libraries(QtPlayer PRIVATE winmm)
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(QtPlayer PRIVATE Threads::Threads)
endif()

# IDE（Qt Creator / Visual Studio）里按层折叠文件
source_group("app"      FILES ${OP_APP_SOURCES})
source_group("platform" FILES ${OP_PLATFORM_SOURCES})

# ============================ 最终化与安装 ============================
set_target_properties(QtPlayer PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

include(GNUInstallDirs)
install(TARGETS QtPlayer
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

qt_finalize_executable(QtPlayer)

# 推流端（上游的 op_pusher / mini-OBS）**没有搬进来**：
# 它和播放器、和这个 SDK 都没有关系（自带 capturesource/encoder/muxer，用的是自己的
# dropqueue 而不是 core 的队列），放在示例里只会让人以为"SDK 还要管推流"。
# 需要它就去上游仓库 QtFFmpegPlayer 拿。
