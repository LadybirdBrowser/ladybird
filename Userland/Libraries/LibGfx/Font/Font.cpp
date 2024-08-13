/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <harfbuzz/hb.h>

namespace Gfx {

Font::~Font()
{
    if (m_harfbuzz_font)
        hb_font_destroy(m_harfbuzz_font);
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

hb_font_t* Font::harfbuzz_font() const
{
    if (!m_harfbuzz_font) {
        m_harfbuzz_font = hb_font_create(typeface().harfbuzz_typeface());
        hb_font_set_scale(m_harfbuzz_font, pixel_size() * text_shaping_resolution, pixel_size() * text_shaping_resolution);
        hb_font_set_ptem(m_harfbuzz_font, point_size());
    }
    return m_harfbuzz_font;
}

}
