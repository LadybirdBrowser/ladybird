/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/CSS/EasingFunction.h>
#include <LibWeb/Compositor/SmoothScrollAnimation.h>

namespace Web::Compositor {

static constexpr double scroll_speed_in_pixels_per_second = 1000.0;
static constexpr double maximum_scroll_duration_in_seconds = 0.2;

SmoothScrollAnimation::SmoothScrollAnimation(Gfx::FloatPoint start_offset, Gfx::FloatPoint destination_offset)
    : m_start_offset(start_offset)
    , m_destination_offset(destination_offset)
{
    auto horizontal_distance = static_cast<double>(destination_offset.x() - start_offset.x());
    auto vertical_distance = static_cast<double>(destination_offset.y() - start_offset.y());
    auto distance = AK::sqrt(horizontal_distance * horizontal_distance + vertical_distance * vertical_distance);
    auto duration_in_seconds = min(distance / scroll_speed_in_pixels_per_second, maximum_scroll_duration_in_seconds);
    m_duration = AK::Duration::from_seconds_f64(duration_in_seconds);
}

SmoothScrollAnimation::Sample SmoothScrollAnimation::sample(AK::Duration elapsed) const
{
    if (m_duration.is_zero() || elapsed >= m_duration)
        return { m_destination_offset, true };

    auto progress = clamp(elapsed.to_seconds_f64() / m_duration.to_seconds_f64(), 0.0, 1.0);

    // https://drafts.csswg.org/cssom-view/#smooth-scroll
    // A smooth scroll follows a user-agent-defined timing function. Match the
    // ease-in-out curve used by WebKit for programmatic smooth scrolling.
    static CSS::CubicBezierEasingFunction const easing_function { 0.42, 0, 0.58, 1, {} };
    auto eased_progress = easing_function.evaluate_at(progress, false);

    return {
        {
            static_cast<float>(m_start_offset.x() + (m_destination_offset.x() - m_start_offset.x()) * eased_progress),
            static_cast<float>(m_start_offset.y() + (m_destination_offset.y() - m_start_offset.y()) * eased_progress),
        },
        false,
    };
}

}
