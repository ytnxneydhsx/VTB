#include "vtb/face/openseeface_geometry_estimator.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>

namespace vtb {
namespace {

constexpr std::size_t kLandmarkCount = 66;
constexpr std::size_t kTemplatePointCount = 70;
constexpr float kPi = 3.14159265358979323846F;
constexpr float kFeatureConfidenceThreshold = 0.4F;
constexpr float kPnpAnomalyThreshold = 300.0F;
constexpr int kPnpAnomalyResetCount = 5;
constexpr float kFailedPnpError = 99999.0F;
constexpr float kInvalidPnpError = 9999999.0F;

const std::array<cv::Point3f, kTemplatePointCount>& face_template() {
    static const std::array<cv::Point3f, kTemplatePointCount> points = {{
        {0.4551769692672F, 0.300895790030204F, -0.764429433974752F},
        {0.448998827123556F, 0.166995837790733F, -0.765143004071253F},
        {0.437431554952677F, 0.022655479179981F, -0.739267175112735F},
        {0.415033422928434F, -0.088941454648772F, -0.747947437846473F},
        {0.389123587370091F, -0.232380029794684F, -0.704788385327458F},
        {0.334630113904382F, -0.361265387599081F, -0.615587579236862F},
        {0.263725112132858F, -0.460009725616771F, -0.491479221041573F},
        {0.16241621322721F, -0.558037146073869F, -0.339445180872282F},
        {0.0F, -0.621079019321682F, -0.287294770748887F},
        {-0.16241621322721F, -0.558037146073869F, -0.339445180872282F},
        {-0.263725112132858F, -0.460009725616771F, -0.491479221041573F},
        {-0.334630113904382F, -0.361265387599081F, -0.615587579236862F},
        {-0.389123587370091F, -0.232380029794684F, -0.704788385327458F},
        {-0.415033422928434F, -0.088941454648772F, -0.747947437846473F},
        {-0.437431554952677F, 0.022655479179981F, -0.739267175112735F},
        {-0.448998827123556F, 0.166995837790733F, -0.765143004071253F},
        {-0.4551769692672F, 0.300895790030204F, -0.764429433974752F},
        {0.385529968662985F, 0.402800553948697F, -0.310031082540741F},
        {0.322196658344302F, 0.464439136821772F, -0.250558059367669F},
        {0.25409760441282F, 0.46420381416882F, -0.208177722146526F},
        {0.186875436782135F, 0.44706071961879F, -0.145299823706503F},
        {0.120880983543622F, 0.423566314072968F, -0.110757158774771F},
        {-0.120880983543622F, 0.423566314072968F, -0.110757158774771F},
        {-0.186875436782135F, 0.44706071961879F, -0.145299823706503F},
        {-0.25409760441282F, 0.46420381416882F, -0.208177722146526F},
        {-0.322196658344302F, 0.464439136821772F, -0.250558059367669F},
        {-0.385529968662985F, 0.402800553948697F, -0.310031082540741F},
        {0.0F, 0.293332603215811F, -0.137582088779393F},
        {0.0F, 0.194828701837823F, -0.069158109325951F},
        {0.0F, 0.103844017393155F, -0.009151819844964F},
        {0.0F, 0.0F, 0.0F},
        {0.080626352317973F, -0.041276068128093F, -0.134161035564826F},
        {0.046439347377934F, -0.057675223874769F, -0.102990627164664F},
        {0.0F, -0.068753126205604F, -0.090545348482397F},
        {-0.046439347377934F, -0.057675223874769F, -0.102990627164664F},
        {-0.080626352317973F, -0.041276068128093F, -0.134161035564826F},
        {0.315905195966084F, 0.298337502555443F, -0.285107407636464F},
        {0.275252345439353F, 0.312721904921771F, -0.244558251170671F},
        {0.176394511553111F, 0.311907184376107F, -0.219205360345231F},
        {0.131229723798772F, 0.284447361805627F, -0.234239149487417F},
        {0.184124948330084F, 0.260179585304867F, -0.226590776513707F},
        {0.279433549294448F, 0.267363071770222F, -0.248441437111633F},
        {-0.131229723798772F, 0.284447361805627F, -0.234239149487417F},
        {-0.176394511553111F, 0.311907184376107F, -0.219205360345231F},
        {-0.275252345439353F, 0.312721904921771F, -0.244558251170671F},
        {-0.315905195966084F, 0.298337502555443F, -0.285107407636464F},
        {-0.279433549294448F, 0.267363071770222F, -0.248441437111633F},
        {-0.184124948330084F, 0.260179585304867F, -0.226590776513707F},
        {0.121155252430729F, -0.208988660580347F, -0.160606287940521F},
        {0.041356305910044F, -0.194484199722098F, -0.096159882202821F},
        {0.0F, -0.205180167345702F, -0.083299217789729F},
        {-0.041356305910044F, -0.194484199722098F, -0.096159882202821F},
        {-0.121155252430729F, -0.208988660580347F, -0.160606287940521F},
        {-0.132325402795928F, -0.290857984604968F, -0.187067868218105F},
        {-0.064137791831655F, -0.325377847425684F, -0.158924039726607F},
        {0.0F, -0.343742581679188F, -0.113925986025684F},
        {0.064137791831655F, -0.325377847425684F, -0.158924039726607F},
        {0.132325402795928F, -0.290857984604968F, -0.187067868218105F},
        {0.181481567104525F, -0.243239316141725F, -0.231284988892766F},
        {0.083999507750469F, -0.239717753728704F, -0.155256465640701F},
        {0.0F, -0.256058040176369F, -0.0950619498899F},
        {-0.083999507750469F, -0.239717753728704F, -0.155256465640701F},
        {-0.181481567104525F, -0.243239316141725F, -0.231284988892766F},
        {-0.074036069749345F, -0.250689938345682F, -0.177346470406188F},
        {0.0F, -0.264945854681568F, -0.112349967428413F},
        {0.074036069749345F, -0.250689938345682F, -0.177346470406188F},
        {0.257990002632141F, 0.276080012321472F, -0.219998998939991F},
        {-0.257990002632141F, 0.276080012321472F, -0.219998998939991F},
        {0.257990002632141F, 0.276080012321472F, -0.324570998549461F},
        {-0.257990002632141F, 0.276080012321472F, -0.324570998549461F},
    }};
    return points;
}

const std::array<std::size_t, 14>& contour_indices() {
    static const std::array<std::size_t, 14> indices = {{
        0, 1, 8, 15, 16, 27, 28, 29, 30, 31, 32, 33, 34, 35,
    }};
    return indices;
}

bool finite_point(const cv::Point3f& point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y) &&
        std::isfinite(point.z);
}

float clamp_unit(float value) noexcept {
    return std::max(0.0F, std::min(1.0F, value));
}

float median_of(std::vector<float> values) {
    if (values.empty()) {
        throw std::logic_error("Remedian level cannot be empty");
    }
    std::sort(values.begin(), values.end());
    const std::size_t size = values.size();
    if (size < 3) {
        if (size == 1) {
            return values[0];
        }
        return (values[0] + values[1]) / 2.0F;
    }
    const std::size_t middle = size / 2;
    if ((size % 2U) == 0U) {
        return (values[middle] + values[middle - 1U]) / 2.0F;
    }
    return values[middle];
}

class Remedian final {
public:
    Remedian() = default;
    ~Remedian() = default;

    Remedian(const Remedian&) = delete;
    Remedian& operator=(const Remedian&) = delete;
    Remedian(Remedian&&) = delete;
    Remedian& operator=(Remedian&&) = delete;

    void add(float value) {
        values_.push_back(value);
        if (values_.size() != 64U) {
            return;
        }
        if (!next_) {
            next_ = std::make_unique<Remedian>();
        }
        next_->add(median_of(values_));
        values_.clear();
    }

    float median() const {
        if (next_) {
            return next_->median();
        }
        return median_of(values_);
    }

    void reset() noexcept {
        values_.clear();
        next_.reset();
    }

private:
    std::vector<float> values_;
    std::unique_ptr<Remedian> next_;
};

class Feature final {
public:
    explicit Feature(float threshold = 0.15F) : threshold_(threshold) {}
    ~Feature() = default;

    Feature(const Feature&) = delete;
    Feature& operator=(const Feature&) = delete;
    Feature(Feature&&) = delete;
    Feature& operator=(Feature&&) = delete;

    float update(float value) {
        const float normalized = update_state(value);
        const float filtered = last_ * 0.2F + normalized * 0.8F;
        last_ = filtered;
        return filtered;
    }

    void reset() noexcept {
        median_.reset();
        minimum_.reset();
        maximum_.reset();
        hard_minimum_.reset();
        hard_maximum_.reset();
        last_ = 0.0F;
    }

private:
    float update_state(float value) {
        median_.add(value);
        const float median = median_.median();

        if (!minimum_) {
            if (value < median &&
                (median - value) / median > threshold_) {
                minimum_ = value;
                hard_minimum_ =
                    value + 0.15F * (median - value);
                return -1.0F;
            }
            return 0.0F;
        }
        if (value < *minimum_) {
            minimum_ = value;
            hard_minimum_ = value + 0.15F * (median - value);
            return -1.0F;
        }

        if (!maximum_) {
            if (value > median &&
                (value - median) / median > threshold_) {
                maximum_ = value;
                hard_maximum_ =
                    value - 0.15F * (value - median);
                return 1.0F;
            }
            return 0.0F;
        }
        if (value > *maximum_) {
            maximum_ = value;
            hard_maximum_ = value - 0.15F * (value - median);
            return 1.0F;
        }

        if (*minimum_ < *hard_minimum_) {
            minimum_ = *hard_minimum_ * 0.001F + *minimum_ * 0.999F;
        }
        if (*maximum_ > *hard_maximum_) {
            maximum_ = *hard_maximum_ * 0.001F + *maximum_ * 0.999F;
        }

        if (value < median) {
            return -(1.0F -
                (value - *minimum_) / (median - *minimum_));
        }
        if (value > median) {
            return (value - median) / (*maximum_ - median);
        }
        return 0.0F;
    }

    Remedian median_;
    std::optional<float> minimum_;
    std::optional<float> maximum_;
    std::optional<float> hard_minimum_;
    std::optional<float> hard_maximum_;
    float threshold_ = 0.15F;
    float last_ = 0.0F;
};

float positive_angle(const cv::Point2f& first, const cv::Point2f& second) {
    float value = std::atan2(second.y - first.y, second.x - first.x);
    if (value < 0.0F) {
        value += 2.0F * kPi;
    }
    return value;
}

cv::Point2f rotate_like_openseeface(
    const cv::Point2f& origin,
    const cv::Point2f& point,
    float angle) {
    const float applied_angle = -angle;
    const float cosine = std::cos(applied_angle);
    const float sine = std::sin(applied_angle);
    return cv::Point2f(
        origin.x + cosine * (point.x - origin.x) -
            sine * (point.y - origin.y),
        origin.y + sine * (point.x - origin.x) +
            cosine * (point.y - origin.y));
}

std::array<cv::Point2f, 4> align_eye_points(
    const cv::Point2f& first_corner,
    const cv::Point2f& second_corner,
    const std::array<cv::Point2f, 4>& points) {
    float degrees = positive_angle(first_corner, second_corner) *
        180.0F / kPi;
    if (degrees >= 90.0F) {
        degrees = -(degrees - 180.0F);
    }
    if (degrees <= -90.0F) {
        degrees = -(degrees + 180.0F);
    }
    const float angle = degrees * kPi / 180.0F;
    std::array<cv::Point2f, 4> aligned{};
    for (std::size_t index = 0; index < aligned.size(); ++index) {
        aligned[index] = rotate_like_openseeface(
            first_corner, points[index], angle);
    }
    return aligned;
}

class FeatureExtractor final {
public:
    FeatureExtractor() : mouth_wide_(0.02F) {}
    ~FeatureExtractor() = default;

    FeatureExtractor(const FeatureExtractor&) = delete;
    FeatureExtractor& operator=(const FeatureExtractor&) = delete;
    FeatureExtractor(FeatureExtractor&&) = delete;
    FeatureExtractor& operator=(FeatureExtractor&&) = delete;

    FaceExpression update(
        const std::array<cv::Point3f, kLandmarkCount>& points) {
        const float normalization_x = 0.5F * (
            points[0].x - points[16].x +
            points[1].x - points[15].x);
        const float normalization_y = (
            points[27].y - points[28].y +
            points[28].y - points[29].y +
            points[29].y - points[30].y) / 3.0F;

        const std::array<cv::Point2f, 4> left_points = {{
            {points[43].x, points[43].y},
            {points[44].x, points[44].y},
            {points[47].x, points[47].y},
            {points[46].x, points[46].y},
        }};
        const std::array<cv::Point2f, 4> aligned_left = align_eye_points(
            {points[42].x, points[42].y},
            {points[45].x, points[45].y},
            left_points);
        const float left_eye_raw = std::abs(
            0.5F * (aligned_left[0].y + aligned_left[1].y) -
            0.5F * (aligned_left[2].y + aligned_left[3].y)) /
            normalization_y;

        const std::array<cv::Point2f, 4> right_points = {{
            {points[37].x, points[37].y},
            {points[38].x, points[38].y},
            {points[41].x, points[41].y},
            {points[40].x, points[40].y},
        }};
        const std::array<cv::Point2f, 4> aligned_right = align_eye_points(
            {points[36].x, points[36].y},
            {points[39].x, points[39].y},
            right_points);
        const float right_eye_raw = std::abs(
            0.5F * (aligned_right[0].y + aligned_right[1].y) -
            0.5F * (aligned_right[2].y + aligned_right[3].y)) /
            normalization_y;

        const float upper_inner_lip =
            (points[59].y + points[60].y + points[61].y) / 3.0F;
        const float lower_inner_lip =
            (points[63].y + points[64].y + points[65].y) / 3.0F;
        const float mouth_open_raw =
            std::abs(upper_inner_lip - lower_inner_lip) /
            normalization_y;
        const float mouth_wide_raw =
            std::abs(points[58].x - points[62].x) / normalization_x;

        const float left_eye = eye_left_.update(left_eye_raw);
        const float right_eye = eye_right_.update(right_eye_raw);
        const float mouth_open = mouth_open_.update(mouth_open_raw);
        const float mouth_wide = mouth_wide_.update(mouth_wide_raw);
        if (!std::isfinite(left_eye) || !std::isfinite(right_eye) ||
            !std::isfinite(mouth_open) || !std::isfinite(mouth_wide)) {
            throw std::runtime_error(
                "OpenSeeFace feature extraction produced non-finite values");
        }

        const float left_eye_open = 1.0F - clamp_unit(-left_eye);
        const float right_eye_open = 1.0F - clamp_unit(-right_eye);
        return FaceExpression(
            left_eye_open, right_eye_open, mouth_open, mouth_wide);
    }

    void reset() noexcept {
        eye_left_.reset();
        eye_right_.reset();
        mouth_open_.reset();
        mouth_wide_.reset();
    }

private:
    Feature eye_left_;
    Feature eye_right_;
    Feature mouth_open_;
    Feature mouth_wide_;
};

std::array<cv::Point3f, kLandmarkCount> normalize_points(
    std::array<cv::Point3f, kLandmarkCount> points) {
    const float center_x = points[30].x;
    const float center_y = points[30].y;
    for (cv::Point3f& point : points) {
        point.x -= center_x;
        point.y -= center_y;
    }

    const cv::Point2f origin(points[30].x, points[30].y);
    const cv::Point2f nose_bridge(points[27].x, points[27].y);
    const float angle = positive_angle(origin, nose_bridge) - kPi / 2.0F;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    for (cv::Point3f& point : points) {
        const float x = point.x - points[30].x;
        const float y = point.y - points[30].y;
        point.x = x * cosine + y * sine + points[30].x;
        point.y = -x * sine + y * cosine + points[30].y;
    }

    const auto& model = face_template();
    const float vertical_scale = (
        (points[27].y - points[28].y) /
            (model[27].y - model[28].y) +
        (points[28].y - points[29].y) /
            (model[28].y - model[29].y) +
        (points[29].y - points[30].y) /
            (model[29].y - model[30].y)) / 3.0F;
    const float horizontal_scale = (
        std::abs(points[0].x - points[16].x) /
            std::abs(model[0].x - model[16].x) +
        std::abs(points[36].x - points[39].x) /
            std::abs(model[36].x - model[39].x) +
        std::abs(points[42].x - points[45].x) /
            std::abs(model[42].x - model[45].x)) / 3.0F;
    if (!std::isfinite(vertical_scale) ||
        !std::isfinite(horizontal_scale) || vertical_scale == 0.0F ||
        horizontal_scale == 0.0F) {
        throw std::runtime_error(
            "OpenSeeFace 3D normalization scale is invalid");
    }
    for (cv::Point3f& point : points) {
        point.y /= vertical_scale;
        point.x /= horizontal_scale;
        if (!finite_point(point)) {
            throw std::runtime_error(
                "OpenSeeFace 3D normalization produced non-finite values");
        }
    }
    return points;
}

FaceExpression default_expression() {
    return FaceExpression(1.0F, 1.0F, 0.0F, 0.0F);
}

}  // namespace

class OpenSeeFaceGeometryState final {
public:
    OpenSeeFaceGeometryState() = default;
    ~OpenSeeFaceGeometryState() = default;

    OpenSeeFaceGeometryState(const OpenSeeFaceGeometryState&) = delete;
    OpenSeeFaceGeometryState& operator=(
        const OpenSeeFaceGeometryState&) = delete;
    OpenSeeFaceGeometryState(OpenSeeFaceGeometryState&&) = delete;
    OpenSeeFaceGeometryState& operator=(
        OpenSeeFaceGeometryState&&) = delete;

    FaceGeometryResult estimate(
        const FrameView& frame,
        const std::vector<FaceLandmark>& landmarks) {
        if (landmarks.size() != kLandmarkCount) {
            throw std::invalid_argument(
                "OpenSeeFace geometry requires 66 landmarks");
        }
        if (frame.width() == 0 || frame.height() == 0) {
            throw std::invalid_argument(
                "OpenSeeFace geometry requires non-empty frame dimensions");
        }

        const float frame_width = static_cast<float>(frame.width());
        const float frame_height = static_cast<float>(frame.height());
        const cv::Matx33f camera(
            frame_width, 0.0F, frame_width / 2.0F,
            0.0F, frame_width, frame_height / 2.0F,
            0.0F, 0.0F, 1.0F);
        const cv::Matx33f inverse_camera = camera.inv();
        const cv::Mat distortion = cv::Mat::zeros(4, 1, CV_32F);

        std::vector<cv::Point3f> object_points;
        std::vector<cv::Point2f> image_points;
        object_points.reserve(contour_indices().size());
        image_points.reserve(contour_indices().size());
        for (std::size_t index : contour_indices()) {
            object_points.push_back(face_template()[index]);
            image_points.emplace_back(
                landmarks[index].y(), landmarks[index].x());
        }

        if (!has_extrinsic_) {
            rotation_vector_ = cv::Mat::zeros(3, 1, CV_32F);
            translation_vector_ = cv::Mat::zeros(3, 1, CV_32F);
        }
        const bool solved = cv::solvePnP(
            object_points,
            image_points,
            cv::Mat(camera),
            distortion,
            rotation_vector_,
            translation_vector_,
            true,
            cv::SOLVEPNP_ITERATIVE);
        if (!solved) {
            rotation_vector_ = cv::Mat::zeros(3, 1, CV_32F);
            translation_vector_ = cv::Mat::zeros(3, 1, CV_32F);
            has_extrinsic_ = true;
            return FaceGeometryResult(
                false,
                FacePose(0.0F, 0.0F, 0.0F, kFailedPnpError),
                expression_);
        }
        rotation_vector_.convertTo(rotation_vector_, CV_32F);
        translation_vector_.convertTo(translation_vector_, CV_32F);
        has_extrinsic_ = true;

        cv::Mat rotation_matrix_value;
        cv::Rodrigues(rotation_vector_, rotation_matrix_value);
        rotation_matrix_value.convertTo(rotation_matrix_value, CV_32F);
        const cv::Matx33f rotation(
            rotation_matrix_value.at<float>(0, 0),
            rotation_matrix_value.at<float>(0, 1),
            rotation_matrix_value.at<float>(0, 2),
            rotation_matrix_value.at<float>(1, 0),
            rotation_matrix_value.at<float>(1, 1),
            rotation_matrix_value.at<float>(1, 2),
            rotation_matrix_value.at<float>(2, 0),
            rotation_matrix_value.at<float>(2, 1),
            rotation_matrix_value.at<float>(2, 2));
        const cv::Matx33f inverse_rotation = rotation.inv();
        const cv::Vec3f translation(
            translation_vector_.at<float>(0, 0),
            translation_vector_.at<float>(1, 0),
            translation_vector_.at<float>(2, 0));

        std::array<cv::Point2f, kTemplatePointCount> projected{};
        std::array<float, kTemplatePointCount> depths{};
        for (std::size_t index = 0; index < face_template().size(); ++index) {
            const cv::Point3f& model_point = face_template()[index];
            const cv::Vec3f model_vector(
                model_point.x, model_point.y, model_point.z);
            const cv::Vec3f camera_point = rotation * model_vector + translation;
            float depth = camera_point[2];
            if (depth == 0.0F) {
                depth = 0.000001F;
            }
            depths[index] = depth;
            const cv::Vec3f homogeneous = camera * camera_point;
            projected[index] = cv::Point2f(
                homogeneous[0] / depth,
                homogeneous[1] / depth);
        }

        std::array<cv::Point3f, kLandmarkCount> points_3d{};
        for (std::size_t index = 0; index < points_3d.size(); ++index) {
            const cv::Vec3f image_point(
                landmarks[index].y() * depths[index],
                landmarks[index].x() * depths[index],
                depths[index]);
            const cv::Vec3f camera_point = inverse_camera * image_point;
            const cv::Vec3f model_point =
                inverse_rotation * (camera_point - translation);
            points_3d[index] = cv::Point3f(
                model_point[0], model_point[1], model_point[2]);
        }

        double squared_error = 0.0;
        for (std::size_t index = 0; index <= 16U; ++index) {
            const double delta_x = static_cast<double>(landmarks[index].y()) -
                static_cast<double>(projected[index].x);
            const double delta_y = static_cast<double>(landmarks[index].x()) -
                static_cast<double>(projected[index].y);
            squared_error += delta_x * delta_x + delta_y * delta_y;
        }
        const double nose_delta_x =
            static_cast<double>(landmarks[30].y()) -
            static_cast<double>(projected[30].x);
        const double nose_delta_y =
            static_cast<double>(landmarks[30].x()) -
            static_cast<double>(projected[30].y);
        squared_error += nose_delta_x * nose_delta_x +
            nose_delta_y * nose_delta_y;
        float pnp_error = static_cast<float>(std::sqrt(
            squared_error /
            (2.0 * static_cast<double>(contour_indices().size()))));
        if (!std::isfinite(pnp_error)) {
            pnp_error = kInvalidPnpError;
        }

        if (pnp_error > kPnpAnomalyThreshold) {
            ++pnp_anomaly_count_;
            if (pnp_anomaly_count_ > kPnpAnomalyResetCount) {
                rotation_vector_.release();
                translation_vector_ = cv::Mat::zeros(3, 1, CV_32F);
                has_extrinsic_ = false;
            }
        } else {
            pnp_anomaly_count_ = 0;
        }

        cv::Mat upper_triangular;
        cv::Mat orthogonal;
        const cv::Vec3d euler = cv::RQDecomp3x3(
            rotation_matrix_value, upper_triangular, orthogonal);
        const FacePose pose(
            static_cast<float>(euler[1]),
            static_cast<float>(euler[0]),
            static_cast<float>(euler[2]),
            pnp_error);

        float confidence_sum = 0.0F;
        for (const FaceLandmark& landmark : landmarks) {
            confidence_sum += landmark.confidence();
        }
        const float confidence =
            confidence_sum / static_cast<float>(landmarks.size());
        if (confidence < kFeatureConfidenceThreshold ||
            pnp_error > kPnpAnomalyThreshold) {
            return FaceGeometryResult(true, pose, expression_);
        }

        try {
            points_3d = normalize_points(std::move(points_3d));
            expression_ = features_.update(points_3d);
        } catch (const std::runtime_error&) {
            return FaceGeometryResult(false, pose, expression_);
        }
        return FaceGeometryResult(true, pose, expression_);
    }

    void reset() noexcept {
        rotation_vector_.release();
        translation_vector_.release();
        has_extrinsic_ = false;
        pnp_anomaly_count_ = 0;
        features_.reset();
        expression_ = default_expression();
    }

private:
    cv::Mat rotation_vector_;
    cv::Mat translation_vector_;
    bool has_extrinsic_ = false;
    int pnp_anomaly_count_ = 0;
    FeatureExtractor features_;
    FaceExpression expression_ = default_expression();
};

OpenSeeFaceGeometryEstimator::OpenSeeFaceGeometryEstimator()
    : state_(std::make_unique<OpenSeeFaceGeometryState>()) {}

OpenSeeFaceGeometryEstimator::~OpenSeeFaceGeometryEstimator() = default;

FaceGeometryResult OpenSeeFaceGeometryEstimator::estimate(
    const FrameView& frame,
    const FaceBox& box,
    const std::vector<FaceLandmark>& landmarks) {
    static_cast<void>(box);
    return state_->estimate(frame, landmarks);
}

void OpenSeeFaceGeometryEstimator::reset() noexcept {
    state_->reset();
}

}  // namespace vtb
