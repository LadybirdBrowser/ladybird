/*
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGfx/Gradients.h>
#include <LibGfx/PaintStyle.h>

namespace Gfx {

// Note: This file implements the CSS/Canvas gradients for LibWeb according to the spec.
// Please do not make ad-hoc changes that may break spec compliance!

float color_stop_step(ColorStop const& previous_stop, ColorStop const& next_stop, float position)
{
    if (position < previous_stop.position)
        return 0;
    if (position > next_stop.position)
        return 1;
    // For any given point between the two color stops,
    // determine the point’s location as a percentage of the distance between the two color stops.
    // Let this percentage be P.
    auto stop_length = next_stop.position - previous_stop.position;
    // FIXME: Avoids NaNs... Still not quite correct?
    if (stop_length <= 0)
        return 1;
    auto p = (position - previous_stop.position) / stop_length;
    if (!next_stop.transition_hint.has_value())
        return p;
    if (*next_stop.transition_hint >= 1)
        return 0;
    if (*next_stop.transition_hint <= 0)
        return 1;
    // Let C, the color weighting at that point, be equal to P^(logH(.5)).
    auto c = AK::pow(p, AK::log<float>(0.5) / AK::log(*next_stop.transition_hint));
    // The color at that point is then a linear blend between the colors of the two color stops,
    // blending (1 - C) of the first stop and C of the second stop.
    return c;
}

void SVGGradientPaintStyle::set_gradient_transform(AffineTransform transform)
{
    // Note: The scaling is removed so enough points on the gradient line are generated.
    // Otherwise, if you scale a tiny path the gradient looks pixelated.
    m_scale = 1.0f;
    if (auto inverse = transform.inverse(); inverse.has_value()) {
        auto transform_scale = transform.scale();
        m_scale = max(transform_scale.x(), transform_scale.y());
        m_inverse_transform = AffineTransform {}.scale(m_scale, m_scale).multiply(*inverse);
    } else {
        m_inverse_transform = OptionalNone {};
    }
}

}
