/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>

namespace Gfx {

GlyphRasterPosition GlyphRasterPosition::get_nearest_fit_for(FloatPoint position)
{
    constexpr auto subpixel_divisions = GlyphSubpixelOffset::subpixel_divisions();
    auto fit = [](float pos, int& blit_pos, u8& subpixel_offset) {
        blit_pos = floorf(pos);
        subpixel_offset = round_to<u8>((pos - blit_pos) * subpixel_divisions);
        if (subpixel_offset >= subpixel_divisions) {
            blit_pos += 1;
            subpixel_offset = 0;
        }
    };
    int blit_x, blit_y;
    u8 subpixel_x, subpixel_y;
    fit(position.x(), blit_x, subpixel_x);
    fit(position.y(), blit_y, subpixel_y);
    return GlyphRasterPosition { { blit_x, blit_y }, { subpixel_x, subpixel_y } };
}

Font const& Font::bold_variant() const
{
    if (m_bold_variant)
        return *m_bold_variant;
    m_bold_variant = Gfx::FontDatabase::the().get(family(), point_size(), 700, Gfx::FontWidth::Normal, 0);
    if (!m_bold_variant)
        m_bold_variant = this;
    return *m_bold_variant;
}

}
