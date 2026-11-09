#pragma once

#include <cstdint>

#include "vtb/live2d/live2d_parameters.hpp"

namespace vtb {

/**
 * @brief Resamples timestamped Live2D parameters at the render frame rate.
 *
 * The interpolator buffers the two newest tracking samples. Interpolation is
 * enabled only when their interval is meaningfully longer than the render
 * interval, so an existing 30 Hz tracking / 30 FPS render path keeps its
 * direct latest-value behavior while a 30 Hz tracking / 60 FPS render path
 * receives intermediate parameter values.
 */
class Live2DParameterInterpolator final {
public:
    Live2DParameterInterpolator() = default;
    ~Live2DParameterInterpolator() = default;

    Live2DParameterInterpolator(const Live2DParameterInterpolator&) = default;
    Live2DParameterInterpolator& operator=(
        const Live2DParameterInterpolator&) = default;
    Live2DParameterInterpolator(Live2DParameterInterpolator&&) noexcept = default;
    Live2DParameterInterpolator& operator=(
        Live2DParameterInterpolator&&) noexcept = default;

    /** @brief Removes all buffered tracking samples. */
    void reset() noexcept;

    /**
     * @brief Adds a newer tracking sample.
     * @return true when the sample was accepted; false for zero, duplicate or
     * out-of-order timestamps.
     */
    bool push(
        const Live2DParameters& parameters,
        std::uint64_t produced_timestamp_ns) noexcept;

    bool has_parameters() const noexcept;

    /**
     * @brief Samples parameters for one render frame.
     *
     * When resampling is active, the target time is delayed by one source
     * sample interval. This makes both bracketing samples available and avoids
     * extrapolating face motion.
     */
    Live2DParameters sample(
        std::uint64_t render_timestamp_ns,
        std::uint64_t render_interval_ns) const;

private:
    Live2DParameters previous_parameters_;
    Live2DParameters current_parameters_;
    std::uint64_t previous_timestamp_ns_ = 0;
    std::uint64_t current_timestamp_ns_ = 0;
    bool has_previous_ = false;
    bool has_current_ = false;
};

}  // namespace vtb
