#pragma once

#include "vtb/frame_ring_buffer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vtb {

struct CaptureConfig {
    std::string device = "/dev/video0";
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t pixel_format = 0;
    std::uint32_t fps = 30;
    std::uint32_t driver_buffer_count = 4;
    std::uint64_t max_frames = 0;
};

struct NegotiatedFormat {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pixel_format = 0;
    std::uint32_t fps_numerator = 0;
    std::uint32_t fps_denominator = 0;
    std::uint32_t size_image = 0;
};

class V4L2Capture {
public:
    V4L2Capture(CaptureConfig config, FrameRingBuffer& output);
    ~V4L2Capture();

    V4L2Capture(const V4L2Capture&) = delete;
    V4L2Capture& operator=(const V4L2Capture&) = delete;

    // Blocks while capturing. stop_requested may be set from another thread.
    // Any setup or streaming error is reported by throwing std::runtime_error.
    void run(const std::atomic_bool& stop_requested);

    const NegotiatedFormat& negotiatedFormat() const noexcept;

private:
    struct MappedBuffer {
        void* address = nullptr;
        std::size_t length = 0;
    };

    void openDevice();
    void configureDevice();
    void mapBuffers();
    void queueBuffers();
    void startStreaming();
    void stopStreaming() noexcept;
    void cleanup() noexcept;

    CaptureConfig config_;
    FrameRingBuffer& output_;
    NegotiatedFormat negotiated_;
    int fd_ = -1;
    bool streaming_ = false;
    std::vector<MappedBuffer> mapped_buffers_;
};

std::uint32_t makeFourcc(const std::string& text);
std::string fourccToString(std::uint32_t fourcc);

}  // namespace vtb
