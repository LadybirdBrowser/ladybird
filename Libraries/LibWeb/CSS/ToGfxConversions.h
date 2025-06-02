/*
 * Copyright (c) 2020-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Tuur Martens <tuurmartens4@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <LibGfx/ImageOrientation.h>
#include <LibWeb/CSS/Enums.h>

namespace Web::CSS {

[[nodiscard]]
inline Gfx::ImageOrientation to_gfx_image_orientation(CSS::ImageOrientation css_value)
{
    switch (css_value) {
    case CSS::ImageOrientation::None:
        return Gfx::ImageOrientation::FromDecoded;
    case CSS::ImageOrientation::FromImage:
        return Gfx::ImageOrientation::FromExif;
    default:
        VERIFY_NOT_REACHED();
    }
}

}
