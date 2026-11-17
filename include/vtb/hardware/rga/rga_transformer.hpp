#pragma once

#include <memory>

#include "vtb/hardware/rga/rga_device.hpp"

namespace vtb {

enum class RgaTransformOperation {
    crop_scale_convert,
    rgba_to_nv12,
    rgba_to_nv12_flip_vertical,
};

/** @brief 已解析为 RgaBuffer 租约的通用 Worker 执行请求。 */
class RgaTransformRequest final {
public:
    RgaTransformRequest(
        RgaTransformOperation operation,
        std::shared_ptr<RgaBuffer> source,
        std::shared_ptr<RgaBuffer> destination,
        RgaRectangle source_rectangle);
    ~RgaTransformRequest() = default;

    RgaTransformRequest(const RgaTransformRequest&) = default;
    RgaTransformRequest& operator=(const RgaTransformRequest&) = default;
    RgaTransformRequest(RgaTransformRequest&&) noexcept = default;
    RgaTransformRequest& operator=(RgaTransformRequest&&) noexcept = default;

    RgaTransformOperation operation() const noexcept;
    const std::shared_ptr<RgaBuffer>& source() const noexcept;
    const std::shared_ptr<RgaBuffer>& destination() const noexcept;
    const RgaRectangle& source_rectangle() const noexcept;

private:
    RgaTransformOperation operation_ =
        RgaTransformOperation::crop_scale_convert;
    std::shared_ptr<RgaBuffer> source_;
    std::shared_ptr<RgaBuffer> destination_;
    RgaRectangle source_rectangle_{0, 0, 0, 0};
};

/** @brief 一个业务线程独占的同步 RGA 命令解释器。 */
class RgaTransformer final {
public:
    RgaTransformer() = default;
    ~RgaTransformer() = default;

    RgaTransformer(const RgaTransformer&) = delete;
    RgaTransformer& operator=(const RgaTransformer&) = delete;
    RgaTransformer(RgaTransformer&&) = delete;
    RgaTransformer& operator=(RgaTransformer&&) = delete;

    void transform(const RgaTransformRequest& request);

private:
    RgaDevice device_;
};

}  // namespace vtb
