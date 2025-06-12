/*
 * Copyright (c) 2023-2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/NonnullOwnPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>

namespace Gfx {

namespace Detail {

template<typename BitmapLike>
class ExifOrientedBitmap {
public:
    static ErrorOr<ExifOrientedBitmap> create(TIFF::Orientation orientation, IntSize size, BitmapFormat format)
    requires(SameAs<BitmapLike, Bitmap>)
    {
        auto bitmap = TRY(Bitmap::create(format, oriented_size(size, orientation)));
        return ExifOrientedBitmap(move(bitmap), size, orientation);
    }

    static ErrorOr<ExifOrientedBitmap> create(TIFF::Orientation orientation, IntSize size)
    requires(SameAs<BitmapLike, CMYKBitmap>)
    {
        auto bitmap = TRY(CMYKBitmap::create_with_size(oriented_size(size, orientation)));
        return ExifOrientedBitmap(move(bitmap), size, orientation);
    }

    template<OneOf<ARGB32, CMYK> Value>
    void set_pixel(u32 x, u32 y, Value color)
    {
        auto const new_position = oriented_position(IntPoint(x, y));
        m_bitmap->scanline(new_position.y())[new_position.x()] = color;
    }

    NonnullRefPtr<BitmapLike>& bitmap()
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

private:
    using Orientation = TIFF::Orientation;

    ExifOrientedBitmap(NonnullRefPtr<BitmapLike> bitmap, IntSize size, Orientation orientation)
        : m_bitmap(move(bitmap))
        , m_orientation(orientation)
        , m_width(size.width())
        , m_height(size.height())
    {
    }

    IntPoint oriented_position(IntPoint point)
    {
        auto const flip_horizontally = [this](IntPoint point) {
            return IntPoint(m_width - point.x() - 1, point.y());
        };

        auto const rotate_90_clockwise = [this](IntPoint point) {
            return IntPoint(m_height - point.y() - 1, point.x());
        };

        switch (m_orientation) {
        case Orientation::Default:
            return point;
        case Orientation::FlipHorizontally:
            return flip_horizontally(point);
        case Orientation::Rotate180:
            return IntPoint(m_width - point.x() - 1, m_height - point.y() - 1);
        case Orientation::FlipVertically:
            return IntPoint(point.x(), m_height - point.y() - 1);
        case Orientation::Rotate90ClockwiseThenFlipHorizontally:
            return flip_horizontally(rotate_90_clockwise(point));
        case Orientation::Rotate90Clockwise:
            return rotate_90_clockwise(point);
        case Orientation::FlipHorizontallyThenRotate90Clockwise:
            return rotate_90_clockwise(flip_horizontally(point));
        case Orientation::Rotate90CounterClockwise:
            return IntPoint(point.y(), m_width - point.x() - 1);
        }
        VERIFY_NOT_REACHED();
    }

    NonnullRefPtr<BitmapLike> m_bitmap;
    Orientation m_orientation;

    u32 m_width {};
    u32 m_height {};
};

}

using ExifOrientedBitmap = Detail::ExifOrientedBitmap<Bitmap>;
using ExifOrientedCMYKBitmap = Detail::ExifOrientedBitmap<CMYKBitmap>;

}
