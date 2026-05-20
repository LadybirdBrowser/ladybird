/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibWeb/CSS/StyleValues/ColorInterpolationMethodStyleValue.h>
#include <LibWeb/Forward.h>

namespace Web::Painting {

struct ColorStopData {
    Vector<Color, 4> colors;
    Vector<float, 4> positions;
    bool repeating { false };
};

struct LinearGradientData {
    float gradient_angle;
    float first_stop_position { 0 };
    float repeat_length { 1 };
    ColorStopData color_stops;
    CSS::ColorInterpolationMethodStyleValue::ColorInterpolationMethod interpolation_method;
};

struct ConicGradientData {
    float start_angle;
    ColorStopData color_stops;
    CSS::ColorInterpolationMethodStyleValue::ColorInterpolationMethod interpolation_method;
};

struct RadialGradientData {
    ColorStopData color_stops;
    CSS::ColorInterpolationMethodStyleValue::ColorInterpolationMethod interpolation_method;
};

}
