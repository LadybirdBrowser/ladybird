/*
 * Copyright (c) 2025, Tuur Martens <tuurmartens4@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

#include "AffineTransform.h"

namespace Web::CSS {
enum class ImageOrientation : u8;
}

namespace Gfx {

enum class ImageOrientation : u8 {
    FromExif,
    FromDecoded,
};

enum class ExifOrientation : u8 {
    Default = 1,
    FlipHorizontally = 2,
    Rotate180 = 3,
    FlipVertically = 4,
    Rotate90ClockwiseThenFlipHorizontally = 5,
    Rotate90Clockwise = 6,
    FlipHorizontallyThenRotate90Clockwise = 7,
    Rotate90CounterClockwise = 8,
};

[[nodiscard]]
bool is_valid_exif_orientation(u32 orientation);

ImageOrientation to_gfx_image_orientation(Web::CSS::ImageOrientation orientation);

AffineTransform compute_exif_orientation_matrix(ExifOrientation orientation, FloatRect& dst_rect);

template<typename T>
[[nodiscard]]
static Size<T> exif_oriented_size(Size<T> size, Gfx::ExifOrientation orientation)
{
    switch (orientation) {
    case Gfx::ExifOrientation::Default:
    case Gfx::ExifOrientation::FlipHorizontally:
    case Gfx::ExifOrientation::Rotate180:
    case Gfx::ExifOrientation::FlipVertically:
        return size;
    case Gfx::ExifOrientation::Rotate90ClockwiseThenFlipHorizontally:
    case Gfx::ExifOrientation::Rotate90Clockwise:
    case Gfx::ExifOrientation::FlipHorizontallyThenRotate90Clockwise:
    case Gfx::ExifOrientation::Rotate90CounterClockwise:
        return { size.height(), size.width() };
    }
    VERIFY_NOT_REACHED();
}

}
