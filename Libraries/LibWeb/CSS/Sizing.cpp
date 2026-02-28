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
    Optional<CSSPixels> specified_width,
    Optional<CSSPixels> specified_height,
    SizeWithAspectRatio const& natural,
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
        if (natural.has_aspect_ratio() && !natural.aspect_ratio->might_be_saturated()) {
            if (specified_width.has_value())
                return CSSPixelSize { specified_width.value(), (CSSPixels(1) / natural.aspect_ratio.value()) * specified_width.value() };
            if (specified_height.has_value())
                return CSSPixelSize { specified_height.value() * natural.aspect_ratio.value(), specified_height.value() };
        }
        // 2. Otherwise, if the missing dimension is present in the object’s natural dimensions,
        // the missing dimension is taken from the object’s natural dimensions.
        if (specified_height.has_value() && natural.has_width())
            return CSSPixelSize { natural.width.value(), specified_height.value() };
        if (specified_width.has_value() && natural.has_height())
            return CSSPixelSize { specified_width.value(), natural.height.value() };
        // 3. Otherwise, the missing dimension of the concrete object size is taken from the default object size.
        if (specified_height.has_value())
            return CSSPixelSize { default_size.width(), specified_height.value() };
        if (specified_width.has_value())
            return CSSPixelSize { specified_width.value(), default_size.height() };
        VERIFY_NOT_REACHED();
    }
    // If the specified size has no constraints:
    // 1. If the object has a natural height or width, its size is resolved as if its natural dimensions were given as the specified size.
    if (natural.has_width() || natural.has_height())
        return run_default_sizing_algorithm(natural.width, natural.height, natural, default_size);

    // 2. Otherwise, its size is resolved as a contain constraint against the default object size.
    if (natural.has_aspect_ratio() && !natural.aspect_ratio->might_be_saturated()) {
        auto aspect_ratio = natural.aspect_ratio.value();
        if (default_size.is_empty())
            return default_size;
        auto default_width = default_size.width().to_double();
        auto default_height = default_size.height().to_double();
        if (aspect_ratio.to_double() >= default_width / default_height)
            return CSSPixelSize { default_size.width(), CSSPixels::nearest_value_for(default_width / aspect_ratio.to_double()) };
        return CSSPixelSize { CSSPixels::nearest_value_for(default_height * aspect_ratio.to_double()), default_size.height() };
    }
    return default_size;
}

}
