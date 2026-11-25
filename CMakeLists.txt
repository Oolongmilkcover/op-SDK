cmake_minimum_required(VERSION 3.21)

project(op-sdk
    VERSION 1.0.0
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

# 打包模式：默认仍是静态库（和现有单测/上游用法一致）。
# -DOP_BUILD_SHARED=ON 时构建动态库，对外**只**导出 include/op/op_player.h 里的 op_*，
# 调用方不再需要 FFmpeg 头文件/导入库，也不再看到 core/ 与 src/。
option(OP_BUILD_SHARED "把 op_sdk 构建成动态库（对外只暴露 C 接口）" OFF)
# 鸿蒙 har / 安卓 jniLibs 里一般要求库名就是 libop_sdk.so，
# 带 SOVERSION 会变成 libop_sdk.so.1 → 加载器按原名找不到。
option(OP_PLAIN_SO "不生成带版本号的 .so（鸿蒙/安卓打包用）" OFF)

include(GNUInstallDirs)

# FFmpeg：封装成 ffmpeg_deps INTERFACE 目标（Windows 走 FFMPEG_PATH，Linux 走 pkg-config）
include(cmake/FFmpeg.cmake)

# ============================ 内核静态库 ============================
# 这里**不允许**出现任何 Qt 依赖。当前版本仍是 C++ 接口（PlayerCore），
# 对外 C 接口在 include/op/op_player.h（设计稿，见 docs/SDK-PLAN.md 阶段 1）。
set(OP_CORE_SOURCES
    core/perfclock.h
    core/log.h
    core/platform.h            # 编译期平台档位（队列深度 / 历史 / 逐帧）
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
    # 动态库模式要把 op_core 的静态对象并进 .so/.dll，Linux 上必须是 -fPIC
    POSITION_INDEPENDENT_CODE ${OP_BUILD_SHARED}
)

# ============================ SDK 门面 ============================
# 门面单独成一个 target，不塞进 op_core，理由：
#   1. op_core 是"内核"—— 只想要内核的人（比如上游 Qt 播放器）不该被迫连带拿到 C 接口；
#   2. 门面是唯一对 CMD_*/EVT_* 做映射的地方，T4 还要往里塞拉模式的音频缓冲，
#      它会比内核长得快，混在一个库里以后不好拆；
#   3. 静态/动态两种形态都由 OP_BUILD_SHARED 控制（见文件顶部）：
#      动态库模式对外只导出 OP_API（= op_* 函数），core/src 的 C++ 符号一律隐藏。
set(OP_SDK_SOURCES
    src/op_player.cpp
    # 公开头文件挂在这里，IDE（Qt Creator / VS）的工程树才列得出来 ——
    # 只写在 install(FILES ...) 里的话，IDE 是看不见的。
    include/op/op_player.h
    src/audioring.h src/audioring.cpp
)

if(OP_BUILD_SHARED)
    add_library(op_sdk SHARED ${OP_SDK_SOURCES})

    # 编译 DLL 时导出 OP_API；用 DLL 的人由 CMake 包带 OP_USE_SHARED → dllimport
    target_compile_definitions(op_sdk PRIVATE OP_BUILD_SHARED)
    target_compile_definitions(op_sdk INTERFACE OP_USE_SHARED)

    # 动态库模式下 op_core / FFmpeg 都是**实现细节**：PRIVATE 链接，
    # 不出现在导出的 target 接口里，调用方也不需要 FFmpeg 头文件和 .lib。
    target_link_libraries(op_sdk PRIVATE op_core ffmpeg_deps)
    # 编译门面源码时要能找到 core/xxx.h（不进 INTERFACE）
    target_include_directories(op_sdk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    set_target_properties(op_sdk PROPERTIES
        # Linux：只导出标了 visibility("default") 的 OP_API，其余符号隐藏
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    if(WIN32)
        # Windows 上不要给 DLL 加 "lib" 前缀，调用方链接 op_sdk.lib；
        # Linux/鸿蒙/安卓要的是 libop_sdk.so，别把 lib 前缀去掉。
        set_target_properties(op_sdk PROPERTIES PREFIX "")
    endif()
    if(NOT OP_PLAIN_SO)
        # 文件名带版本：libop_sdk.so.1.0.0 / libop_sdk.so.1（Windows 只有 op_sdk.dll）
        set_target_properties(op_sdk PROPERTIES
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR})
    endif()

    # 发行包里 FFmpeg 的 .so 和 libop_sdk.so 放在同一目录：
    # rpath 写 $ORIGIN，调用方不用设 LD_LIBRARY_PATH。
    if(UNIX)
        set_target_properties(op_sdk PROPERTIES
            BUILD_RPATH "$ORIGIN"
            INSTALL_RPATH "$ORIGIN")
    endif()
else()
    add_library(op_sdk STATIC ${OP_SDK_SOURCES})
    # 静态库必须把 op_core 的依赖传给调用方：它要自己链 FFmpeg
    target_link_libraries(op_sdk PUBLIC op_core)
endif()

# 头文件路径：构建树用源码里的 include/，安装后只用 include/。
# 注意这里是 PUBLIC + INSTALL_INTERFACE —— 安装出去的 target 不能带源码绝对路径，
# 否则换台机器 find_package 直接报 "directory does not exist"。
target_include_directories(op_sdk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# ============================ 消费者示例 ============================
# examples/qtplayer 是用这个 SDK 做出来的 Qt 播放器壳，**故意不 add_subdirectory**：
#   1. 它自己的 CMakeLists 里也定义了 op_core 这个 target，并进来会直接 target 重名；
#   2. 它依赖 Qt6，而本仓库的硬约束是"SDK 零 Qt"—— 不能让它污染默认构建
#      （Linux CI、鸿蒙交叉编译都不该被 Qt 拖住）。
# 它是独立子项目：用 Qt Creator 单独打开 examples/qtplayer/CMakeLists.txt，
# 原因和适配步骤见 examples/README.md。

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

# ============================ 安装（发行包） ============================
# 目标目录（install 之后）：
#   include/op/op_player.h           ← 调用方唯一需要的头文件
#   bin/op_sdk.dll (+ av*.dll)       ← Windows 运行时（FFmpeg 是共享链接，必须一起带）
#   lib/op_sdk.lib (或 libop_sdk.so) ← 导入库
#   lib/cmake/op_sdk/*               ← find_package(op_sdk CONFIG) 用的配置
# 调用方 CMake 只需要：
#   find_package(op_sdk CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE op::op_sdk)
include(CMakePackageConfigHelpers)

install(FILES include/op/op_player.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/op)

if(OP_BUILD_SHARED)
    install(TARGETS op_sdk EXPORT op_sdkTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT op_sdkTargets
        NAMESPACE op::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/op_sdk)

    configure_package_config_file(
        cmake/op_sdkConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/op_sdkConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/op_sdk)

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/op_sdkConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/op_sdkConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/op_sdkConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/op_sdk)

    if(WIN32)
        # FFmpeg 这里是共享链接（lib/*.lib 都是导入库），运行时 DLL 必须跟 op_sdk.dll 一起发。
        # 想做成"单文件 DLL"就换成 FFmpeg 静态库并给 av_* 加 /MT —— 那是另一个话题。
        file(GLOB _op_ffmpeg_runtime_dlls "${FFMPEG_PATH}/bin/*.dll")
        if(_op_ffmpeg_runtime_dlls)
            install(FILES ${_op_ffmpeg_runtime_dlls} DESTINATION ${CMAKE_INSTALL_BINDIR})
        else()
            message(WARNING "没在 ${FFMPEG_PATH}/bin 下找到 FFmpeg DLL，发行包里只有 op_sdk.dll，运行时需要自己带 av*.dll")
        endif()
    endif()
else()
    # 静态库形态：调用方要自己链 FFmpeg，这里只把 .lib 和头文件装上
    install(TARGETS op_core op_sdk ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
