/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/ColorSpace.h>
#include <LibMediaCodecs/Color/CodingIndependentCodePoints.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Bindings/PredefinedColorSpace.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#predefinedcolorspace
// The coding-independent code points that describe the predefined color space.
WEB_API Media::CodingIndependentCodePoints coding_independent_code_points_for(Bindings::PredefinedColorSpace);

// The color space of the pixels of a bitmap in the predefined color space. That of "srgb" is the color space that a
// bitmap without color space information is in.
WEB_API Gfx::ColorSpace gfx_color_space_for(Bindings::PredefinedColorSpace);

// The CSS color space of the predefined color space.
WEB_API CSS::ColorStyleValue::ColorType css_color_type_for(Bindings::PredefinedColorSpace);

// The predefined color space whose underlying value the Rust implementation passes.
WEB_API Bindings::PredefinedColorSpace predefined_color_space_from_underlying(u8);

}
