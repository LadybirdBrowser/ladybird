/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/StdLibExtras.h>
#include <LibCompositing/Scrolling/SmoothScrollAnimation.h>

namespace Compositing {

static constexpr double scroll_speed_in_pixels_per_second = 1000.0;
static constexpr double maximum_scroll_duration_in_seconds = 0.2;

// A wheel step takes longer the shorter its distance, within these bounds.
static constexpr double minimum_wheel_scroll_duration_in_seconds = 0.1;
static constexpr double maximum_wheel_scroll_duration_in_seconds = 0.2;
static constexpr double wheel_scroll_distance_covered_at_maximum_duration = 120.0;
static constexpr double wheel_scroll_duration_reduction_per_pixel = 1.0 / 3600.0;

// The ease-in-out curve WebKit uses for programmatic smooth scrolling, cubic-bezier(0.42, 0, 0.58, 1). A retargeted
// scroll changes y1 so that it keeps the speed it had.
static constexpr double easing_control_point_x1 = 0.42;
static constexpr double easing_control_point_x2 = 0.58;
static constexpr double easing_control_point_y2 = 1.0;

static constexpr double maximum_easing_slope = 1000.0;
static constexpr double retarget_ease_out_allowance = 2.5;

// https://drafts.csswg.org/css-easing/#cubic-bezier-algo
static double cubic_bezier(double t, double p1, double p2)
{
    return 3 * (1 - t) * (1 - t) * t * p1 + 3 * (1 - t) * t * t * p2 + t * t * t;
}

static double cubic_bezier_derivative(double t, double p1, double p2)
{
    return 3 * (1 - t) * (1 - t) * p1 + 6 * (1 - t) * t * (p2 - p1) + 3 * t * t * (1 - p2);
}

static double curve_parameter_for_progress(double progress)
{
    // Find the curve parameter whose horizontal position is the progress: a few Newton steps usually land on it, and
    // bisection finishes the job when the slope gets too flat for them.
    auto t = progress;
    for (int i = 0; i < 8; ++i) {
        auto x = cubic_bezier(t, easing_control_point_x1, easing_control_point_x2) - progress;
        if (AK::abs(x) < 1e-7)
            return t;
        auto slope = cubic_bezier_derivative(t, easing_control_point_x1, easing_control_point_x2);
        if (AK::abs(slope) < 1e-6)
            break;
        t -= x / slope;
    }
    double low = 0;
    double high = 1;
    while (high - low > 1e-7) {
        t = (low + high) / 2;
        if (cubic_bezier(t, easing_control_point_x1, easing_control_point_x2) < progress)
            low = t;
        else
            high = t;
    }
    return t;
}

static double momentum_frames_for_distance(double distance)
{
    auto frames = AK::ceil(-AK::log(1 - distance * (1 - 1 / momentum_distance_share_per_frame)) / AK::log(momentum_distance_share_per_frame));
    return min(frames, maximum_momentum_duration_in_seconds / momentum_frame_duration_in_seconds);
}

SmoothScrollAnimation::SmoothScrollAnimation(Gfx::FloatPoint start_offset, Gfx::FloatPoint destination_offset, double pixels_per_css_pixel, ScrollAnimationKind kind)
    : m_start_offset(start_offset)
    , m_destination_offset(destination_offset)
    , m_pixels_per_css_pixel(pixels_per_css_pixel)
    , m_kind(kind)
{
    VERIFY(pixels_per_css_pixel > 0);
    m_duration = duration_for_distance(destination_offset - start_offset);
}

AK::Duration SmoothScrollAnimation::duration_for_distance(Gfx::FloatPoint distance) const
{
    auto horizontal_distance = static_cast<double>(distance.x()) / m_pixels_per_css_pixel;
    auto vertical_distance = static_cast<double>(distance.y()) / m_pixels_per_css_pixel;
    auto length = AK::sqrt(horizontal_distance * horizontal_distance + vertical_distance * vertical_distance);
    if (length == 0)
        return AK::Duration::zero();

    double duration_in_seconds = 0;
    switch (m_kind) {
    case ScrollAnimationKind::SmoothScroll:
        duration_in_seconds = min(length / scroll_speed_in_pixels_per_second, maximum_scroll_duration_in_seconds);
        break;
    case ScrollAnimationKind::Wheel:
        duration_in_seconds = clamp(
            maximum_wheel_scroll_duration_in_seconds - (length - wheel_scroll_distance_covered_at_maximum_duration) * wheel_scroll_duration_reduction_per_pixel,
            minimum_wheel_scroll_duration_in_seconds, maximum_wheel_scroll_duration_in_seconds);
        break;
    case ScrollAnimationKind::Momentum:
        duration_in_seconds = momentum_frames_for_distance(length) * momentum_frame_duration_in_seconds;
        break;
    }
    return AK::Duration::from_seconds_f64(duration_in_seconds);
}

double SmoothScrollAnimation::eased_progress_at(double progress) const
{
    if (m_kind == ScrollAnimationKind::Momentum) {
        auto frames = m_duration.to_seconds_f64() / momentum_frame_duration_in_seconds;
        auto frames_elapsed = progress * frames;
        return (1 - AK::pow(momentum_distance_share_per_frame, frames_elapsed)) / (1 - AK::pow(momentum_distance_share_per_frame, frames));
    }

    // https://drafts.csswg.org/cssom-view/#smooth-scroll
    // A smooth scroll follows a user-agent-defined timing function.
    return cubic_bezier(curve_parameter_for_progress(progress), m_easing_control_point_y1, easing_control_point_y2);
}

double SmoothScrollAnimation::easing_slope_at(double progress) const
{
    if (m_kind == ScrollAnimationKind::Momentum) {
        auto frames = m_duration.to_seconds_f64() / momentum_frame_duration_in_seconds;
        auto frames_elapsed = progress * frames;
        auto decay = AK::log(momentum_distance_share_per_frame);
        return -AK::pow(momentum_distance_share_per_frame, frames_elapsed) * decay * frames / (1 - AK::pow(momentum_distance_share_per_frame, frames));
    }

    auto t = curve_parameter_for_progress(progress);
    auto horizontal_slope = cubic_bezier_derivative(t, easing_control_point_x1, easing_control_point_x2);
    if (horizontal_slope == 0)
        return 0;
    return cubic_bezier_derivative(t, m_easing_control_point_y1, easing_control_point_y2) / horizontal_slope;
}

Gfx::FloatPoint SmoothScrollAnimation::velocity_at(AK::Duration elapsed) const
{
    if (m_duration.is_zero())
        return {};

    auto duration_in_seconds = m_duration.to_seconds_f64();
    auto progress = clamp((elapsed - m_start_time).to_seconds_f64() / duration_in_seconds, 0.0, 1.0);
    auto speed_per_pixel = easing_slope_at(progress) / duration_in_seconds;
    auto distance = m_destination_offset - m_start_offset;
    return { static_cast<float>(distance.x() * speed_per_pixel), static_cast<float>(distance.y() * speed_per_pixel) };
}

SmoothScrollAnimation::Sample SmoothScrollAnimation::sample(AK::Duration elapsed) const
{
    auto elapsed_since_start = elapsed - m_start_time;
    if (m_duration.is_zero() || elapsed_since_start >= m_duration)
        return { m_destination_offset, true };

    auto progress = clamp(elapsed_since_start.to_seconds_f64() / m_duration.to_seconds_f64(), 0.0, 1.0);
    auto eased_progress = eased_progress_at(progress);

    return {
        {
            static_cast<float>(m_start_offset.x() + (m_destination_offset.x() - m_start_offset.x()) * eased_progress),
            static_cast<float>(m_start_offset.y() + (m_destination_offset.y() - m_start_offset.y()) * eased_progress),
        },
        false,
    };
}

void SmoothScrollAnimation::retarget(Gfx::FloatPoint destination_offset, AK::Duration elapsed)
{
    if (destination_offset == m_destination_offset)
        return;

    auto current_offset = sample(elapsed).offset;
    auto remaining_distance = destination_offset - current_offset;
    auto remaining_length = AK::hypot(static_cast<double>(remaining_distance.x()), static_cast<double>(remaining_distance.y()));

    // Only the speed along the new path carries over.
    double velocity = 0;
    if (remaining_length != 0) {
        auto velocity_vector = velocity_at(elapsed);
        velocity = (static_cast<double>(velocity_vector.x()) * remaining_distance.x() + static_cast<double>(velocity_vector.y()) * remaining_distance.y()) / remaining_length;
    }

    auto duration = duration_for_distance(remaining_distance);

    // An animation moving faster than the remaining distance needs would swing past the destination and back.
    if (velocity > 0)
        duration = min(duration, AK::Duration::from_seconds_f64(remaining_length / velocity * retarget_ease_out_allowance));

    m_start_offset = current_offset;
    m_destination_offset = destination_offset;
    m_start_time = elapsed;
    m_duration = duration;
    m_easing_control_point_y1 = 0;

    if (duration.is_zero() || remaining_length == 0)
        return;

    // The curve's starting slope is y1 / x1.
    auto slope = velocity * duration.to_seconds_f64() / remaining_length;
    m_easing_control_point_y1 = easing_control_point_x1 * clamp(slope, -maximum_easing_slope, maximum_easing_slope);
}

}
