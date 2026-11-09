#include "vtb/face/cpu_image_transformer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vtb {
namespace {

class RgbPixel {
public:
    RgbPixel(float red, float green, float blue) noexcept
        : red_(red), green_(green), blue_(blue) {}
    ~RgbPixel() = default;

    RgbPixel(const RgbPixel&) = default;
    RgbPixel& operator=(const RgbPixel&) = default;
    RgbPixel(RgbPixel&&) noexcept = default;
    RgbPixel& operator=(RgbPixel&&) noexcept = default;

    float red() const noexcept { return red_; }
    float green() const noexcept { return green_; }
    float blue() const noexcept { return blue_; }

private:
    float red_ = 0.0F;
    float green_ = 0.0F;
    float blue_ = 0.0F;
};

class KeepStorageAlive final {
public:
    explicit KeepStorageAlive(
        std::shared_ptr<std::vector<std::byte>> storage)
        : storage_(std::move(storage)) {}
    ~KeepStorageAlive() = default;

    KeepStorageAlive(const KeepStorageAlive&) = default;
    KeepStorageAlive& operator=(const KeepStorageAlive&) = default;
    KeepStorageAlive(KeepStorageAlive&&) noexcept = default;
    KeepStorageAlive& operator=(KeepStorageAlive&&) noexcept = default;

    void operator()() const noexcept {}

private:
    std::shared_ptr<std::vector<std::byte>> storage_;
};

std::uint8_t byte_value(const std::byte* data, std::size_t index) noexcept {
    return std::to_integer<std::uint8_t>(data[index]);
}

float clamp_channel(float value) noexcept {
    return std::max(0.0F, std::min(255.0F, value));
}

RgbPixel yuv_to_rgb(std::uint8_t y, std::uint8_t u, std::uint8_t v) noexcept {
    const float luminance = static_cast<float>(y) - 16.0F;
    const float blue_difference = static_cast<float>(u) - 128.0F;
    const float red_difference = static_cast<float>(v) - 128.0F;
    const float red =
        (298.0F * luminance + 409.0F * red_difference + 128.0F) / 256.0F;
    const float green =
        (298.0F * luminance - 100.0F * blue_difference -
         208.0F * red_difference + 128.0F) /
        256.0F;
    const float blue =
        (298.0F * luminance + 516.0F * blue_difference + 128.0F) / 256.0F;
    return RgbPixel(
        clamp_channel(red), clamp_channel(green), clamp_channel(blue));
}

RgbPixel read_rgb(const FrameView& frame, std::uint32_t x, std::uint32_t y) {
    const FramePlaneView& plane = frame.plane(0);
    const std::byte* data = plane.data();
    if (data == nullptr) {
        throw std::invalid_argument(
            "CpuImageTransformer requires CPU-addressable frame data");
    }

    const std::size_t stride = plane.stride_bytes();
    if (frame.pixel_format() == VideoPixelFormat::rgb888 ||
        frame.pixel_format() == VideoPixelFormat::bgr888) {
        const std::size_t offset =
            static_cast<std::size_t>(y) * stride +
            static_cast<std::size_t>(x) * 3U;
        const std::uint8_t first = byte_value(data, offset);
        const std::uint8_t green = byte_value(data, offset + 1U);
        const std::uint8_t third = byte_value(data, offset + 2U);
        if (frame.pixel_format() == VideoPixelFormat::rgb888) {
            return RgbPixel(first, green, third);
        }
        return RgbPixel(third, green, first);
    }

    if (frame.pixel_format() == VideoPixelFormat::yuyv) {
        const std::size_t pair_offset =
            static_cast<std::size_t>(y) * stride +
            static_cast<std::size_t>(x / 2U) * 4U;
        std::size_t y_offset = pair_offset;
        if ((x & 1U) != 0U) {
            y_offset += 2U;
        }
        return yuv_to_rgb(
            byte_value(data, y_offset),
            byte_value(data, pair_offset + 1U),
            byte_value(data, pair_offset + 3U));
    }

    if (frame.pixel_format() == VideoPixelFormat::nv12 ||
        frame.pixel_format() == VideoPixelFormat::nv21) {
        const std::size_t y_offset =
            static_cast<std::size_t>(y) * stride + x;
        const std::size_t chroma_base =
            stride * static_cast<std::size_t>(frame.height());
        const std::size_t chroma_offset =
            chroma_base + static_cast<std::size_t>(y / 2U) * stride +
            static_cast<std::size_t>(x / 2U) * 2U;
        std::uint8_t u = byte_value(data, chroma_offset);
        std::uint8_t v = byte_value(data, chroma_offset + 1U);
        if (frame.pixel_format() == VideoPixelFormat::nv21) {
            std::swap(u, v);
        }
        return yuv_to_rgb(byte_value(data, y_offset), u, v);
    }

    throw std::invalid_argument(
        "CpuImageTransformer does not support the source pixel format");
}

float interpolate(float top_left, float top_right, float bottom_left,
                  float bottom_right, float x_weight, float y_weight) noexcept {
    const float top = top_left + (top_right - top_left) * x_weight;
    const float bottom = bottom_left + (bottom_right - bottom_left) * x_weight;
    return top + (bottom - top) * y_weight;
}

std::uint8_t rounded_channel(float value) noexcept {
    const long rounded = std::lround(clamp_channel(value));
    return static_cast<std::uint8_t>(rounded);
}

void validate_source_frame(const FrameView& frame) {
    if (frame.plane_count() != 1) {
        throw std::invalid_argument(
            "CpuImageTransformer requires one source plane");
    }
    const FramePlaneView& plane = frame.plane(0);
    const std::size_t stride = plane.stride_bytes();
    std::size_t required_size = 0;
    if (frame.pixel_format() == VideoPixelFormat::rgb888 ||
        frame.pixel_format() == VideoPixelFormat::bgr888) {
        const std::size_t row_bytes =
            static_cast<std::size_t>(frame.width()) * 3U;
        if (stride < row_bytes) {
            throw std::invalid_argument("RGB source stride is too small");
        }
        required_size =
            stride * static_cast<std::size_t>(frame.height() - 1U) + row_bytes;
    } else if (frame.pixel_format() == VideoPixelFormat::yuyv) {
        if ((frame.width() & 1U) != 0U) {
            throw std::invalid_argument("YUYV source width must be even");
        }
        const std::size_t row_bytes =
            static_cast<std::size_t>(frame.width()) * 2U;
        if (stride < row_bytes) {
            throw std::invalid_argument("YUYV source stride is too small");
        }
        required_size =
            stride * static_cast<std::size_t>(frame.height() - 1U) + row_bytes;
    } else if (frame.pixel_format() == VideoPixelFormat::nv12 ||
               frame.pixel_format() == VideoPixelFormat::nv21) {
        if ((frame.width() & 1U) != 0U || (frame.height() & 1U) != 0U) {
            throw std::invalid_argument("NV12/NV21 source dimensions must be even");
        }
        if (stride < frame.width()) {
            throw std::invalid_argument("NV12/NV21 source stride is too small");
        }
        required_size = stride * static_cast<std::size_t>(frame.height()) +
            stride * static_cast<std::size_t>(frame.height() / 2U);
    } else {
        throw std::invalid_argument(
            "CpuImageTransformer does not support the source pixel format");
    }
    if (plane.data() == nullptr || plane.size_bytes() < required_size) {
        throw std::invalid_argument(
            "CpuImageTransformer source buffer is incomplete");
    }
}

}  // namespace

ModelInputLease CpuImageTransformer::prepare(
    const FrameView& frame,
    const CropRegion& crop,
    const ModelInputSpec& spec) {
    if (spec.pixel_format() != VideoPixelFormat::rgb888 ||
        spec.layout() != ModelInputLayout::nhwc ||
        spec.data_type() != ModelInputDataType::uint8) {
        throw std::invalid_argument(
            "CpuImageTransformer currently produces RGB888 UINT8 NHWC only");
    }
    validate_source_frame(frame);
    if (crop.x() + crop.width() > frame.width() ||
        crop.y() + crop.height() > frame.height()) {
        throw std::invalid_argument("Image transform crop exceeds source frame");
    }

    const std::size_t output_stride =
        static_cast<std::size_t>(spec.width()) * 3U;
    const std::size_t packed_output_size =
        output_stride * static_cast<std::size_t>(spec.height());
    const std::size_t output_size = spec.minimum_buffer_size_bytes();
    if (output_size < packed_output_size) {
        throw std::invalid_argument(
            "CPU model input storage is smaller than packed RGB888 data");
    }
    auto storage = std::make_shared<std::vector<std::byte>>(output_size);

    const float source_width = static_cast<float>(crop.width());
    const float source_height = static_cast<float>(crop.height());
    const float output_width = static_cast<float>(spec.width());
    const float output_height = static_cast<float>(spec.height());
    const float minimum_x = static_cast<float>(crop.x());
    const float minimum_y = static_cast<float>(crop.y());
    const float maximum_x =
        static_cast<float>(crop.x() + crop.width() - 1U);
    const float maximum_y =
        static_cast<float>(crop.y() + crop.height() - 1U);

    for (std::uint32_t output_y = 0; output_y < spec.height(); ++output_y) {
        float source_y = minimum_y +
            (static_cast<float>(output_y) + 0.5F) * source_height /
                output_height -
            0.5F;
        source_y = std::max(minimum_y, std::min(maximum_y, source_y));
        const std::uint32_t y0 = static_cast<std::uint32_t>(std::floor(source_y));
        const std::uint32_t y1 = std::min(y0 + 1U, crop.y() + crop.height() - 1U);
        const float y_weight = source_y - static_cast<float>(y0);

        for (std::uint32_t output_x = 0; output_x < spec.width(); ++output_x) {
            float source_x = minimum_x +
                (static_cast<float>(output_x) + 0.5F) * source_width /
                    output_width -
                0.5F;
            source_x = std::max(minimum_x, std::min(maximum_x, source_x));
            const std::uint32_t x0 =
                static_cast<std::uint32_t>(std::floor(source_x));
            const std::uint32_t x1 =
                std::min(x0 + 1U, crop.x() + crop.width() - 1U);
            const float x_weight = source_x - static_cast<float>(x0);

            const RgbPixel top_left = read_rgb(frame, x0, y0);
            const RgbPixel top_right = read_rgb(frame, x1, y0);
            const RgbPixel bottom_left = read_rgb(frame, x0, y1);
            const RgbPixel bottom_right = read_rgb(frame, x1, y1);

            const std::size_t output_offset =
                static_cast<std::size_t>(output_y) * output_stride +
                static_cast<std::size_t>(output_x) * 3U;
            (*storage)[output_offset] = static_cast<std::byte>(rounded_channel(
                interpolate(
                    top_left.red(), top_right.red(), bottom_left.red(),
                    bottom_right.red(), x_weight, y_weight)));
            (*storage)[output_offset + 1U] =
                static_cast<std::byte>(rounded_channel(interpolate(
                    top_left.green(), top_right.green(), bottom_left.green(),
                    bottom_right.green(), x_weight, y_weight)));
            (*storage)[output_offset + 2U] =
                static_cast<std::byte>(rounded_channel(interpolate(
                    top_left.blue(), top_right.blue(), bottom_left.blue(),
                    bottom_right.blue(), x_weight, y_weight)));
        }
    }

    std::array<FramePlaneView, FrameView::max_planes> planes{};
    planes[0] = FramePlaneView(
        storage->data(), output_size,
        static_cast<std::uint32_t>(output_stride), -1, 0);
    FrameView transformed(
        VideoPixelFormat::rgb888,
        FrameMemoryType::cpu,
        spec.width(),
        spec.height(),
        frame.frame_id(),
        frame.timestamp_ns(),
        frame.timestamp_source(),
        planes,
        1);
    ModelInputView view(
        std::move(transformed), spec, crop);
    return ModelInputLease(
        std::move(view), KeepStorageAlive(std::move(storage)));
}

}  // namespace vtb
