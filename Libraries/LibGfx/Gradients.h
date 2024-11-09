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

class GradientLine;

inline float normalized_gradient_angle_radians(float gradient_angle)
{
    // Adjust angle so 0 degrees is bottom
    float real_angle = 90 - gradient_angle;
    return AK::to_radians(real_angle);
}

template<typename T>
inline float calculate_gradient_length(Size<T> gradient_size, float sin_angle, float cos_angle)
{
    return AK::fabs(static_cast<float>(gradient_size.height()) * sin_angle) + AK::fabs(static_cast<float>(gradient_size.width()) * cos_angle);
}

template<typename T>
inline float calculate_gradient_length(Size<T> gradient_size, float gradient_angle)
{
    float angle = normalized_gradient_angle_radians(gradient_angle);
    float sin_angle, cos_angle;
    AK::sincos(angle, sin_angle, cos_angle);
    return calculate_gradient_length(gradient_size, sin_angle, cos_angle);
}

}
