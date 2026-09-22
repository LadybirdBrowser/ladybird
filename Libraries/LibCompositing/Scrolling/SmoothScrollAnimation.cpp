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

// The share of a frame's distance that the frame after it covers while momentum decays, and the length of the frames
// that share is measured in.
static constexpr double momentum_distance_share_per_frame = 0.92;
static constexpr double momentum_frame_duration_in_seconds = 0.016;
static constexpr double maximum_momentum_duration_in_seconds = 5.0;

// https://drafts.csswg.org/css-easing/#cubic-bezier-algo
// The ease-in-out curve WebKit uses for programmatic smooth scrolling, cubic-bezier(0.42, 0, 0.58, 1).
static double ease_in_out(double progress)
{
    constexpr double x1 = 0.42;
    constexpr double y1 = 0;
    constexpr double x2 = 0.58;
    constexpr double y2 = 1;

    auto bezier = [](double t, double p1, double p2) {
        return 3 * (1 - t) * (1 - t) * t * p1 + 3 * (1 - t) * t * t * p2 + t * t * t;
    };
    auto bezier_derivative = [](double t, double p1, double p2) {
        return 3 * (1 - t) * (1 - t) * p1 + 6 * (1 - t) * t * (p2 - p1) + 3 * t * t * (1 - p2);
    };

    // Find the curve parameter whose horizontal position is the progress: a few Newton steps usually land on it, and
    // bisection finishes the job when the slope gets too flat for them.
    auto t = progress;
    for (int i = 0; i < 8; ++i) {
        auto x = bezier(t, x1, x2) - progress;
        if (AK::abs(x) < 1e-7)
            return bezier(t, y1, y2);
        auto slope = bezier_derivative(t, x1, x2);
        if (AK::abs(slope) < 1e-6)
            break;
        t -= x / slope;
    }
    double low = 0;
    double high = 1;
    while (high - low > 1e-7) {
        t = (low + high) / 2;
        if (bezier(t, x1, x2) < progress)
            low = t;
        else
            high = t;
    }
    return bezier(t, y1, y2);
}

static double momentum_frames_for_distance(double distance)
{
    auto frames = AK::ceil(-AK::log(1 - distance * (1 - 1 / momentum_distance_share_per_frame)) / AK::log(momentum_distance_share_per_frame));
    return min(frames, maximum_momentum_duration_in_seconds / momentum_frame_duration_in_seconds);
}

SmoothScrollAnimation::SmoothScrollAnimation(Gfx::FloatPoint start_offset, Gfx::FloatPoint destination_offset, double pixels_per_css_pixel, ScrollAnimationKind kind)
    : m_start_offset(start_offset)
    , m_destination_offset(destination_offset)
    , m_kind(kind)
{
    VERIFY(pixels_per_css_pixel > 0);
    auto horizontal_distance = static_cast<double>(destination_offset.x() - start_offset.x()) / pixels_per_css_pixel;
    auto vertical_distance = static_cast<double>(destination_offset.y() - start_offset.y()) / pixels_per_css_pixel;
    auto distance = AK::sqrt(horizontal_distance * horizontal_distance + vertical_distance * vertical_distance);

    auto duration_in_seconds = kind == ScrollAnimationKind::Momentum
        ? momentum_frames_for_distance(distance) * momentum_frame_duration_in_seconds
        : min(distance / scroll_speed_in_pixels_per_second, maximum_scroll_duration_in_seconds);
    m_duration = AK::Duration::from_seconds_f64(duration_in_seconds);
}

SmoothScrollAnimation::Sample SmoothScrollAnimation::sample(AK::Duration elapsed) const
{
    if (m_duration.is_zero() || elapsed >= m_duration)
        return { m_destination_offset, true };

    auto progress = clamp(elapsed.to_seconds_f64() / m_duration.to_seconds_f64(), 0.0, 1.0);

    double eased_progress = 0;
    if (m_kind == ScrollAnimationKind::Momentum) {
        auto frames = m_duration.to_seconds_f64() / momentum_frame_duration_in_seconds;
        auto frames_elapsed = progress * frames;
        eased_progress = (1 - AK::pow(momentum_distance_share_per_frame, frames_elapsed)) / (1 - AK::pow(momentum_distance_share_per_frame, frames));
    } else {
        // https://drafts.csswg.org/cssom-view/#smooth-scroll
        // A smooth scroll follows a user-agent-defined timing function.
        eased_progress = ease_in_out(progress);
    }

    return {
        {
            static_cast<float>(m_start_offset.x() + (m_destination_offset.x() - m_start_offset.x()) * eased_progress),
            static_cast<float>(m_start_offset.y() + (m_destination_offset.y() - m_start_offset.y()) * eased_progress),
        },
        false,
    };
}

}
