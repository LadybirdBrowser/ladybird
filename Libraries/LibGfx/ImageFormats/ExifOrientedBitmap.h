/*
 * Copyright (c) 2023-2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>

namespace Gfx {

class ExifOrientedBitmap {
public:
    static ErrorOr<ExifOrientedBitmap> create(TIFF::Orientation orientation, IntSize size, BitmapFormat format, AlphaType alpha_type)
    {
        auto bitmap = TRY(Bitmap::create_shareable_with_gpu_access(format, alpha_type, oriented_size(size, orientation)));
        return ExifOrientedBitmap(move(bitmap), size, orientation);
    }

    void set_pixel(u32 x, u32 y, RawPixel color)
    {
        auto const new_position = oriented_position(IntPoint(x, y), { static_cast<int>(m_width), static_cast<int>(m_height) }, m_orientation);
        m_bitmap->scanline(new_position.y())[new_position.x()] = color;
    }

    NonnullRefPtr<Bitmap>& bitmap()
    {
        return m_bitmap;
    }

    static IntSize oriented_size(IntSize size, TIFF::Orientation orientation)
    {
        switch (orientation) {
        case Orientation::Default:
        case Orientation::FlipHorizontally:
        case Orientation::Rotate180:
        case Orientation::FlipVertically:
            return size;
        case Orientation::Rotate90ClockwiseThenFlipHorizontally:
        case Orientation::Rotate90Clockwise:
        case Orientation::FlipHorizontallyThenRotate90Clockwise:
        case Orientation::Rotate90CounterClockwise:
            return { size.height(), size.width() };
        }
        VERIFY_NOT_REACHED();
    }

    // Where the pixel at the point of an image of the given size goes once the image is shown in the orientation.
    static IntPoint oriented_position(IntPoint point, IntSize size, TIFF::Orientation orientation)
    {
        auto const width = size.width();
        auto const height = size.height();
        auto const flip_horizontally = [width](IntPoint point) {
            return IntPoint(width - point.x() - 1, point.y());
        };

        auto const rotate_90_clockwise = [height](IntPoint point) {
            return IntPoint(height - point.y() - 1, point.x());
        };

        switch (orientation) {
        case TIFF::Orientation::Default:
            return point;
        case TIFF::Orientation::FlipHorizontally:
            return flip_horizontally(point);
        case TIFF::Orientation::Rotate180:
            return IntPoint(width - point.x() - 1, height - point.y() - 1);
        case TIFF::Orientation::FlipVertically:
            return IntPoint(point.x(), height - point.y() - 1);
        case TIFF::Orientation::Rotate90ClockwiseThenFlipHorizontally:
            return IntPoint(point.y(), point.x());
        case TIFF::Orientation::Rotate90Clockwise:
            return rotate_90_clockwise(point);
        case TIFF::Orientation::FlipHorizontallyThenRotate90Clockwise:
            return rotate_90_clockwise(flip_horizontally(point));
        case TIFF::Orientation::Rotate90CounterClockwise:
            return IntPoint(point.y(), width - point.x() - 1);
        }
        VERIFY_NOT_REACHED();
    }

private:
    using Orientation = TIFF::Orientation;

    ExifOrientedBitmap(NonnullRefPtr<Bitmap> bitmap, IntSize size, Orientation orientation)
        : m_bitmap(move(bitmap))
        , m_orientation(orientation)
        , m_width(size.width())
        , m_height(size.height())
    {
    }

    NonnullRefPtr<Bitmap> m_bitmap;
    Orientation m_orientation;

    u32 m_width {};
    u32 m_height {};
};

}
