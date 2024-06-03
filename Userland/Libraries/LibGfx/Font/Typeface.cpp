/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/ScaledFont.h>
#include <LibGfx/Font/Typeface.h>

namespace Gfx {

unsigned Typeface::weight() const
{
    return m_vector_font->weight();
}

unsigned Typeface::width() const
{
    return m_vector_font->width();
}

u8 Typeface::slope() const
{
    return m_vector_font->slope();
}

bool Typeface::is_fixed_width() const
{
    return m_vector_font->is_fixed_width();
}

void Typeface::set_vector_font(RefPtr<VectorFont> font)
{
    m_vector_font = move(font);
}

RefPtr<Font> Typeface::get_font(float point_size) const
{
    VERIFY(point_size >= 0);
    return m_vector_font->scaled_font(point_size);
}

}
