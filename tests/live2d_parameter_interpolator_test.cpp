#include <cmath>
#include <cstdint>
#include <stdexcept>

#include "vtb/live2d/live2d_parameter_interpolator.hpp"

namespace {

constexpr std::uint64_t kRender60FpsIntervalNs = 16666666ULL;
constexpr std::uint64_t kRender30FpsIntervalNs = 33333333ULL;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_near(float actual, float expected, const char* message) {
    if (std::fabs(actual - expected) > 0.001F) {
        throw std::runtime_error(message);
    }
}

vtb::Live2DParameters parameters(float value) {
    return vtb::Live2DParameters(
        value,
        value,
        value,
        value,
        value,
        value,
        value,
        value,
        value,
        value);
}

void test_first_sample_is_immediate() {
    vtb::Live2DParameterInterpolator interpolator;
    require(
        interpolator.push(parameters(12.0F), 1000000000ULL),
        "first sample was rejected");
    require_near(
        interpolator.sample(1000000000ULL, kRender60FpsIntervalNs).angle_x(),
        12.0F,
        "first sample was delayed");
}

void test_30_hz_samples_are_interpolated_for_60_fps_rendering() {
    vtb::Live2DParameterInterpolator interpolator;
    const std::uint64_t first_timestamp_ns = 1000000000ULL;
    const std::uint64_t second_timestamp_ns =
        first_timestamp_ns + kRender30FpsIntervalNs;
    require(
        interpolator.push(parameters(0.0F), first_timestamp_ns),
        "first sample was rejected");
    require(
        interpolator.push(parameters(30.0F), second_timestamp_ns),
        "second sample was rejected");

    const std::uint64_t midpoint_render_timestamp_ns =
        second_timestamp_ns + kRender60FpsIntervalNs;
    require_near(
        interpolator.sample(
            midpoint_render_timestamp_ns,
            kRender60FpsIntervalNs).angle_x(),
        15.0F,
        "60 FPS midpoint was not interpolated");
    require_near(
        interpolator.sample(
            second_timestamp_ns + kRender30FpsIntervalNs,
            kRender60FpsIntervalNs).angle_x(),
        30.0F,
        "60 FPS interpolation did not reach the latest sample");
}

void test_30_hz_rendering_keeps_latest_value() {
    vtb::Live2DParameterInterpolator interpolator;
    require(
        interpolator.push(parameters(0.0F), 1000000000ULL),
        "first sample was rejected");
    require(
        interpolator.push(parameters(30.0F), 1033333333ULL),
        "second sample was rejected");
    require_near(
        interpolator.sample(1033333333ULL, kRender30FpsIntervalNs).angle_x(),
        30.0F,
        "30 FPS rendering unexpectedly delayed the latest value");
}

void test_duplicate_and_out_of_order_samples_are_ignored() {
    vtb::Live2DParameterInterpolator interpolator;
    require(
        interpolator.push(parameters(5.0F), 2000000000ULL),
        "first sample was rejected");
    require(
        !interpolator.push(parameters(9.0F), 2000000000ULL),
        "duplicate timestamp was accepted");
    require(
        !interpolator.push(parameters(11.0F), 1999999999ULL),
        "out-of-order timestamp was accepted");
    require_near(
        interpolator.sample(2100000000ULL, kRender60FpsIntervalNs).angle_x(),
        5.0F,
        "ignored sample changed the current value");
}

void test_reset_discards_old_interpolation_pair() {
    vtb::Live2DParameterInterpolator interpolator;
    require(
        interpolator.push(parameters(0.0F), 1000000000ULL),
        "first sample was rejected");
    require(
        interpolator.push(parameters(30.0F), 1033333333ULL),
        "second sample was rejected");
    interpolator.reset();
    require(
        interpolator.push(parameters(8.0F), 2000000000ULL),
        "post-reset sample was rejected");
    require_near(
        interpolator.sample(2000000000ULL, kRender60FpsIntervalNs).angle_x(),
        8.0F,
        "post-reset sample was blended with stale state");
}

}  // namespace

int main() {
    test_first_sample_is_immediate();
    test_30_hz_samples_are_interpolated_for_60_fps_rendering();
    test_30_hz_rendering_keeps_latest_value();
    test_duplicate_and_out_of_order_samples_are_ignored();
    test_reset_discards_old_interpolation_pair();
    return 0;
}
