/*
 * Copyright (c) 2025, Tuur Martens <tuurmartens4@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <AK/Types.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Rect.h>

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

[[nodiscard]] bool is_valid_exif_orientation(u32 orientation);

template<typename T>
[[nodiscard]] Gfx::AffineTransform compute_exif_orientation_matrix(Gfx::ExifOrientation orientation, Gfx::Rect<T> const& dst_rect)
{
    Gfx::AffineTransform matrix;

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
        matrix.rotate_radians(AK::Pi<float>);
        break;
    case Gfx::ExifOrientation::FlipVertically:
        matrix.set_translation(0, dst_rect.height() / 2);
        matrix.set_scale(1, -1);
        matrix.translate(0, -dst_rect.height() / 2);
        break;
    case Gfx::ExifOrientation::Rotate90ClockwiseThenFlipHorizontally:
        matrix.set_translation(dst_rect.height(), 0);
        matrix.rotate_radians(-AK::Pi<float> / 2.f);
        matrix.translate(0, -dst_rect.height());
        matrix.scale(-1, 1);
        break;
    case Gfx::ExifOrientation::Rotate90Clockwise:
        matrix.set_translation(dst_rect.width(), 0);
        matrix.rotate_radians(AK::Pi<float> / 2.f);
        break;
    case Gfx::ExifOrientation::FlipHorizontallyThenRotate90Clockwise:
        // We translate by the dst_rect.height(), which will be the new dst_rect.width() of the image.
        matrix.set_translation(dst_rect.width(), 0);
        matrix.rotate_radians(AK::Pi<float> / 2.f);
        // We translate by the old dst_rect.height() to move the image back to the origin.
        matrix.translate(dst_rect.height(), 0);
        matrix.scale(-1, 1);
        break;
    case Gfx::ExifOrientation::Rotate90CounterClockwise:
        matrix.translate(0, dst_rect.height());
        matrix.rotate_radians(-AK::Pi<float> / 2.f);
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    return matrix;
}

template<typename T>
[[nodiscard]] Size<T> exif_oriented_size(Size<T> const& size, Gfx::ExifOrientation orientation)
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

[[nodiscard]] bool exif_orientation_affects_image_size(Gfx::ImageOrientation image_orientation, Gfx::ExifOrientation orientation);

}
