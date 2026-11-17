/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCompositing/Export.h>
#include <LibGfx/ColorSpace.h>
#include <LibMediaCodecs/Color/CodingIndependentCodePoints.h>
#include <LibWebCommon/Bindings/PredefinedColorSpace.h>

namespace Compositing {

// https://html.spec.whatwg.org/multipage/canvas.html#predefinedcolorspace
// The coding-independent code points that describe the predefined color space.
COMPOSITING_API Media::CodingIndependentCodePoints coding_independent_code_points_for(Web::Bindings::PredefinedColorSpace);

// The color space of the pixels of a bitmap in the predefined color space. That of "srgb" is the color space that a
// bitmap without color space information is in.
COMPOSITING_API Gfx::ColorSpace gfx_color_space_for(Web::Bindings::PredefinedColorSpace);

}
