/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/PaintStyle.h>

namespace Web::Painting {

void SVGGradientPaintStyle::set_gradient_transform(Gfx::AffineTransform transform)
{
    // Note: The scaling is removed so enough points on the gradient line are generated.
    // Otherwise, if you scale a tiny path the gradient looks pixelated.
    m_scale = 1.0f;
    if (auto inverse = transform.inverse(); inverse.has_value()) {
        auto transform_scale = transform.scale();
        m_scale = max(transform_scale.x(), transform_scale.y());
        m_inverse_transform = Gfx::AffineTransform {}.scale(m_scale, m_scale).multiply(*inverse);
    } else {
        m_inverse_transform = OptionalNone {};
    }
}

}
