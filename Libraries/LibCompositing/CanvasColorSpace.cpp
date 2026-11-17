/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/CanvasColorSpace.h>

namespace Compositing {

// https://html.spec.whatwg.org/multipage/canvas.html#predefinedcolorspace
Media::CodingIndependentCodePoints coding_independent_code_points_for(Web::Bindings::PredefinedColorSpace color_space)
{
    auto code_points = [](Media::ColorPrimaries color_primaries, Media::TransferCharacteristics transfer_characteristics) {
        return Media::CodingIndependentCodePoints { color_primaries, transfer_characteristics, Media::MatrixCoefficients::Identity, Media::VideoFullRangeFlag::Full };
    };

    switch (color_space) {
    // The "srgb" value indicates the 'srgb' color space.
    case Web::Bindings::PredefinedColorSpace::Srgb:
        return code_points(Media::ColorPrimaries::BT709, Media::TransferCharacteristics::SRGB);
    // The "srgb-linear" value indicates the 'srgb-linear' color space.
    case Web::Bindings::PredefinedColorSpace::SrgbLinear:
        return code_points(Media::ColorPrimaries::BT709, Media::TransferCharacteristics::Linear);
    // The "display-p3" value indicates the 'display-p3' color space.
    case Web::Bindings::PredefinedColorSpace::DisplayP3:
        return code_points(Media::ColorPrimaries::SMPTE432, Media::TransferCharacteristics::SRGB);
    // The "display-p3-linear" value indicates the 'display-p3-linear' color space.
    case Web::Bindings::PredefinedColorSpace::DisplayP3Linear:
        return code_points(Media::ColorPrimaries::SMPTE432, Media::TransferCharacteristics::Linear);
    }
    VERIFY_NOT_REACHED();
}

Gfx::ColorSpace gfx_color_space_for(Web::Bindings::PredefinedColorSpace color_space)
{
    if (color_space == Web::Bindings::PredefinedColorSpace::Srgb)
        return {};
    return MUST(Gfx::ColorSpace::from_cicp(coding_independent_code_points_for(color_space)));
}

}
