/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/PixelUnits.h>

namespace Web::CSS {

struct SizeWithAspectRatio {
    Optional<CSSPixels> width;
    Optional<CSSPixels> height;
    Optional<CSSPixelFraction> aspect_ratio;
    bool has_width() const { return width.has_value(); }
    bool has_height() const { return height.has_value(); }
    bool has_aspect_ratio() const { return aspect_ratio.has_value(); }
};

// https://drafts.csswg.org/css-images/#default-sizing
CSSPixelSize run_default_sizing_algorithm(
    Optional<CSSPixels> specified_width, Optional<CSSPixels> specified_height,
    SizeWithAspectRatio const& natural_size,
    CSSPixelSize default_size);

}
