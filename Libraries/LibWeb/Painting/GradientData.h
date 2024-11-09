/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/Gradients.h>
#include <LibWeb/Forward.h>

namespace Web::Painting {

using ColorStopList = Vector<Gfx::ColorStop, 4>;

struct ColorStopData {
    ColorStopList list;
    Optional<float> repeat_length;
};

struct LinearGradientData {
    float gradient_angle;
    ColorStopData color_stops;
};

struct ConicGradientData {
    float start_angle;
    ColorStopData color_stops;
};

struct RadialGradientData {
    ColorStopData color_stops;
};

}
