/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
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
    bool is_empty() const { return m_fonts.is_empty(); }
    Font const& first() const { return *m_fonts.first().font; }

    template<typename Callback>
    void for_each_font_entry(Callback callback) const
    {
        for (auto const& font : m_fonts)
            callback(font);
    }

    void add(NonnullRefPtr<Font> font);
    void add(NonnullRefPtr<Font> font, Vector<UnicodeRange> unicode_ranges);

    void extend(FontCascadeList const& other);

    Gfx::Font const& font_for_code_point(u32 code_point) const;

    bool equals(FontCascadeList const& other) const;

    struct Entry {
        NonnullRefPtr<Font> font;
        Optional<Vector<UnicodeRange>> unicode_ranges;
    };

private:
    Vector<Entry> m_fonts;
};

}
