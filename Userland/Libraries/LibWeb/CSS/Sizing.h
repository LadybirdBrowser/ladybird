/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/PixelUnits.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-images/#default-sizing
CSSPixelSize run_default_sizing_algorithm(
    Optional<CSSPixels> specified_width, Optional<CSSPixels> specified_height,
    Optional<CSSPixels> natural_width, Optional<CSSPixels> natural_height,
    Optional<CSSPixelFraction> natural_aspect_ratio,
    CSSPixelSize default_size);

}
