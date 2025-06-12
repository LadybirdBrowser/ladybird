/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/ViewBox.h>

namespace Web::SVG {

class SVGViewport {
public:
    virtual Optional<ViewBox> view_box() const = 0;
    virtual Optional<PreserveAspectRatio> preserve_aspect_ratio() const = 0;
    virtual ~SVGViewport() = default;
};

inline CSSPixels normalized_diagonal_length(CSSPixelSize viewport_size)
{
    if (viewport_size.width() == viewport_size.height())
        return viewport_size.width();
    return sqrt((viewport_size.width() * viewport_size.width()) + (viewport_size.height() * viewport_size.height())) / CSSPixels::nearest_value_for(AK::Sqrt2<float>);
}

}
