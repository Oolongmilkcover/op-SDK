# Android arm64 (arm64-v8a) 交叉编译 toolchain。
#
# 需要 ANDROID_NDK 指向 NDK 根目录（含 toolchains/llvm/prebuilt/*），例如：
#   set ANDROID_NDK=D:/QtProject/op-sdk-deps/android-ndk-r27c
# 用法：
#   cmake -S . -B build-android -G Ninja ^
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android-arm64.cmake ^
#         -DFFMPEG_ROOT=<android arm64 版 FFmpeg 前缀> ^
#         -DOP_BUILD_SHARED=ON -DOP_PLAIN_SO=ON
#
# 注意：CMake 自带的 Android 工具链会去 NDK 里找 clang，不需要手写编译器路径；
# FFmpeg 必须是同一个 NDK / 同一个 API level 编出来的静态库，否则链接会打架。
set(CMAKE_SYSTEM_NAME      Android)
set(CMAKE_SYSTEM_VERSION   21)          # 最低 API 21
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)

if(NOT ANDROID_NDK)
    if(DEFINED ENV{ANDROID_NDK})
        set(ANDROID_NDK "$ENV{ANDROID_NDK}")
    else()
        message(FATAL_ERROR "请设置 ANDROID_NDK（NDK 根目录）")
    endif()
endif()

set(CMAKE_ANDROID_NDK "${ANDROID_NDK}")
set(CMAKE_ANDROID_STL_TYPE c++_shared)

# 交叉编译时不去主机上找库/头文件
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
