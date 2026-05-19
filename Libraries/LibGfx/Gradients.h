/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <LibGfx/Color.h>
#include <LibGfx/Size.h>

namespace Gfx {

struct ColorStop {
    Color color;
    float position = AK::NaN<float>;
    Optional<float> transition_hint = {};

    bool operator==(ColorStop const&) const = default;
};

float color_stop_step(ColorStop const& previous_stop, ColorStop const& next_stop, float position);

template<typename T>
inline float calculate_gradient_length(Size<T> gradient_size, float gradient_angle)
{
    // Adjust angle so 0 degrees is bottom.
    float angle = AK::to_radians(90 - gradient_angle);
    float sin_angle, cos_angle;
    AK::sincos(angle, sin_angle, cos_angle);
    return AK::fabs(static_cast<float>(gradient_size.height()) * sin_angle) + AK::fabs(static_cast<float>(gradient_size.width()) * cos_angle);
}

}
