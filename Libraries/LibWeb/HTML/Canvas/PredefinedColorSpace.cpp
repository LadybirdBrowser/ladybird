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

// https://html.spec.whatwg.org/multipage/canvas.html#predefinedcolorspace
Media::CodingIndependentCodePoints coding_independent_code_points_for(Bindings::PredefinedColorSpace color_space)
{
    auto code_points = [](Media::ColorPrimaries color_primaries, Media::TransferCharacteristics transfer_characteristics) {
        return Media::CodingIndependentCodePoints { color_primaries, transfer_characteristics, Media::MatrixCoefficients::Identity, Media::VideoFullRangeFlag::Full };
    };

    switch (color_space) {
    // The "srgb" value indicates the 'srgb' color space.
    case Bindings::PredefinedColorSpace::Srgb:
        return code_points(Media::ColorPrimaries::BT709, Media::TransferCharacteristics::SRGB);
    // The "srgb-linear" value indicates the 'srgb-linear' color space.
    case Bindings::PredefinedColorSpace::SrgbLinear:
        return code_points(Media::ColorPrimaries::BT709, Media::TransferCharacteristics::Linear);
    // The "display-p3" value indicates the 'display-p3' color space.
    case Bindings::PredefinedColorSpace::DisplayP3:
        return code_points(Media::ColorPrimaries::SMPTE432, Media::TransferCharacteristics::SRGB);
    // The "display-p3-linear" value indicates the 'display-p3-linear' color space.
    case Bindings::PredefinedColorSpace::DisplayP3Linear:
        return code_points(Media::ColorPrimaries::SMPTE432, Media::TransferCharacteristics::Linear);
    }
    VERIFY_NOT_REACHED();
}

Gfx::ColorSpace gfx_color_space_for(Bindings::PredefinedColorSpace color_space)
{
    if (color_space == Bindings::PredefinedColorSpace::Srgb)
        return {};
    return MUST(Gfx::ColorSpace::from_cicp(coding_independent_code_points_for(color_space)));
}

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
