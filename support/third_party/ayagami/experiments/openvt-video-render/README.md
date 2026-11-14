# RK3588 OpenVT 视频渲染实验

这个实验使用已经验证的 Godot + Ayagami + Xorg + Mali-G610 路径，把指定 Live2D 模型渲染为本地 MJPEG AVI 文件。Godot 将 1920×1080 `SubViewport` 逐帧保存为 JPEG，FFmpeg 只负责把 300 张 JPEG 封装进 AVI，不重新编码。

## 固定测试条件

- 模型：`/home/huangnv/open-vt-model/阿芙洛狄忒模型文件/fense_vts/fense.model3.json`
- 输出：1920×1080、30 FPS、300 帧、10 秒
- 背景：中性深灰色
- 动作：默认姿态、眨眼、转头、张嘴、`yaotou.motion3.json`
- 音频：无

## 部署位置

把本目录复制到 Ayagami 测试项目的：

```text
/home/huangnv/open-vt-test/support/third_party/ayagami/experiments/openvt-video-render/
```

场景入口为：

```text
res://experiments/openvt-video-render/render_video.tscn
```

## 边界

本实验不修改 Xorg、LightDM、GPU 驱动、内核、设备树或开机服务。输出文件、逐帧 JPEG 和运行日志保存在独立任务目录，不进入 Git。

媒体文件的 30 FPS 是容器时间轴帧率。日志中的 `throughput_fps` 记录包含渲染、GPU读回和JPEG保存的实际墙钟吞吐率，两者需要分别解释。

`run_render_on_rk.sh` 是板端运行入口。它拒绝覆盖已有输出目录，记录输入哈希、Godot日志、墙钟时间、进程RSS与CPU tick采样、GPU负载、`ffprobe` 结果和产物SHA-256。它不会安装软件，也不会启动、停止或修改 Xorg/LightDM。
