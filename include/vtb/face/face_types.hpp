#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vtb {

/** @brief FacePipeline 的模型、预处理资源、检测策略和关键点 ROI 配置。 */
class FacePipelineConfig {
public:
    std::string detector_model;
    std::string landmark_model;
    std::string rga_dma_heap;
    std::size_t redetect_interval_frames = 0;
    float minimum_tracking_confidence = 0.6F;
    float landmark_roi_horizontal_expansion = 0.10F;
    float landmark_roi_vertical_expansion = 0.125F;
};

/** @brief 源视频帧像素坐标中的人脸矩形。 */
class FaceBox {
public:
    FaceBox(float x, float y, float width, float height);
    ~FaceBox() = default;

    FaceBox(const FaceBox&) = default;
    FaceBox& operator=(const FaceBox&) = default;
    FaceBox(FaceBox&&) noexcept = default;
    FaceBox& operator=(FaceBox&&) noexcept = default;

    float x() const noexcept;
    float y() const noexcept;
    float width() const noexcept;
    float height() const noexcept;

private:
    float x_ = 0.0F;
    float y_ = 0.0F;
    float width_ = 0.0F;
    float height_ = 0.0F;
};

/** @brief 一次人脸检测产生的矩形和置信度。 */
class FaceDetection {
public:
    FaceDetection(FaceBox box, float confidence);
    ~FaceDetection() = default;

    FaceDetection(const FaceDetection&) = default;
    FaceDetection& operator=(const FaceDetection&) = default;
    FaceDetection(FaceDetection&&) noexcept = default;
    FaceDetection& operator=(FaceDetection&&) noexcept = default;

    const FaceBox& box() const noexcept;
    float confidence() const noexcept;

private:
    FaceBox box_;
    float confidence_ = 0.0F;
};

/** @brief 源视频帧像素坐标中的单个人脸关键点。 */
class FaceLandmark {
public:
    FaceLandmark(float x, float y, float confidence);
    ~FaceLandmark() = default;

    FaceLandmark(const FaceLandmark&) = default;
    FaceLandmark& operator=(const FaceLandmark&) = default;
    FaceLandmark(FaceLandmark&&) noexcept = default;
    FaceLandmark& operator=(FaceLandmark&&) noexcept = default;

    float x() const noexcept;
    float y() const noexcept;
    float confidence() const noexcept;

private:
    float x_ = 0.0F;
    float y_ = 0.0F;
    float confidence_ = 0.0F;
};

/** @brief 关键点模型输出的更新后人脸框、66 点关键点和置信度。 */
class LandmarkEstimate {
public:
    LandmarkEstimate(
        FaceBox updated_box,
        std::vector<FaceLandmark> landmarks,
        float confidence);
    ~LandmarkEstimate() = default;

    LandmarkEstimate(const LandmarkEstimate&) = default;
    LandmarkEstimate& operator=(const LandmarkEstimate&) = default;
    LandmarkEstimate(LandmarkEstimate&&) noexcept = default;
    LandmarkEstimate& operator=(LandmarkEstimate&&) noexcept = default;

    const FaceBox& updated_box() const noexcept;
    const std::vector<FaceLandmark>& landmarks() const noexcept;
    float confidence() const noexcept;

private:
    FaceBox updated_box_;
    std::vector<FaceLandmark> landmarks_;
    float confidence_ = 0.0F;
};

/** @brief 由人脸几何计算得到的头部姿态。 */
class FacePose {
public:
    FacePose(float yaw, float pitch, float roll, float pnp_error);
    ~FacePose() = default;

    FacePose(const FacePose&) = default;
    FacePose& operator=(const FacePose&) = default;
    FacePose(FacePose&&) noexcept = default;
    FacePose& operator=(FacePose&&) noexcept = default;

    float yaw() const noexcept;
    float pitch() const noexcept;
    float roll() const noexcept;
    float pnp_error() const noexcept;

private:
    float yaw_ = 0.0F;
    float pitch_ = 0.0F;
    float roll_ = 0.0F;
    float pnp_error_ = 0.0F;
};

/** @brief 由人脸关键点计算得到的眼睛和嘴部表情参数。 */
class FaceExpression {
public:
    FaceExpression(
        float left_eye_open,
        float right_eye_open,
        float mouth_open,
        float mouth_wide);
    ~FaceExpression() = default;

    FaceExpression(const FaceExpression&) = default;
    FaceExpression& operator=(const FaceExpression&) = default;
    FaceExpression(FaceExpression&&) noexcept = default;
    FaceExpression& operator=(FaceExpression&&) noexcept = default;

    float left_eye_open() const noexcept;
    float right_eye_open() const noexcept;
    float mouth_open() const noexcept;
    float mouth_wide() const noexcept;

private:
    float left_eye_open_ = 1.0F;
    float right_eye_open_ = 1.0F;
    float mouth_open_ = 0.0F;
    float mouth_wide_ = 0.0F;
};

/** @brief 一次姿态和表情几何计算的结果。 */
class FaceGeometryResult {
public:
    FaceGeometryResult(
        bool success,
        FacePose pose,
        FaceExpression expression);
    ~FaceGeometryResult() = default;

    FaceGeometryResult(const FaceGeometryResult&) = default;
    FaceGeometryResult& operator=(const FaceGeometryResult&) = default;
    FaceGeometryResult(FaceGeometryResult&&) noexcept = default;
    FaceGeometryResult& operator=(FaceGeometryResult&&) noexcept = default;

    bool success() const noexcept;
    const FacePose& pose() const noexcept;
    const FaceExpression& expression() const noexcept;

private:
    bool success_ = false;
    FacePose pose_;
    FaceExpression expression_;
};

/**
 * @brief FacePipeline 对一帧视频生成的完整、可独立保存的人脸跟踪结果值。
 *
 * 结果按值拥有帧编号、采集时间戳、人脸框、66 点、姿态、表情、几何成功标志和
 * 关键点平均置信度，不借用源 FrameView 或模型输出缓冲区。复制使用值语义；移动
 * 构造和移动赋值明确为 noexcept default。
 */
class FaceTrackingResult {
public:
    FaceTrackingResult(
        std::uint64_t frame_id,
        std::uint64_t capture_timestamp_ns,
        FaceBox box,
        std::vector<FaceLandmark> landmarks,
        FacePose pose,
        FaceExpression expression,
        bool geometry_success,
        float confidence);
    ~FaceTrackingResult() = default;

    FaceTrackingResult(const FaceTrackingResult&) = default;
    FaceTrackingResult& operator=(const FaceTrackingResult&) = default;
    FaceTrackingResult(FaceTrackingResult&&) noexcept = default;
    FaceTrackingResult& operator=(FaceTrackingResult&&) noexcept = default;

    std::uint64_t frame_id() const noexcept;
    std::uint64_t capture_timestamp_ns() const noexcept;
    const FaceBox& box() const noexcept;
    const std::vector<FaceLandmark>& landmarks() const noexcept;
    const FacePose& pose() const noexcept;
    const FaceExpression& expression() const noexcept;
    bool geometry_success() const noexcept;
    float confidence() const noexcept;

private:
    std::uint64_t frame_id_ = 0;
    std::uint64_t capture_timestamp_ns_ = 0;
    FaceBox box_;
    std::vector<FaceLandmark> landmarks_;
    FacePose pose_;
    FaceExpression expression_;
    bool geometry_success_ = false;
    float confidence_ = 0.0F;
};

}  // namespace vtb
