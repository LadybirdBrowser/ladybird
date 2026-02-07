/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/FontCascadeList.h>

namespace Gfx {

void FontCascadeList::add(NonnullRefPtr<Font const> font)
{
    m_fonts.append({ move(font), {} });
}

void FontCascadeList::add(NonnullRefPtr<Font const> font, Vector<UnicodeRange> unicode_ranges)
{
    if (unicode_ranges.is_empty()) {
        m_fonts.append({ move(font), {} });
        return;
    }
    u32 lowest_code_point = 0xFFFFFFFF;
    u32 highest_code_point = 0;

    for (auto& range : unicode_ranges) {
        lowest_code_point = min(lowest_code_point, range.min_code_point());
        highest_code_point = max(highest_code_point, range.max_code_point());
    }

    m_fonts.append({ move(font),
        Entry::RangeData {
            { lowest_code_point, highest_code_point },
            move(unicode_ranges),
        } });
}

void FontCascadeList::extend(FontCascadeList const& other)
{
    m_fonts.extend(other.m_fonts);
}

Gfx::Font const& FontCascadeList::font_for_code_point(u32 code_point) const
{
    for (auto const& entry : m_fonts) {
        if (entry.range_data.has_value()) {
            if (!entry.range_data->enclosing_range.contains(code_point))
                continue;
            for (auto const& range : entry.range_data->unicode_ranges) {
                if (range.contains(code_point) && entry.font->contains_glyph(code_point))
                    return entry.font;
            }
        } else if (entry.font->contains_glyph(code_point)) {
            return entry.font;
        }
    }

    if (m_system_font_fallback_callback) {
        if (auto fallback = m_system_font_fallback_callback(code_point, first())) {
            m_fonts.append({ fallback.release_nonnull(), {} });
            return *m_fonts.last().font;
        }
    }

    return *m_last_resort_font;
}

bool FontCascadeList::equals(FontCascadeList const& other) const
{
    if (m_fonts.size() != other.m_fonts.size())
        return false;
    for (size_t i = 0; i < m_fonts.size(); ++i) {
        if (m_fonts[i].font != other.m_fonts[i].font)
            return false;
    }
    return true;
}

u32 FontCascadeList::password_mask_character() const
{
    return m_cached_password_mask_character.ensure([&] {
        // Check for available masking characters in order of preference:
        // Preferred: U+25CF BLACK CIRCLE (●)
        // Fallback 1: U+2022 BULLET (•) - this has wider support
        // Fallback 2: U+002A ASTERISK (*) - available in all fonts
        constexpr u32 black_circle = 0x25CF;
        constexpr u32 bullet = 0x2022;
        constexpr u32 asterisk = '*';

        if (!font_for_code_point(black_circle).contains_glyph(black_circle)) {
            if (font_for_code_point(bullet).contains_glyph(bullet))
                return bullet;
            return asterisk;
        }

        return black_circle;
    });
}

}
