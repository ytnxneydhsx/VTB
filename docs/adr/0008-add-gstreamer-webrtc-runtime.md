# ADR 0008：为 RK3588 构建完整 GStreamer WebRTC 运行时

## 状态

新版 Live2D 已在 RK3588 上验证，使用
`/opt/vtb/gstreamer-1.28.9-webrtc`。同一个进程监听 RTSP 8554、网页 8080
和信令 8443。信令已注册 `live2d` 视频源，Mac 请求网页和播放器脚本均返回
HTTP 200。Mac Chrome 已收到并解码 Live2D 画面，视频可单独全屏播放。

## 背景

移除 MediaMTX 后，进程内 `GstRTSPServer` 可以继续服务 RTSP 客户端，但
浏览器通常不能直接消费 RTSP。此前的 GStreamer 1.28.9 构建使用
`auto_features=disabled`，只启用 RTSP、UVC 和 Rockchip MPP 当前链路所需的
最小插件，因此没有 ICE、DTLS、SRTP、SCTP、`webrtcbin` 和 `webrtcsink`。

继续使用 MediaMTX 可以恢复 RTSP 到 WebRTC 的协议转换。当前目标希望媒体、
信令与浏览器入口统一在 GStreamer 体系中，因此在现有应用里直接分流编码后的
H.264，并让 `webrtcsink` 提供网页和信令。

## 决策

在现有版本化前缀 `/opt/vtb/gstreamer-1.28.9` 内增加：

- gst-plugins-bad 的 `webrtcbin`、DTLS、SRTP 和内部 usrsctp/SCTP；
- libnice 0.1.24 和 `libgstnice.so`，提供 ICE 及 `nicesrc`、`nicesink`；
- OpenSSL 3.5.9，提供 DTLS 所需的密码能力；
- libsrtp 2.8.0，提供 SRTP；
- gst-plugins-rs 0.15 的 `webrtcsink`，启用内置信令/Web 服务和 rustls；
- `gstwebrtc-api` 的生产浏览器资源。

应用通过 `output.webrtc.enabled` 开启 WebRTC。启用时，`h264parse` 后
的 tee 和两个有界队列分别供给原 RTSP appsink 和 `webrtcsink`，两路共用
同一个 MPP H.264 编码器。WebRTC 内部需要 `errorignore`，随
gst-plugins-bad 的 `debugutilsbad` 构建。

所有新增源码固定到构建脚本记录的提交，继续使用 Buildroot/Godot GCC
13.2.0 和 Rust 1.94.0 交叉编译为 AArch64。Cargo 使用命令行 Git 客户端，
让远端构建主机的 HTTPS 代理配置稳定生效。

## 边界与代价

`webrtcsink` 把命令行原型和简单应用接入压缩到一个高层元件，但 WebRTC
仍需要信令、ICE 候选交换、DTLS/SRTP 和浏览器页面。公网或复杂 NAT 环境
仍可能需要外部 STUN/TURN 服务。

新增运行库会扩大部署包和依赖面。版本化前缀隔离了这些库，板端原有
GStreamer 1.24.2 不受替换。现阶段保留进程内 RTSP 输出，WebRTC 作为新增
输出路径接入；是否移除 RTSP 由后续产品需求决定。

## 验证要求

交叉构建阶段检查：

1. `webrtcbin`、DTLS、SRTP、SCTP、`webrtcsink` 和 Rockchip MPP 插件均为
   AArch64 ELF；
2. 动态依赖指向版本化前缀中的 OpenSSL、libsrtp 和 GStreamer 库；
3. `gstwebrtc-api` 生成生产资源；
4. 构建包、源码提交、工具链和 SHA-256 写入 manifest。

RK3588 已使用独立插件注册表通过 `gst-inspect-1.0` 检查。Mac Chrome
完成首次播放和全屏切换。重连、浏览器实际解码帧率和端到端延迟待测。
