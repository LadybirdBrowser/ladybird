/*
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Sizing.h"

namespace Web::CSS {

// https://drafts.csswg.org/css-images/#default-sizing
CSSPixelSize run_default_sizing_algorithm(
    Optional<CSSPixels> specified_width, Optional<CSSPixels> specified_height,
    Optional<CSSPixels> natural_width, Optional<CSSPixels> natural_height,
    Optional<CSSPixelFraction> natural_aspect_ratio,
    CSSPixelSize default_size)
{
    // If the specified size is a definite width and height, the concrete object size is given that width and height.
    if (specified_width.has_value() && specified_height.has_value())
        return CSSPixelSize { specified_width.value(), specified_height.value() };
    // If the specified size is only a width or height (but not both) then the concrete object size is given that specified width or height.
    // The other dimension is calculated as follows:
    if (specified_width.has_value() || specified_height.has_value()) {
        // 1. If the object has a natural aspect ratio,
        // the missing dimension of the concrete object size is calculated using that aspect ratio and the present dimension.
        if (natural_aspect_ratio.has_value() && !natural_aspect_ratio->might_be_saturated()) {
            if (specified_width.has_value())
                return CSSPixelSize { specified_width.value(), (CSSPixels(1) / natural_aspect_ratio.value()) * specified_width.value() };
            if (specified_height.has_value())
                return CSSPixelSize { specified_height.value() * natural_aspect_ratio.value(), specified_height.value() };
        }
        // 2. Otherwise, if the missing dimension is present in the object’s natural dimensions,
        // the missing dimension is taken from the object’s natural dimensions.
        if (specified_height.has_value() && natural_width.has_value())
            return CSSPixelSize { natural_width.value(), specified_height.value() };
        if (specified_width.has_value() && natural_height.has_value())
            return CSSPixelSize { specified_width.value(), natural_height.value() };
        // 3. Otherwise, the missing dimension of the concrete object size is taken from the default object size.
        if (specified_height.has_value())
            return CSSPixelSize { default_size.width(), specified_height.value() };
        if (specified_width.has_value())
            return CSSPixelSize { specified_width.value(), default_size.height() };
        VERIFY_NOT_REACHED();
    }
    // If the specified size has no constraints:
    // 1. If the object has a natural height or width, its size is resolved as if its natural dimensions were given as the specified size.
    if (natural_width.has_value() || natural_height.has_value())
        return run_default_sizing_algorithm(natural_width, natural_height, natural_width, natural_height, natural_aspect_ratio, default_size);
    // FIXME: 2. Otherwise, its size is resolved as a contain constraint against the default object size.
    return default_size;
}

}
