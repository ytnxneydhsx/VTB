# Live2D 板端帧追踪

Live2D 通过 Perfetto SDK 的 system backend 注册内置数据源 `track_event`，
VTB 的线程帧追踪使用 `vtb.thread_frame_trace_flow` 分类。板端独立运行
`tracebox traced` 和 `tracebox perfetto`。采集服务拥有内存缓冲，录制进程按
Live2D 服务的启动和退出创建并关闭文件。

## 构建

使用与 Live2D 媒体宿主相同的 ARM64 工具链。配置已有的嵌入式 Godot 构建时加上
`-DVTB_PERFETTO_SDK_ROOT=/path/to/perfetto/sdk`。该目录包含 `include/perfetto.h` 和
`lib/libperfetto.a`。这个分支的 Live2D 构建默认开启追踪支持。需要对照构建时可指定
`-DVTB_ENABLE_FRAME_TRACING=OFF`。应用向系统后端注册后最多等待一秒，采集端不可用时
媒体流程继续启动。

## 板端服务

把匹配版本的 Linux ARM64 `tracebox`、`record-frame-trace.py`、`frame-trace.pbtxt`
放到 `/opt/vtb/perfetto-v59.1/`，把两个 `.service` 文件放到
`/etc/systemd/system/`。为 root 运行的 Live2D systemd 服务安装
`30-frame-trace.conf` drop-in，然后重新加载 systemd 配置。已安装旧版开机录制的板卡，
迁移时先用 `sudo systemctl disable --now vtb-frame-trace.service` 移除旧的开机启动设置。

每次 Live2D 服务启动前，drop-in 启动独立录制单元并等待 Perfetto 就绪。Live2D 退出后，
drop-in 停掉录制单元，Perfetto 完成写盘。由 systemd 重启 Live2D 时也会重新开一份文件。
用命令行直接启动 Live2D 时，可用 `PERFETTO_PRODUCER_SOCK_NAME=/run/vtb-perfetto/producer`
连接采集服务，按次留档由 systemd 服务入口触发。

板端每 10 秒将 `64 MiB` 环形缓冲中的可读事件写入本次运行的
`/var/lib/vtb-frame-trace/vtb-live2d-<UTC时间>.pftrace`。每次运行开一份独立文件。
单个文件达到 `128 MiB` 时结束本次归档。录制器最多保留四份按次录制的文件。
当前定时写入采用 Linux 文件系统缓存，配置 `fflush_post_write: FFLUSH_ENABLED`
可在每轮写入后显式同步到存储。

查看或导出正在录制的会话时，在板端执行：

```sh
sudo env PERFETTO_CONSUMER_SOCK_NAME=/run/vtb-perfetto/consumer \
  /opt/vtb/perfetto-v59.1/tracebox perfetto --query
sudo env PERFETTO_CONSUMER_SOCK_NAME=/run/vtb-perfetto/consumer \
  /opt/vtb/perfetto-v59.1/tracebox perfetto --clone-by-name vtb-frame \
  --no-clobber -o /path/to/frame-trace.pftrace
```

## 在 Mac 拉取快照

在项目根目录执行 `./support/perfetto/pull-frame-trace.sh`。脚本通过已有的
`rk3588-direct` SSH 别名导出快照，保存到 `~/Downloads/VTB-Traces/` 并打印文件路径。
可传入一个目录作为保存位置。板端录制会话在导出期间继续运行。

在 [Perfetto Web UI](https://ui.perfetto.dev/) 选择 **Open trace file**，打开脚本打印的文件。

## 在 Mac 查看短片段

首次运行，从 [Perfetto v59.1](https://github.com/google/perfetto/releases/tag/v59.1)
下载 Mac 架构对应的发行包。将其中的 `trace_processor_shell` 放入
`build/perfetto-viewer/bin/` 或 `PATH`，也可用 `--trace-processor` 指定路径。

```sh
python3 support/perfetto/watch-frame-trace.py
```

打开 `http://127.0.0.1:8770/`。页面通过 `rk3588-direct` 采集一次 9 秒短片段，
显示采集、跟踪和 NV12 输出帧率、队列淘汰、分段耗时及一条帧关联，并将片段载入
官方 Perfetto Web UI。之后点击「刷新片段」才会再次采集，完成后更新统计和时间线。
关闭页面取消进行中的采集。终端按 Ctrl-C 停止 Mac 本地入口，板端常驻录制继续运行。

每次请求使用现有 `traced` 上一个独立的 4 MiB 内存会话，板端直接把短片段经 SSH
返回到 Mac。每次请求只显示本次采样。带新分类埋点的 Live2D 运行期间才有帧事件。
归档会话还在录制时可通过 `pull-frame-trace.sh` 导出快照，结束后可取对应运行的文件。

## 帧关联和统计

`v4l2.dequeue` 在成功 `DQBUF` 后生成，`render.nv12_ready` 在 NV12 租约返回且尚未
交给媒体 Sink 时生成。两段跨线程传递基于 camera frame ID 和 snapshot generation
分别建立 Flow。摄像头序号从零开始。输出上的 snapshot generation 为零表示初始帧尚未
取得跟踪快照。视角更新和无有效人脸时仍保留摄像头来源，`tracking_valid` 区分推理状态。
`new_snapshot` 表示消费这份快照后的首张 NV12。后续帧可复用或插值参数。
这一关联记录宿主消费快照的时间，最终像素开始响应某个参数的时间还受插值影响。

启用媒体输出时，`media.appsrc.submit` 与 `media.appsrc.output` 按 NV12 输出序号
建立 Flow。输出序号在应用侧写成 `GstBuffer::offset = sequence - 1`，采集端从原始
NV12 缓冲区还原它。appsrc 输出端为每个已启用的原始 NV12 编码队列开启独立 Flow，
在各自的 `media.raw_queue.input` 和 `media.raw_queue.output` 上显示流转，轨道按
实际执行线程归入「帧链路」。这组事件的范围到原始 NV12 队列出队为止。

RTSP 的 H.264 队列使用 `media.rtsp.queue.input` 和 `media.rtsp.queue.output`
记录等待时间，按 `encoded_id` 建立 Flow。共享媒体内由 appsrc 直接接到 RTP
打包器，`media.rtsp.appsrc.submit` 和 `media.rtsp.rtp_input` 按单次提交的
`delivery_id` 建立另一段 Flow。两个提交点事件都带有 `encoded_id`，可回查
编码队列；未捕获到来源时该字段为零。重放缓存关键帧会获得新的 `delivery_id`。
`media.rtsp.rtp_input` 表示进入打包器。

在 Perfetto UI 中，展开 `vtb-live2d → 帧链路` 即可按线程查看帧事件。
轨道标出线程名和实际 TID，包括主线程、采集、跟踪和渲染线程。V4L2、RGB 预处理、
RKNN、NV12 池取槽和同步 RGA 调用都在这个组中。通过同目录的 SQL 可以得到各段 FPS、队列淘汰数、
P50/P95 和 trace 丢失统计。

```sh
trace_processor_shell query -f support/perfetto/frame-summary.sql frame-trace.pftrace
```
