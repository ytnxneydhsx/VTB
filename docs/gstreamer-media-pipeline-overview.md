# GStreamer 媒体 Pipeline 全貌

本文说明 Live2D 的 NV12 画面进入 GStreamer 后，如何同时输出 WebRTC、RTSP 和
USB UVC。图中大框表示独立的 GStreamer pipeline，箭头表示 Buffer 的流动方向。

## Pipeline 框型图

```mermaid
flowchart TB
    RGA["RGA<br/>输出 NV12"]

    subgraph MAIN["Pipeline 1：主媒体 pipeline（vtb-media，长期运行）"]
        direction TB

        RAW_SRC["NV12 appsrc<br/>共用 DMA-BUF caps"]
        RAW_TEE{{"NV12 tee<br/>nv12-tee"}}

        subgraph H264["H.264 编码与分流"]
            direction LR
            H264_Q["queue<br/>h264-raw-queue"]
            H264_ENC["MPP H.264 编码器<br/>mpph264enc"]
            H264_PARSE["H.264 解析器<br/>h264parse"]
            H264_TEE{{"H.264 tee<br/>encoded-h264-tee"}}

            H264_Q --> H264_ENC --> H264_PARSE --> H264_TEE
        end

        subgraph WEBRTC["WebRTC 分支"]
            direction LR
            WEBRTC_Q["queue<br/>webrtc-h264-queue"]
            WEBRTC_SINK["webrtcsink"]
            WEBRTC_CLIENTS["浏览器 WebRTC 客户端"]

            WEBRTC_Q --> WEBRTC_SINK --> WEBRTC_CLIENTS
        end

        subgraph RTSP_INPUT["RTSP 编码数据出口"]
            direction LR
            RTSP_Q["queue<br/>rtsp-h264-queue"]
            H264_APPSINK["appsink<br/>rtsp-server-input"]

            RTSP_Q --> H264_APPSINK
        end

        subgraph UVC["USB UVC 分支"]
            direction LR
            MJPEG_Q["queue<br/>mjpeg-raw-queue"]
            MJPEG_ENC["MPP MJPEG 编码器<br/>mppjpegenc"]
            JPEG_PARSE["jpegparse"]
            JPEG_Q["queue<br/>uvc-mjpeg-output-queue"]
            UVC_SINK["uvcsink"]
            UVC_HOST["Mac / USB UVC 主机"]

            MJPEG_Q --> MJPEG_ENC --> JPEG_PARSE --> JPEG_Q --> UVC_SINK --> UVC_HOST
        end

        RAW_SRC --> RAW_TEE
        RAW_TEE --> H264_Q
        RAW_TEE --> MJPEG_Q
        H264_TEE --> WEBRTC_Q
        H264_TEE --> RTSP_Q
    end

    subgraph BRIDGE["应用层桥接（两个 pipeline 之间）"]
        direction LR
        CALLBACK["appsink new-sample 回调"]
        CACHE["保存 H.264 caps<br/>缓存最近一个 IDR"]
        PUSH["gst_app_src_push_buffer()"]

        CALLBACK --> CACHE --> PUSH
    end

    subgraph RTSP["Pipeline 2：共享 RTSP media pipeline（按连接生命周期创建）"]
        direction LR
        RTSP_APPSRC["RTSP appsrc<br/>rtsp-h264-source"]
        RTSP_MEDIA_Q["queue"]
        RTP_PAY["rtph264pay"]
        RTSP_STREAM["GstRTSPStream"]

        RTSP_APPSRC --> RTSP_MEDIA_Q --> RTP_PAY --> RTSP_STREAM
    end

    RTSP_A["RTSP 客户端 A"]
    RTSP_B["RTSP 客户端 B"]
    RTSP_C["RTSP 客户端 C"]

    RGA --> RAW_SRC
    H264_APPSINK --> CALLBACK
    PUSH --> RTSP_APPSRC
    RTSP_STREAM --> RTSP_A
    RTSP_STREAM --> RTSP_B
    RTSP_STREAM --> RTSP_C

    classDef source fill:#e8f4ff,stroke:#2878b5,stroke-width:2px,color:#102a43;
    classDef encode fill:#fff4d6,stroke:#b7791f,stroke-width:2px,color:#4a2c00;
    classDef output fill:#e8f8ee,stroke:#2f855a,stroke-width:2px,color:#163c2a;
    classDef bridge fill:#f4ebff,stroke:#805ad5,stroke-width:2px,color:#322659;

    class RGA,RAW_SRC,RAW_TEE source;
    class H264_Q,H264_ENC,H264_PARSE,H264_TEE,MJPEG_Q,MJPEG_ENC,JPEG_PARSE,JPEG_Q encode;
    class WEBRTC_Q,WEBRTC_SINK,WEBRTC_CLIENTS,RTSP_Q,H264_APPSINK,UVC_SINK,UVC_HOST,RTSP_APPSRC,RTSP_MEDIA_Q,RTP_PAY,RTSP_STREAM,RTSP_A,RTSP_B,RTSP_C output;
    class CALLBACK,CACHE,PUSH bridge;
```

## Pipeline 边界

- `nv12-source` 声明共用的 NV12 DMA-BUF caps。RTSP、WebRTC 和 UVC 的目标帧率
  都来自 `render.output_fps`，示例配置为 60 fps。
- `Pipeline 1` 在程序运行期间持续工作。H.264 只由 `mpph264enc` 编码一次，编码结果由
  `encoded-h264-tee` 同时交给 WebRTC 和 RTSP 数据出口。
- WebRTC 位于主 pipeline 内，直接接收已经编码好的 H.264，不会再创建一套 H.264
  编码器。
- UVC 同样位于主 pipeline 内，使用 `mppjpegenc` 输出 USB 主机所需的 MJPEG。
- `Pipeline 2` 由 `GstRTSPServer` 管理。同时在线的 RTSP 客户端共享一个 `appsrc`、
  `queue`、`rtph264pay` 和 `GstRTSPStream`。
- `appsink` 回调和 `RTSP appsrc` 构成两个 pipeline 之间的桥。应用只向共享 RTSP
  pipeline 提交一次 H.264 Buffer，后续客户端分发由 `GstRTSPStream` 完成。

## RTSP media 生命周期

```mermaid
flowchart LR
    FIRST["第一个 RTSP 客户端连接"]
    CREATE["创建 shared GstRTSPMedia"]
    SHARE["多个客户端共享同一条<br/>RTSP media pipeline"]
    LAST["最后一个客户端断开"]
    UNPREPARE["media unprepare"]
    RELEASE["移出 factory 缓存<br/>释放 appsrc 和 media 引用"]
    NEXT["后续客户端连接"]

    FIRST --> CREATE --> SHARE --> LAST --> UNPREPARE --> RELEASE --> NEXT
    NEXT --> CREATE
```
