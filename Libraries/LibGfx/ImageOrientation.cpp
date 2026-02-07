/*
 * Copyright (c) 2025, Tuur Martens <tuurmartens4@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImageOrientation.h>

namespace Gfx {

bool is_valid_exif_orientation(u32 orientation)
{
    switch (static_cast<Gfx::ExifOrientation>(orientation)) {
    case Gfx::ExifOrientation::Default:
    case Gfx::ExifOrientation::FlipHorizontally:
    case Gfx::ExifOrientation::Rotate180:
    case Gfx::ExifOrientation::FlipVertically:
    case Gfx::ExifOrientation::Rotate90ClockwiseThenFlipHorizontally:
    case Gfx::ExifOrientation::Rotate90Clockwise:
    case Gfx::ExifOrientation::FlipHorizontallyThenRotate90Clockwise:
    case Gfx::ExifOrientation::Rotate90CounterClockwise:
        return true;
    default:
        return false;
    }
}

bool exif_orientation_affects_image_size(Gfx::ExifOrientation orientation)
{
    switch (orientation) {
    case Gfx::ExifOrientation::Rotate90Clockwise:
    case Gfx::ExifOrientation::Rotate90CounterClockwise:
    case Gfx::ExifOrientation::FlipHorizontallyThenRotate90Clockwise:
    case Gfx::ExifOrientation::Rotate90ClockwiseThenFlipHorizontally:
        return true;
    default:
        return false;
    }
}

}
