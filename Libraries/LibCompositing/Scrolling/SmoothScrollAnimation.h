/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/Types.h>
#include <LibGfx/Point.h>

namespace Compositing {

// During a momentum scroll, each frame moves this fraction of the distance of the frame before it.
inline constexpr double momentum_distance_share_per_frame = 0.92;
inline constexpr double momentum_frame_duration_in_seconds = 0.016;
inline constexpr double maximum_momentum_duration_in_seconds = 5.0;

class COMPOSITING_API SmoothScrollAnimation {
public:
    struct Sample {
        Gfx::FloatPoint offset;
        bool complete { false };
    };

    SmoothScrollAnimation(Gfx::FloatPoint start_offset, Gfx::FloatPoint destination_offset, double pixels_per_css_pixel, ScrollAnimationKind = ScrollAnimationKind::SmoothScroll);

    AK::Duration duration() const { return m_duration; }
    Gfx::FloatPoint destination_offset() const { return m_destination_offset; }
    ScrollAnimationKind kind() const { return m_kind; }
    Sample sample(AK::Duration elapsed) const;

    void retarget(Gfx::FloatPoint destination_offset, AK::Duration elapsed);

private:
    AK::Duration duration_for_distance(Gfx::FloatPoint distance) const;
    double eased_progress_at(double progress) const;
    double easing_slope_at(double progress) const;
    Gfx::FloatPoint velocity_at(AK::Duration elapsed) const;

    Gfx::FloatPoint m_start_offset;
    Gfx::FloatPoint m_destination_offset;
    double m_pixels_per_css_pixel { 1 };
    AK::Duration m_start_time;
    AK::Duration m_duration;
    ScrollAnimationKind m_kind { ScrollAnimationKind::SmoothScroll };
    double m_easing_control_point_y1 { 0 };
};

}
