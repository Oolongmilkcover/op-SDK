# 把 op-SDK 打成"真正的库"（发行包）

目标：别人只拿到 **一个头文件 + 一个动态库（+ 导入库）**，就能在自己的程序里调用，
看不到 `core/`、`src/`，也不需要装 FFmpeg 开发包。

```
发出去的包
├─ include/op/op_player.h      ← 唯一公开的头文件（C 接口）
├─ bin/op_sdk.dll              ← 动态库（Windows）
│   └─ av*.dll                 ← FFmpeg 运行时（当前是共享链接，必须一起发）
├─ lib/op_sdk.lib              ← 导入库（链接时用）
└─ lib/cmake/op_sdk/*          ← find_package(op_sdk CONFIG) 的配置
```

调用方 CMake 只需要：

```cmake
find_package(op_sdk CONFIG REQUIRED)
target_link_libraries(app PRIVATE op::op_sdk)
```

`op::op_sdk` 已经带上了头文件路径、库、以及 Windows 动态库所需的 `OP_USE_SHARED`（dllimport），
调用方不用再写任何 FFmpeg 相关的东西。

---

## 1. 构建 + 安装

```powershell
# 动态库模式（发行用）
cmake -S . -B build-shared -G Ninja ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DOP_BUILD_SHARED=ON ^
      -DFFMPEG_PATH="C:/Program Files/ffmpeg/ffmpeg8.1"
cmake --build build-shared
ctest --test-dir build-shared --output-on-failure     # 5/5

cmake --install build-shared --prefix dist
```

Linux / 鸿蒙 / 安卓同样是 `-DOP_BUILD_SHARED=ON`，产物是 `libop_sdk.so.1.0.0`（带 SONAME）。

默认（不传 `OP_BUILD_SHARED`）仍然是**静态库**，行为和以前完全一致，
方便单测和上游 Qt 播放器直接 `add_subdirectory`。

---

## 2. 头文件宏是怎么工作的

`op_player.h` 里：

```c
#if defined(_WIN32)
#  if defined(OP_BUILD_SHARED)          /* 编译 DLL 时由 CMake PRIVATE 定义 */
#    define OP_API __declspec(dllexport)
#  elif defined(OP_USE_SHARED)          /* 用 DLL 的人由 CMake INTERFACE 定义 */
#    define OP_API __declspec(dllimport)
#  else
#    define OP_API
#  endif
#else
#  if defined(OP_BUILD_SHARED)
#    define OP_API __attribute__((visibility("default")))
#  else
#    define OP_API
#  endif
#endif
```

- 构建 DLL：`-DOP_BUILD_SHARED` → 每个 `OP_API` 函数被导出；
- 使用 DLL：CMake 包的 `INTERFACE_COMPILE_DEFINITIONS` 带 `OP_USE_SHARED` → `dllimport`；
- Linux 额外开了 `CXX_VISIBILITY_PRESET hidden`，**只有** `OP_API` 的 C 函数可见，
  `core/`、`src/` 的 C++ 符号（`std::`、模板实例、RTTI）都不会出现在 `.so` 的动态符号表里。

验证导出面（Windows）：

```powershell
dumpbin /exports dist\bin\op_sdk.dll
# 33 个导出，全部以 op_ 开头；没有 ??0PlayerCore@@... 之类的 C++ 符号
```

Linux：

```bash
nm -D --defined-only dist/lib/libop_sdk.so | grep ' T '
```

---

## 3. 调用方怎么用（不装 FFmpeg）

```c
#include <op/op_player.h>

op_player *p = op_create();
op_open_options opt = {0};
opt.path = "D:/media/test.mp4";
opt.fd   = -1;
op_open(p, &opt);
/* 事件回调里处理 OPENED / POSITION / ERROR ... */
op_destroy(p);
```

运行时要保证 **`op_sdk.dll` 和 `av*.dll` 能被加载器找到**（同一目录最简单）。
例子见 `examples/consumer/`：它只 `find_package(op_sdk)`，不引用仓库任何源码，
并且用 **C** 编译（顺带验证头文件是 C 兼容的）。

```powershell
cmake -S examples/consumer -B build-consumer -G Ninja ^
      -DCMAKE_PREFIX_PATH="D:/QtProject/op-SDK/dist"
cmake --build build-consumer
$env:PATH = "D:/QtProject/op-SDK/dist/bin;$env:PATH"
./build-consumer/op_consumer.exe "D:/media/test.mp4"
```

---

## 4. 手工链接（不用 CMake 的人）

Windows（MSVC）：

```bat
cl app.c /I dist\include /utf-8 dist\lib\op_sdk.lib
:: 运行时需要 dist\bin\op_sdk.dll + av*.dll 在 PATH 或 exe 同目录
```

Linux：

```bash
cc app.c -I dist/include -L dist/lib -lop_sdk -Wl,-rpath,'$ORIGIN/lib'
```

---

## 5. 想做成"只有一个 DLL"怎么办

现在 FFmpeg 是**共享链接**（`lib/*.lib` 都是导入库，几十 KB），所以发行包要带 `av*.dll`。
要变成单文件，需要换成 FFmpeg 的**静态库**构建（`--enable-static --disable-shared`），
并确保运行时库一致（`/MT`），否则会 C 运行时冲突。这是发布形态的选择，不影响当前接口。

---

## 6. 版本与 ABI

- `op_version(&major,&minor)` 返回接口版本（当前 1.0），和 `OP_API_VERSION_*` 宏一致；
- DLL/SO 文件名带 `PROJECT_VERSION`（1.0.0），Linux 的 SONAME 是 `libop_sdk.so.1`；
- 只要**不删字段、不改已有函数签名**，加函数/加结构体尾部字段都是兼容的；
- `op_frame` / `op_event` 是扁平结构体，调用方按值读，跨编译器也能用；
- **不要**把 C++ 类型加进这个头文件（`std::string`、`bool`、异常），那会立刻毁掉 ABI。

---

## 7. 多平台产物

同一次 `v1.0` 发布按平台出不同资产，调用方按自己的平台取一个即可：

| 平台 | 资产 | 库文件 |
|---|---|---|
| Windows x64 | `op-sdk-1.0.0-win64.zip` | `bin/op_sdk.dll` + `av*.dll` |
| Linux x64 | `op-sdk-1.0.0-linux-x64.tar.gz` | `lib/libop_sdk.so*` + `libav*.so*` |
| HarmonyOS arm64 | `op-sdk-1.0.0-ohos-arm64.zip` | `lib/libop_sdk.so` + `libav*.so` + `libc++_shared.so` |
| Android arm64 | `op-sdk-1.0.0-android-arm64.zip` | `lib/libop_sdk.so` + `libav*.so` + `libc++_shared.so` |

### 7.1 Linux x64

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DOP_BUILD_SHARED=ON -DFFMPEG_ROOT=/path/to/ffmpeg-linux-shared
cmake --build build-linux && ctest --test-dir build-linux --output-on-failure
cmake --install build-linux --prefix dist-linux
```

`libop_sdk.so` 的 `RUNPATH` 是 `$ORIGIN`，所以 FFmpeg 的 `.so` 只要和它放同一目录，
调用方**不用**设 `LD_LIBRARY_PATH`。

### 7.2 HarmonyOS arm64

```powershell
$env:OHOS_NDK = "D:/ohos-sdk/default/openharmony/native"
cmake -S . -B build-ohos -G Ninja `
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ohos-arm64.cmake `
      -DFFMPEG_ROOT=D:/deps/ffmpeg-ohos-shared `
      -DOP_BUILD_SHARED=ON -DOP_PLAIN_SO=ON -DOP_BUILD_TESTS=OFF
cmake --build build-ohos
```

- `-DOP_PLAIN_SO=ON`：产物就叫 `libop_sdk.so`（不带 `.so.1`），har 里按原名加载；
- FFmpeg 必须用**同一套 ohos clang + sysroot** 编（`--target-os=linux --arch=aarch64`），
  否则 musl/arm64 ABI 对不上；
- `libc++_shared.so` 要从 NDK 一起拷进 `libs/arm64-v8a/`；
- ArkTS 不能直接调 C 函数，需要一层很薄的 NAPI 包装 `.so`（`napi_module_register` + 方法表），
  它链接 `libop_sdk.so`。两个 `.so` 都放 `libs/arm64-v8a/`。

### 7.3 Android arm64

```powershell
$env:ANDROID_NDK = "D:/deps/android-ndk-r27c"
cmake -S . -B build-android -G Ninja `
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android-arm64.cmake `
      -DFFMPEG_ROOT=D:/deps/ffmpeg-android-arm64 `
      -DOP_BUILD_SHARED=ON -DOP_PLAIN_SO=ON -DOP_BUILD_TESTS=OFF
cmake --build build-android
```

产物直接放进 `jniLibs/arm64-v8a/`：`libop_sdk.so` + `libav*.so` + `libc++_shared.so`，
用 JNI 包一层即可。

> 交叉编译的 FFmpeg 用 `--disable-autodetect --disable-vulkan`，
> 避免 configure 去猜主机上的库/头文件；协议（file/http/rtsp/rtmp）是 FFmpeg 内置的，不受影响。

## 8. 常见问题


| 现象 | 原因 | 处理 |
|---|---|---|
| 链接报 `LNK2019: op_create` | 只 link 了 op_core，没 link op_sdk | 链 `op::op_sdk` / `op_sdk.lib` |
| 运行报"找不到 op_sdk.dll" | DLL 不在 exe 目录 / PATH | 把 `bin\op_sdk.dll` 和 `av*.dll` 放一起 |
| 运行报"找不到 avcodec-62.dll" | 只拷了 op_sdk.dll | FFmpeg DLL 要一起发 |
| MSVC 刷 C4819 | 头文件是 UTF-8，源文件按 GBK 读 | 编译加 `/utf-8`（推荐），BOM 也能兜底 |
| `find_package(op_sdk)` 找不到 | 没设 `CMAKE_PREFIX_PATH` | 指到安装前缀（含 `lib/cmake/op_sdk`） |
