/*
 * Copyright (c) 2023-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintingSurface.h>

namespace Gfx {

int ImmutableBitmap::width() const
{
    return m_bitmap->width();
}

int ImmutableBitmap::height() const
{
    return m_bitmap->height();
}

IntRect ImmutableBitmap::rect() const
{
    return { {}, size() };
}

IntSize ImmutableBitmap::size() const
{
    return { width(), height() };
}

AlphaType ImmutableBitmap::alpha_type() const
{
    return m_bitmap->alpha_type();
}

ColorSpace const& ImmutableBitmap::color_space() const
{
    return m_color_space;
}

RefPtr<Gfx::Bitmap const> ImmutableBitmap::bitmap() const
{
    return m_bitmap;
}

Color ImmutableBitmap::get_pixel(int x, int y) const
{
    return m_bitmap->get_pixel(x, y);
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap const> const& bitmap, ColorSpace color_space)
{
    return adopt_ref(*new ImmutableBitmap(bitmap, move(color_space)));
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap const> const& bitmap, AlphaType alpha_type, ColorSpace color_space)
{
    // Convert the source bitmap to the right alpha type on a mismatch. We want to do this when converting from a
    // Bitmap to an ImmutableBitmap, since at that point we usually know the right alpha type to use in context.
    auto converted_bitmap = [&] -> NonnullRefPtr<Bitmap const> {
        if (bitmap->alpha_type() == alpha_type)
            return bitmap;
        auto new_bitmap = MUST(bitmap->clone());
        new_bitmap->set_alpha_type_destructive(alpha_type);
        return new_bitmap;
    }();

    return create(converted_bitmap, move(color_space));
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create_snapshot_from_painting_surface(NonnullRefPtr<PaintingSurface> const& painting_surface)
{
    auto bitmap = MUST(Bitmap::create(BitmapFormat::BGRA8888, AlphaType::Premultiplied, painting_surface->size()));
    painting_surface->read_into_bitmap(*bitmap);
    return create(bitmap);
}

ImmutableBitmap::ImmutableBitmap(NonnullRefPtr<Bitmap const> bitmap, ColorSpace color_space)
    : m_bitmap(move(bitmap))
    , m_color_space(move(color_space))
{
}

ImmutableBitmap::~ImmutableBitmap() = default;

}
