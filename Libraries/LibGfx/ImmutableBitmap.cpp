/*
 * Copyright (c) 2023-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>

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

RefPtr<Gfx::Bitmap const> ImmutableBitmap::bitmap() const
{
    return m_bitmap;
}

Color ImmutableBitmap::get_pixel(int x, int y) const
{
    return m_bitmap->get_pixel(x, y);
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap const> const& bitmap)
{
    return adopt_ref(*new ImmutableBitmap(bitmap));
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap const> const& bitmap, AlphaType alpha_type)
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

    return create(converted_bitmap);
}

ImmutableBitmap::ImmutableBitmap(NonnullRefPtr<Bitmap const> bitmap)
    : m_bitmap(move(bitmap))
{
}

ImmutableBitmap::~ImmutableBitmap() = default;

}
