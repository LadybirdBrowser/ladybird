/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Function.h>
#include <AK/RefCounted.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/UnicodeRange.h>

namespace Gfx {

class FontCascadeList : public RefCounted<FontCascadeList> {
public:
    using SystemFontFallbackCallback = Function<RefPtr<Font const>(u32, Font const&)>;

    static NonnullRefPtr<FontCascadeList> create()
    {
        return adopt_ref(*new FontCascadeList());
    }

    size_t size() const { return m_fonts.size(); }
    bool is_empty() const { return m_fonts.is_empty() && m_pending_faces.is_empty() && !m_last_resort_font; }
    Font const& first() const { return !m_fonts.is_empty() ? *m_fonts.first().font : *m_last_resort_font; }

    template<typename Callback>
    void for_each_font_entry(Callback callback) const
    {
        for (auto const& font : m_fonts)
            callback(font);
    }

    void add(NonnullRefPtr<Font const> font);
    void add(NonnullRefPtr<Font const> font, Vector<UnicodeRange> unicode_ranges);

    // Register an unloaded face covering `unicode_ranges`. The cascade invokes
    // `start_load` the first time a rendered codepoint falls within one of the ranges.
    void add_pending_face(Vector<UnicodeRange> unicode_ranges, Function<void()> start_load);

    void extend(FontCascadeList const& other);

    // A pending-face fetch should only be initiated for codepoints that are actually
    // being shaped into glyph runs. Callers that merely probe the cascade (e.g. the
    // U+0020 check in "first available font" metrics) pass No so that probing does
    // not kick off downloads for subset faces that happen to cover the probe point.
    enum class TriggerPendingLoads : u8 {
        No,
        Yes,
    };
    Gfx::Font const& font_for_code_point(u32 code_point, TriggerPendingLoads = TriggerPendingLoads::No) const;

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

    class PendingFace : public RefCounted<PendingFace> {
    public:
        PendingFace(UnicodeRange enclosing, Vector<UnicodeRange> ranges, Function<void()> start_load)
            : m_enclosing_range(enclosing)
            , m_unicode_ranges(move(ranges))
            , m_start_load(move(start_load))
        {
        }

        bool covers(u32 code_point) const
        {
            if (!m_enclosing_range.contains(code_point))
                return false;
            for (auto const& range : m_unicode_ranges) {
                if (range.contains(code_point))
                    return true;
            }
            return false;
        }

        void start_load() { m_start_load(); }

    private:
        UnicodeRange m_enclosing_range;
        Vector<UnicodeRange> m_unicode_ranges;
        Function<void()> m_start_load;
    };

    void set_last_resort_font(NonnullRefPtr<Font> font) { m_last_resort_font = move(font); }
    void set_system_font_fallback_callback(SystemFontFallbackCallback callback) { m_system_font_fallback_callback = move(callback); }

    Font const& first_text_face() const
    {
        for (auto const& entry : m_fonts)
            if (!entry.font->is_emoji_font())
                return *entry.font;
        return first();
    }

private:
    RefPtr<Font const> m_last_resort_font;
    mutable Vector<Entry> m_fonts;
    mutable Vector<NonnullRefPtr<PendingFace>> m_pending_faces;
    SystemFontFallbackCallback m_system_font_fallback_callback;

    // OPTIMIZATION: Cache of resolved fonts for ASCII code points. Since m_fonts only grows and the cascade returns
    //               the first matching font, a cached hit can never become stale.
    mutable Array<Font const*, 128> m_ascii_cache {};
};

}
