# op-core

播放器内核化的实验仓。当前内容是从 [QtFFmpegPlayer](https://github.com/Oolongmilkcover/QtFFmpegPlayer)
搬过来的 Qt 播放器代码（不含推流端），下一步在这里把与 Qt 无关的部分抽出来：

```text
core/        零 Qt：demux / 解码 / 队列 / 同步 / 时钟
platform/    接口层：IAudioSink、IRenderTarget
app/         Qt 壳（Widgets + QOpenGLWidget + QAudioSink）
```

> 本 README 是占位稿，等目录结构调整完成后再正式写。