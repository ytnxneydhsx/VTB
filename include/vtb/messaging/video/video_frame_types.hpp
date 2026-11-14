#pragma once

/**
 * @file
 * @brief 视频帧消息共享的格式、内存和时间戳类型。
 */

namespace vtb {

enum class VideoPixelFormat {
    unknown,
    yuyv,
    nv12,
    nv21,
    rgb888,
    bgr888,
};

enum class FrameMemoryType {
    cpu,
    dma_buf,
};

enum class FrameTimestampSource {
    driver_monotonic,
    dequeue_monotonic,
    file_timeline,
};

}  // namespace vtb
