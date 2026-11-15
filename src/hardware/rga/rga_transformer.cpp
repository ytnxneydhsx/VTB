#include "vtb/hardware/rga/rga_transformer.hpp"

#include <stdexcept>
#include <utility>

namespace vtb {

RgaTransformRequest::RgaTransformRequest(
    RgaTransformOperation operation,
    std::shared_ptr<RgaBuffer> source,
    std::shared_ptr<RgaBuffer> destination,
    RgaRectangle source_rectangle)
    : operation_(operation),
      source_(std::move(source)),
      destination_(std::move(destination)),
      source_rectangle_(source_rectangle) {
    if (!source_ || !destination_) {
        throw std::invalid_argument(
            "RGA transform request buffers must not be null");
    }
}

RgaTransformOperation RgaTransformRequest::operation() const noexcept {
    return operation_;
}

const std::shared_ptr<RgaBuffer>&
RgaTransformRequest::source() const noexcept {
    return source_;
}

const std::shared_ptr<RgaBuffer>&
RgaTransformRequest::destination() const noexcept {
    return destination_;
}

const RgaRectangle&
RgaTransformRequest::source_rectangle() const noexcept {
    return source_rectangle_;
}

void RgaTransformer::transform(const RgaTransformRequest& request) {
    if (request.operation() == RgaTransformOperation::crop_scale_convert) {
        device_.crop_scale_convert(
            *request.source(),
            request.source_rectangle(),
            *request.destination());
        return;
    }
    if (request.operation() == RgaTransformOperation::rgba_to_nv12) {
        device_.convert_rgba_to_nv12(
            *request.source(), *request.destination());
        return;
    }
    throw std::invalid_argument("Unsupported RGA transform operation");
}

}  // namespace vtb
