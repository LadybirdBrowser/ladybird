/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Canvas/PredefinedColorSpace.h>

namespace Web::HTML {

static_assert(to_underlying(Bindings::PredefinedColorSpace::Srgb) == 0);
static_assert(to_underlying(Bindings::PredefinedColorSpace::SrgbLinear) == 1);
static_assert(to_underlying(Bindings::PredefinedColorSpace::DisplayP3) == 2);
static_assert(to_underlying(Bindings::PredefinedColorSpace::DisplayP3Linear) == 3);

CSS::ColorStyleValue::ColorType css_color_type_for(Bindings::PredefinedColorSpace color_space)
{
    switch (color_space) {
    case Bindings::PredefinedColorSpace::Srgb:
        return CSS::ColorStyleValue::ColorType::sRGB;
    case Bindings::PredefinedColorSpace::SrgbLinear:
        return CSS::ColorStyleValue::ColorType::sRGBLinear;
    case Bindings::PredefinedColorSpace::DisplayP3:
        return CSS::ColorStyleValue::ColorType::DisplayP3;
    case Bindings::PredefinedColorSpace::DisplayP3Linear:
        return CSS::ColorStyleValue::ColorType::DisplayP3Linear;
    }
    VERIFY_NOT_REACHED();
}

Bindings::PredefinedColorSpace predefined_color_space_from_underlying(u8 value)
{
    VERIFY(value <= to_underlying(Bindings::PredefinedColorSpace::DisplayP3Linear));
    return static_cast<Bindings::PredefinedColorSpace>(value);
}

}
