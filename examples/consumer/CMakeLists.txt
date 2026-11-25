# 消费者示例：**只依赖安装出来的 SDK**，不 add_subdirectory 主仓库。
#
# 用法（先安装 SDK）：
#   cmake --install build-shared --prefix dist
#   cmake -S examples/consumer -B build-consumer -G Ninja -DCMAKE_PREFIX_PATH=<repo>/dist
#   cmake --build build-consumer
#
# 它验证的就是"别人拿到 include/op/op_player.h + lib + dll 之后能不能直接调用"。
cmake_minimum_required(VERSION 3.21)

project(op-consumer LANGUAGES C)

find_package(op_sdk CONFIG REQUIRED)

add_executable(op_consumer main.c)

# op::op_sdk 自带头文件路径、库、以及动态库模式下的 OP_USE_SHARED（Windows dllimport）
target_link_libraries(op_consumer PRIVATE op::op_sdk)

# 头文件是 UTF-8 with BOM、注释全中文：MSVC 默认按 GBK 读会刷 C4819。
# 调用方要么加 /utf-8（推荐），要么指望 BOM —— 这里显式加上，示范正确做法。
if(MSVC)
    target_compile_options(op_consumer PRIVATE /utf-8)
endif()
