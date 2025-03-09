/*
 * Copyright (c) 2025, Tuur Martens <tuurmartens4@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImageOrientation.h>
#include <math.h>

#include "Rect.h"

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

ImageOrientation to_gfx_image_orientation(Web::CSS::ImageOrientation orientation)
{
    return static_cast<ImageOrientation>(orientation);
}

Gfx::AffineTransform compute_exif_orientation_matrix(Gfx::ExifOrientation orientation, Gfx::FloatRect& dst_rect)
{
    Gfx::AffineTransform matrix;

    switch (orientation) {
    case Gfx::ExifOrientation::Rotate90ClockwiseThenFlipHorizontally:
    case Gfx::ExifOrientation::Rotate90Clockwise:
    case Gfx::ExifOrientation::FlipHorizontallyThenRotate90Clockwise:
    case Gfx::ExifOrientation::Rotate90CounterClockwise:
        dst_rect.set_size(dst_rect.height(), dst_rect.width());
        break;
    default:
        break;
    }

    switch (orientation) {
    case Gfx::ExifOrientation::Default:
        return matrix;
    case Gfx::ExifOrientation::FlipHorizontally:
        matrix.set_translation(dst_rect.width() / 2, 0);
        matrix.set_scale(-1, 1);
        matrix.translate(-dst_rect.width() / 2.f, 0);
        break;
    case Gfx::ExifOrientation::Rotate180:
        matrix.set_translation(dst_rect.width(), dst_rect.height());
        matrix.rotate_radians(M_PI);
        break;
    case Gfx::ExifOrientation::FlipVertically:
        matrix.set_translation(0, dst_rect.height() / 2);
        matrix.set_scale(1, -1);
        matrix.translate(0, -dst_rect.height() / 2);
        break;
    case Gfx::ExifOrientation::Rotate90ClockwiseThenFlipHorizontally:
        matrix.set_translation(dst_rect.height(), 0);
        matrix.rotate_radians(-M_PI / 2.);
        matrix.translate(0, -dst_rect.height());
        matrix.scale(-1, 1);
        break;
    case Gfx::ExifOrientation::Rotate90Clockwise:
        matrix.set_translation(dst_rect.height(), 0);
        matrix.rotate_radians(M_PI / 2.);
        break;
    case Gfx::ExifOrientation::FlipHorizontallyThenRotate90Clockwise:
        // We translate by the height, which will be the new width of the image.
        matrix.set_translation(dst_rect.height(), 0);
        matrix.rotate_radians(M_PI / 2.);
        // We translate by the old height to move the image back to the origin.
        matrix.translate(dst_rect.width(), 0);
        matrix.scale(-1, 1);
        break;
    case Gfx::ExifOrientation::Rotate90CounterClockwise:
        matrix.translate(0, dst_rect.width());
        matrix.rotate_radians(-M_PI / 2);
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    return matrix;
}

}
