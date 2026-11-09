#include "vtb/face/rknn_face_models.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <rknn_api.h>

namespace vtb {
namespace {

void require_rknn_success(int result, const std::string& operation) {
    if (result != RKNN_SUCC) {
        throw std::runtime_error(
            operation + " failed with RKNN error " + std::to_string(result));
    }
}

[[noreturn]] void throw_errno(const std::string& operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void require_unit_interval(float value, const std::string& description) {
    if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
        throw std::invalid_argument(description);
    }
}

rknn_core_mask to_core_mask(RknnCore core) {
    if (core == RknnCore::core0) {
        return RKNN_NPU_CORE_0;
    }
    if (core == RknnCore::core1) {
        return RKNN_NPU_CORE_1;
    }
    if (core == RknnCore::core2) {
        return RKNN_NPU_CORE_2;
    }
    return RKNN_NPU_CORE_AUTO;
}

std::vector<std::byte> read_model(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("Unable to open RKNN model: " + path);
    }
    const std::streampos end = stream.tellg();
    if (end <= 0) {
        throw std::runtime_error("RKNN model is empty: " + path);
    }
    const auto size = static_cast<std::size_t>(end);
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("RKNN model is too large: " + path);
    }
    std::vector<std::byte> bytes(size);
    stream.seekg(0, std::ios::beg);
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        throw std::runtime_error("Unable to read RKNN model: " + path);
    }
    return bytes;
}

class RknnTensor {
public:
    RknnTensor(
        const rknn_tensor_attr& attr,
        const float* values,
        std::size_t value_count)
        : format_(attr.fmt),
          values_(values),
          value_count_(value_count) {
        if (attr.n_dims != dimensions_.size()) {
            throw std::runtime_error("RKNN tensor must have four dimensions");
        }
        if (format_ != RKNN_TENSOR_NCHW && format_ != RKNN_TENSOR_NHWC) {
            throw std::runtime_error("RKNN tensor layout must be NCHW or NHWC");
        }
        if (values_ == nullptr) {
            throw std::runtime_error("RKNN tensor requires an output buffer");
        }
        std::size_t expected = 1;
        for (std::size_t index = 0; index < dimensions_.size(); ++index) {
            dimensions_[index] = attr.dims[index];
            expected *= dimensions_[index];
        }
        if (expected != value_count_) {
            throw std::runtime_error("RKNN tensor element count mismatch");
        }
    }
    ~RknnTensor() = default;

    RknnTensor(const RknnTensor&) = default;
    RknnTensor& operator=(const RknnTensor&) = default;
    RknnTensor(RknnTensor&&) noexcept = default;
    RknnTensor& operator=(RknnTensor&&) noexcept = default;

    std::uint32_t channels() const noexcept {
        if (format_ == RKNN_TENSOR_NCHW) {
            return dimensions_[1];
        }
        return dimensions_[3];
    }

    std::uint32_t height() const noexcept {
        if (format_ == RKNN_TENSOR_NCHW) {
            return dimensions_[2];
        }
        return dimensions_[1];
    }

    std::uint32_t width() const noexcept {
        if (format_ == RKNN_TENSOR_NCHW) {
            return dimensions_[3];
        }
        return dimensions_[2];
    }

    float value(
        std::uint32_t channel,
        std::uint32_t y,
        std::uint32_t x) const {
        if (channel >= channels() || y >= height() || x >= width()) {
            throw std::out_of_range("RKNN tensor index is out of range");
        }
        std::size_t index = 0;
        if (format_ == RKNN_TENSOR_NCHW) {
            index = (static_cast<std::size_t>(channel) * height() + y) *
                width() + x;
        } else {
            index = (static_cast<std::size_t>(y) * width() + x) *
                channels() + channel;
        }
        return values_[index];
    }

private:
    rknn_tensor_format format_ = RKNN_TENSOR_UNDEFINED;
    std::array<std::uint32_t, 4> dimensions_{};
    const float* values_ = nullptr;
    std::size_t value_count_ = 0;
};

class RknnOutputLease {
public:
    RknnOutputLease(
        rknn_context context,
        std::vector<rknn_output>& outputs,
        const std::vector<RknnTensor>& tensors,
        bool& output_in_use) noexcept
        : context_(context),
          outputs_(&outputs),
          tensors_(&tensors),
          output_in_use_(&output_in_use),
          valid_(true) {}
    ~RknnOutputLease() {
        reset();
    }

    RknnOutputLease(const RknnOutputLease&) = delete;
    RknnOutputLease& operator=(const RknnOutputLease&) = delete;
    RknnOutputLease(RknnOutputLease&& other) noexcept
        : context_(other.context_),
          outputs_(other.outputs_),
          tensors_(other.tensors_),
          output_in_use_(other.output_in_use_),
          valid_(other.valid_) {
        other.valid_ = false;
        other.outputs_ = nullptr;
        other.tensors_ = nullptr;
        other.output_in_use_ = nullptr;
    }
    RknnOutputLease& operator=(RknnOutputLease&& other) noexcept {
        if (this != &other) {
            reset();
            context_ = other.context_;
            outputs_ = other.outputs_;
            tensors_ = other.tensors_;
            output_in_use_ = other.output_in_use_;
            valid_ = other.valid_;
            other.valid_ = false;
            other.outputs_ = nullptr;
            other.tensors_ = nullptr;
            other.output_in_use_ = nullptr;
        }
        return *this;
    }

    const std::vector<RknnTensor>& tensors() const {
        if (!valid_ || tensors_ == nullptr) {
            throw std::logic_error("RKNN output lease is not valid");
        }
        return *tensors_;
    }

private:
    void reset() noexcept {
        if (!valid_) {
            return;
        }
        if (outputs_ != nullptr && !outputs_->empty()) {
            rknn_outputs_release(
                context_, static_cast<std::uint32_t>(outputs_->size()),
                outputs_->data());
        }
        if (output_in_use_ != nullptr) {
            *output_in_use_ = false;
        }
        valid_ = false;
        outputs_ = nullptr;
        tensors_ = nullptr;
        output_in_use_ = nullptr;
    }

    rknn_context context_ = 0;
    std::vector<rknn_output>* outputs_ = nullptr;
    const std::vector<RknnTensor>* tensors_ = nullptr;
    bool* output_in_use_ = nullptr;
    bool valid_ = false;
};

float clamped_logit(float probability) noexcept {
    const float clamped =
        std::max(0.0000001F, std::min(0.9999999F, probability));
    return std::log(clamped / (1.0F - clamped)) / 16.0F;
}

bool tensor_shape_matches(
    const rknn_tensor_attr& attr,
    std::uint32_t channels,
    std::uint32_t height,
    std::uint32_t width) noexcept {
    if (attr.n_dims != 4 || attr.dims[0] != 1) {
        return false;
    }
    if (attr.fmt == RKNN_TENSOR_NCHW) {
        return attr.dims[1] == channels && attr.dims[2] == height &&
            attr.dims[3] == width;
    }
    if (attr.fmt == RKNN_TENSOR_NHWC) {
        return attr.dims[1] == height && attr.dims[2] == width &&
            attr.dims[3] == channels;
    }
    return false;
}

class RknnInputBinding final {
public:
    RknnInputBinding(
        rknn_context context,
        const FramePlaneView& plane,
        rknn_tensor_attr attribute,
        const std::string& model_path)
        : context_(context),
          attribute_(attribute),
          source_address_(plane.data()),
          source_fd_(plane.dma_buf_fd()),
          source_size_bytes_(plane.size_bytes()),
          source_stride_bytes_(plane.stride_bytes()),
          source_offset_bytes_(plane.offset_bytes()) {
        if (source_address_ == nullptr || source_fd_ < 0) {
            throw std::invalid_argument(
                "RKNN DMA-BUF input requires an address and file descriptor");
        }
        if (source_offset_bytes_ != 0) {
            throw std::invalid_argument(
                "RKNN DMA-BUF input requires a zero-offset plane");
        }
        if (source_size_bytes_ >
            static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
            throw std::invalid_argument("RKNN DMA-BUF input is too large");
        }

        duplicated_fd_ = fcntl(source_fd_, F_DUPFD_CLOEXEC, 0);
        if (duplicated_fd_ < 0) {
            throw_errno("duplicate RKNN input DMA-BUF");
        }
        try {
            mapping_ = mmap(
                nullptr,
                source_size_bytes_,
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                duplicated_fd_,
                0);
            if (mapping_ == MAP_FAILED) {
                throw_errno("mmap RKNN input DMA-BUF");
            }

            memory_ = rknn_create_mem_from_fd(
                context_,
                duplicated_fd_,
                mapping_,
                static_cast<std::uint32_t>(source_size_bytes_),
                0);
            if (memory_ == nullptr) {
                throw std::runtime_error(
                    "rknn_create_mem_from_fd failed for " + model_path);
            }
            require_rknn_success(
                rknn_set_io_mem(context_, memory_, &attribute_),
                "rknn_set_io_mem " + model_path);
        } catch (...) {
            reset();
            throw;
        }
    }

    ~RknnInputBinding() {
        reset();
    }

    RknnInputBinding(const RknnInputBinding&) = delete;
    RknnInputBinding& operator=(const RknnInputBinding&) = delete;
    RknnInputBinding(RknnInputBinding&&) = delete;
    RknnInputBinding& operator=(RknnInputBinding&&) = delete;

    bool matches(const FramePlaneView& plane) const noexcept {
        return source_address_ == plane.data() &&
            source_fd_ == plane.dma_buf_fd() &&
            source_size_bytes_ == plane.size_bytes() &&
            source_stride_bytes_ == plane.stride_bytes() &&
            source_offset_bytes_ == plane.offset_bytes();
    }

private:
    void reset() noexcept {
        if (memory_ != nullptr) {
            rknn_destroy_mem(context_, memory_);
            memory_ = nullptr;
        }
        if (mapping_ != MAP_FAILED) {
            munmap(mapping_, source_size_bytes_);
            mapping_ = MAP_FAILED;
        }
        if (duplicated_fd_ >= 0) {
            close(duplicated_fd_);
            duplicated_fd_ = -1;
        }
    }

    rknn_context context_ = 0;
    rknn_tensor_attr attribute_{};
    rknn_tensor_mem* memory_ = nullptr;
    const std::byte* source_address_ = nullptr;
    int source_fd_ = -1;
    std::size_t source_size_bytes_ = 0;
    std::uint32_t source_stride_bytes_ = 0;
    std::uint32_t source_offset_bytes_ = 0;
    int duplicated_fd_ = -1;
    void* mapping_ = MAP_FAILED;
};

}  // namespace

class RknnModel final {
public:
    RknnModel(std::string model_path, RknnCore core)
        : model_path_(std::move(model_path)) {
        std::vector<std::byte> model = read_model(model_path_);
        require_rknn_success(
            rknn_init(
                &context_, model.data(), static_cast<std::uint32_t>(model.size()),
                0, nullptr),
            "rknn_init " + model_path_);
        initialized_ = true;
        try {
            require_rknn_success(
                rknn_set_core_mask(context_, to_core_mask(core)),
                "rknn_set_core_mask " + model_path_);

            rknn_input_output_num io_count{};
            require_rknn_success(
                rknn_query(
                    context_, RKNN_QUERY_IN_OUT_NUM, &io_count,
                    sizeof(io_count)),
                "RKNN_QUERY_IN_OUT_NUM " + model_path_);
            if (io_count.n_input != 1 || io_count.n_output == 0) {
                throw std::runtime_error(
                    "OpenSeeFace RKNN model requires one input and at least one output");
            }

            input_attr_.index = 0;
            require_rknn_success(
                rknn_query(
                    context_, RKNN_QUERY_INPUT_ATTR, &input_attr_,
                    sizeof(input_attr_)),
                "RKNN_QUERY_INPUT_ATTR " + model_path_);
            if (!tensor_shape_matches(input_attr_, 3, 224, 224)) {
                throw std::runtime_error(
                    "OpenSeeFace RKNN input must have shape 1x3x224x224");
            }

            output_attrs_.resize(io_count.n_output);
            for (std::uint32_t index = 0; index < io_count.n_output; ++index) {
                output_attrs_[index].index = index;
                require_rknn_success(
                    rknn_query(
                        context_, RKNN_QUERY_OUTPUT_ATTR,
                        &output_attrs_[index], sizeof(output_attrs_[index])),
                    "RKNN_QUERY_OUTPUT_ATTR " + model_path_);
            }
            initialize_output_buffers();

            rknn_sdk_version version{};
            require_rknn_success(
                rknn_query(
                    context_, RKNN_QUERY_SDK_VERSION, &version,
                    sizeof(version)),
                "RKNN_QUERY_SDK_VERSION " + model_path_);
            api_version_ = version.api_version;
            driver_version_ = version.drv_version;
        } catch (...) {
            rknn_destroy(context_);
            context_ = 0;
            initialized_ = false;
            throw;
        }
    }

    ~RknnModel() {
        input_binding_.reset();
        if (initialized_) {
            rknn_destroy(context_);
        }
    }

    RknnModel(const RknnModel&) = delete;
    RknnModel& operator=(const RknnModel&) = delete;
    RknnModel(RknnModel&&) = delete;
    RknnModel& operator=(RknnModel&&) = delete;

    ModelInputSpec input_spec() const {
        std::size_t minimum_buffer_size = 224U * 224U * 3U;
        minimum_buffer_size = std::max(
            minimum_buffer_size,
            static_cast<std::size_t>(input_attr_.size));
        minimum_buffer_size = std::max(
            minimum_buffer_size,
            static_cast<std::size_t>(input_attr_.size_with_stride));
        return ModelInputSpec(
            224,
            224,
            VideoPixelFormat::rgb888,
            ModelInputLayout::nhwc,
            ModelInputDataType::uint8,
            ModelNormalization::rknn_runtime,
            minimum_buffer_size);
    }

    void require_output_shapes(
        const std::vector<std::array<std::uint32_t, 3>>& expected) const {
        if (output_attrs_.size() != expected.size()) {
            throw std::runtime_error(
                "Unexpected RKNN output count for " + model_path_);
        }
        for (const std::array<std::uint32_t, 3>& shape : expected) {
            bool found = false;
            for (const rknn_tensor_attr& attr : output_attrs_) {
                if (tensor_shape_matches(
                        attr, shape[0], shape[1], shape[2])) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw std::runtime_error(
                    "Unexpected RKNN output shape for " + model_path_);
            }
        }
    }

    RknnOutputLease run(const ModelInputView& input) {
        if (output_in_use_) {
            throw std::logic_error(
                "RKNN output buffers are still in use by the previous run");
        }
        const FrameView& frame = input.frame();
        const ModelInputSpec& spec = input.spec();
        if (spec.width() != 224 || spec.height() != 224 ||
            spec.pixel_format() != VideoPixelFormat::rgb888 ||
            spec.layout() != ModelInputLayout::nhwc ||
            spec.data_type() != ModelInputDataType::uint8 ||
            spec.normalization() != ModelNormalization::rknn_runtime) {
            throw std::invalid_argument(
                "RKNN OpenSeeFace input specification does not match the model");
        }
        if (frame.pixel_format() != VideoPixelFormat::rgb888 ||
            frame.width() != 224 || frame.height() != 224 ||
            frame.plane_count() != 1) {
            throw std::invalid_argument(
                "RKNN OpenSeeFace input must be 224x224 RGB888");
        }
        const FramePlaneView& plane = frame.plane(0);
        if (plane.data() == nullptr || plane.stride_bytes() != 224U * 3U ||
            plane.size_bytes() < spec.minimum_buffer_size_bytes()) {
            throw std::invalid_argument("RKNN input buffer is incomplete");
        }

        if (frame.memory_type() == FrameMemoryType::dma_buf) {
            if (used_cpu_input_) {
                throw std::logic_error(
                    "RKNN model input transport cannot change from CPU to DMA-BUF");
            }
            if (plane.dma_buf_fd() < 0 || plane.offset_bytes() != 0) {
                throw std::invalid_argument(
                    "RKNN DMA-BUF input requires a zero-offset file descriptor");
            }
            if (!input_binding_ || !input_binding_->matches(plane)) {
                rknn_tensor_attr binding_attr = input_attr_;
                binding_attr.type = RKNN_TENSOR_UINT8;
                binding_attr.fmt = RKNN_TENSOR_NHWC;
                binding_attr.pass_through = 0;
                binding_attr.h_stride = 0;
                input_binding_ = std::make_unique<RknnInputBinding>(
                    context_, plane, binding_attr, model_path_);
            }
        } else {
            if (input_binding_) {
                throw std::logic_error(
                    "RKNN model input transport cannot change from DMA-BUF to CPU");
            }
            used_cpu_input_ = true;
            rknn_input rknn_model_input{};
            rknn_model_input.index = 0;
            rknn_model_input.buf = const_cast<std::byte*>(plane.data());
            rknn_model_input.size = 224U * 224U * 3U;
            rknn_model_input.pass_through = 0;
            rknn_model_input.type = RKNN_TENSOR_UINT8;
            rknn_model_input.fmt = RKNN_TENSOR_NHWC;
            require_rknn_success(
                rknn_inputs_set(context_, 1, &rknn_model_input),
                "rknn_inputs_set " + model_path_);
        }
        require_rknn_success(
            rknn_run(context_, nullptr), "rknn_run " + model_path_);

        require_rknn_success(
            rknn_outputs_get(
                context_, static_cast<std::uint32_t>(outputs_.size()),
                outputs_.data(), nullptr),
            "rknn_outputs_get " + model_path_);
        output_in_use_ = true;
        RknnOutputLease output_lease(
            context_, outputs_, output_tensors_, output_in_use_);
        for (std::size_t index = 0; index < outputs_.size(); ++index) {
            const rknn_tensor_attr& attr = output_attrs_[index];
            const std::size_t expected_size =
                static_cast<std::size_t>(attr.n_elems) * sizeof(float);
            if (outputs_[index].buf != output_buffers_[index].data() ||
                outputs_[index].size < expected_size) {
                throw std::runtime_error("RKNN output buffer is incomplete");
            }
        }
        return output_lease;
    }

private:
    void initialize_output_buffers() {
        output_buffers_.resize(output_attrs_.size());
        outputs_.resize(output_attrs_.size());
        output_tensors_.reserve(output_attrs_.size());
        for (std::size_t index = 0; index < output_attrs_.size(); ++index) {
            const rknn_tensor_attr& attr = output_attrs_[index];
            if (attr.n_elems == 0) {
                throw std::runtime_error(
                    "RKNN output tensor has no elements: " + model_path_);
            }
            const std::size_t byte_size =
                static_cast<std::size_t>(attr.n_elems) * sizeof(float);
            if (byte_size > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error(
                    "RKNN output tensor is too large: " + model_path_);
            }

            std::vector<float>& buffer = output_buffers_[index];
            buffer.resize(attr.n_elems);
            rknn_output& output = outputs_[index];
            output.index = static_cast<std::uint32_t>(index);
            output.buf = buffer.data();
            output.size = static_cast<std::uint32_t>(byte_size);
            output.want_float = 1;
            output.is_prealloc = 1;
            output_tensors_.emplace_back(
                attr, buffer.data(), buffer.size());
        }
    }

    std::string model_path_;
    rknn_context context_ = 0;
    rknn_tensor_attr input_attr_{};
    std::unique_ptr<RknnInputBinding> input_binding_;
    std::vector<rknn_tensor_attr> output_attrs_;
    std::vector<std::vector<float>> output_buffers_;
    std::vector<rknn_output> outputs_;
    std::vector<RknnTensor> output_tensors_;
    std::string api_version_;
    std::string driver_version_;
    bool used_cpu_input_ = false;
    bool output_in_use_ = false;
    bool initialized_ = false;
};

RknnFaceDetector::RknnFaceDetector(
    std::string model_path,
    RknnCore core,
    float confidence_threshold)
    : model_(std::make_unique<RknnModel>(std::move(model_path), core)),
      input_spec_(model_->input_spec()),
      confidence_threshold_(confidence_threshold) {
    require_unit_interval(
        confidence_threshold_, "Detector confidence must be within [0, 1]");
    model_->require_output_shapes({{2, 56, 56}, {2, 56, 56}});
}

RknnFaceDetector::~RknnFaceDetector() = default;

const ModelInputSpec& RknnFaceDetector::input_spec() const noexcept {
    return input_spec_;
}

std::optional<FaceDetection> RknnFaceDetector::detect(
    const ModelInputView& input) {
    RknnOutputLease output_lease = model_->run(input);
    const std::vector<RknnTensor>& tensors = output_lease.tensors();
    if (tensors.size() != 2 ||
        tensors[0].height() != 56 || tensors[0].width() != 56 ||
        tensors[0].channels() != 2 ||
        tensors[1].height() != 56 || tensors[1].width() != 56 ||
        tensors[1].channels() != 2) {
        throw std::runtime_error(
            "Detector RKNN outputs must contain two 2x56x56 tensors");
    }
    // The converted OpenSeeFace model preserves the ONNX output order:
    // detector values first, per-channel max-pool values second.
    const RknnTensor& detection = tensors[0];
    const RknnTensor& max_pool = tensors[1];

    float best_confidence = -std::numeric_limits<float>::infinity();
    std::uint32_t best_x = 0;
    std::uint32_t best_y = 0;
    for (std::uint32_t y = 0; y < 56; ++y) {
        for (std::uint32_t x = 0; x < 56; ++x) {
            const float confidence = detection.value(0, y, x);
            if (confidence != max_pool.value(0, y, x)) {
                continue;
            }
            if (confidence > best_confidence) {
                best_confidence = confidence;
                best_x = x;
                best_y = y;
            }
        }
    }
    if (!std::isfinite(best_confidence) ||
        best_confidence < confidence_threshold_) {
        return std::nullopt;
    }

    const float radius = detection.value(1, best_y, best_x) * 112.0F;
    if (!std::isfinite(radius) || radius <= 0.0F) {
        return std::nullopt;
    }
    const CropRegion& source_crop = input.source_crop();
    const float scale_x = static_cast<float>(source_crop.width()) / 224.0F;
    const float scale_y = static_cast<float>(source_crop.height()) / 224.0F;
    const float center_x = static_cast<float>(best_x * 4U);
    const float center_y = static_cast<float>(best_y * 4U);
    return FaceDetection(
        FaceBox(
            static_cast<float>(source_crop.x()) + (center_x - radius) * scale_x,
            static_cast<float>(source_crop.y()) + (center_y - radius) * scale_y,
            2.0F * radius * scale_x,
            2.0F * radius * scale_y),
        best_confidence);
}

RknnLandmarkEstimator::RknnLandmarkEstimator(
    std::string model_path,
    RknnCore core)
    : model_(std::make_unique<RknnModel>(std::move(model_path), core)),
      input_spec_(model_->input_spec()) {
    model_->require_output_shapes({{198, 28, 28}});
}

RknnLandmarkEstimator::~RknnLandmarkEstimator() = default;

const ModelInputSpec& RknnLandmarkEstimator::input_spec() const noexcept {
    return input_spec_;
}

LandmarkEstimate RknnLandmarkEstimator::estimate(
    const ModelInputView& input) {
    RknnOutputLease output_lease = model_->run(input);
    const std::vector<RknnTensor>& tensors = output_lease.tensors();
    const RknnTensor* output = nullptr;
    for (const RknnTensor& tensor : tensors) {
        if (tensor.channels() == 198 && tensor.height() == 28 &&
            tensor.width() == 28) {
            output = &tensor;
        }
    }
    if (output == nullptr) {
        throw std::runtime_error(
            "Landmark RKNN output must contain a 198x28x28 tensor");
    }

    const CropRegion& crop = input.source_crop();
    const float scale_x = static_cast<float>(crop.width()) / 224.0F;
    const float scale_y = static_cast<float>(crop.height()) / 224.0F;
    std::vector<FaceLandmark> landmarks;
    landmarks.reserve(66);
    float confidence_sum = 0.0F;
    float minimum_x = std::numeric_limits<float>::infinity();
    float minimum_y = std::numeric_limits<float>::infinity();
    float maximum_x = -std::numeric_limits<float>::infinity();
    float maximum_y = -std::numeric_limits<float>::infinity();

    for (std::uint32_t point = 0; point < 66; ++point) {
        float best_confidence = -std::numeric_limits<float>::infinity();
        std::uint32_t best_x = 0;
        std::uint32_t best_y = 0;
        for (std::uint32_t y = 0; y < 28; ++y) {
            for (std::uint32_t x = 0; x < 28; ++x) {
                const float confidence = output->value(point, y, x);
                if (confidence > best_confidence) {
                    best_confidence = confidence;
                    best_x = x;
                    best_y = y;
                }
            }
        }

        const float row_offset =
            223.0F * clamped_logit(output->value(66U + point, best_y, best_x));
        const float column_offset =
            223.0F * clamped_logit(output->value(132U + point, best_y, best_x));
        const float source_y = static_cast<float>(crop.y()) + scale_y *
            (223.0F * static_cast<float>(best_y) / 27.0F + row_offset);
        const float source_x = static_cast<float>(crop.x()) + scale_x *
            (223.0F * static_cast<float>(best_x) / 27.0F + column_offset);
        const float point_confidence =
            std::max(0.0F, std::min(1.0F, best_confidence));
        landmarks.emplace_back(source_x, source_y, point_confidence);
        confidence_sum += point_confidence;
        minimum_x = std::min(minimum_x, source_x);
        minimum_y = std::min(minimum_y, source_y);
        maximum_x = std::max(maximum_x, source_x);
        maximum_y = std::max(maximum_y, source_y);
    }

    const float average_confidence = confidence_sum / 66.0F;
    FaceBox updated_box(
        minimum_x, minimum_y, maximum_x - minimum_x, maximum_y - minimum_y);
    return LandmarkEstimate(
        std::move(updated_box), std::move(landmarks), average_confidence);
}

}  // namespace vtb
