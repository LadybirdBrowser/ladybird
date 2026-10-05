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
