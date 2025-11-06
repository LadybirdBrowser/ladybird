/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/UnicodeRange.h>

namespace Gfx {

class FontCascadeList : public RefCounted<FontCascadeList> {
public:
    static NonnullRefPtr<FontCascadeList> create()
    {
        return adopt_ref(*new FontCascadeList());
    }

    size_t size() const { return m_fonts.size(); }
    bool is_empty() const { return m_fonts.is_empty() && !m_last_resort_font; }
    Font const& first() const { return !m_fonts.is_empty() ? *m_fonts.first().font : *m_last_resort_font; }

    template<typename Callback>
    void for_each_font_entry(Callback callback) const
    {
        for (auto const& font : m_fonts)
            callback(font);
    }

    void add(NonnullRefPtr<Font const> font);
    void add(NonnullRefPtr<Font const> font, Vector<UnicodeRange> unicode_ranges);

    void extend(FontCascadeList const& other);

    Gfx::Font const& font_for_code_point(u32 code_point) const;

    bool equals(FontCascadeList const& other) const;

    struct Entry {
        NonnullRefPtr<Font const> font;
        struct RangeData {
            // The enclosing range is the union of all Unicode ranges. Used for fast skipping.
            UnicodeRange enclosing_range;

            Vector<UnicodeRange> unicode_ranges;
        };
        Optional<RangeData> range_data;
    };

    void set_last_resort_font(NonnullRefPtr<Font> font) { m_last_resort_font = move(font); }

    Font const& first_text_face() const
    {
        for (auto const& entry : m_fonts)
            if (!entry.font->is_emoji_font())
                return *entry.font;
        return first();
    }

private:
    RefPtr<Font const> m_last_resort_font;
    Vector<Entry> m_fonts;
};

}
