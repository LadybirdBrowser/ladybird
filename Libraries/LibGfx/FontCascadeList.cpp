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

void FontCascadeList::add_pending_face(Vector<UnicodeRange> unicode_ranges, Function<void()> start_load)
{
    if (unicode_ranges.is_empty())
        return;

    u32 lowest_code_point = 0xFFFFFFFF;
    u32 highest_code_point = 0;
    for (auto const& range : unicode_ranges) {
        lowest_code_point = min(lowest_code_point, range.min_code_point());
        highest_code_point = max(highest_code_point, range.max_code_point());
    }

    m_pending_faces.append(adopt_ref(*new PendingFace(
        UnicodeRange { lowest_code_point, highest_code_point },
        move(unicode_ranges),
        move(start_load))));
}

void FontCascadeList::extend(FontCascadeList const& other)
{
    m_fonts.extend(other.m_fonts);
    m_pending_faces.extend(other.m_pending_faces);
}

Gfx::Font const& FontCascadeList::font_for_code_point(u32 code_point, TriggerPendingLoads trigger_pending_loads) const
{
    // Only the text-shaping paths pass TriggerPendingLoads::Yes. Probes that don't
    // lead to a glyph being drawn (the U+0020 check used to compute first-available-
    // font metrics, for instance) skip this block so they can't initiate a download
    // for a subset face that happens to cover the probe codepoint.
    if (trigger_pending_loads == TriggerPendingLoads::Yes) {
        // Walk pending entries first: if this codepoint falls in an unloaded face's
        // unicode-range we kick off the fetch and drop the entry — a fallback font
        // that happens to cover the codepoint shouldn't prevent the real face from
        // loading. FontComputer::clear_computed_font_cache() rebuilds the cascade
        // once the fetch completes, so later shapes pick up the loaded face. Run
        // before the ASCII cache lookup so a previously-cached codepoint still
        // triggers a newly-added face.
        m_pending_faces.remove_all_matching([code_point](auto const& pending) {
            if (!pending->covers(code_point))
                return false;
            pending->start_load();
            return true;
        });
    }

    if (code_point < m_ascii_cache.size()) {
        if (auto const* cached = m_ascii_cache[code_point])
            return *cached;
    }

    auto cache_and_return = [&](Font const& font) -> Font const& {
        if (code_point < m_ascii_cache.size())
            m_ascii_cache[code_point] = &font;
        return font;
    };

    for (auto const& entry : m_fonts) {
        if (entry.range_data.has_value()) {
            if (!entry.range_data->enclosing_range.contains(code_point))
                continue;
            for (auto const& range : entry.range_data->unicode_ranges) {
                if (range.contains(code_point) && entry.font->contains_glyph(code_point))
                    return cache_and_return(*entry.font);
            }
        } else if (entry.font->contains_glyph(code_point)) {
            return cache_and_return(*entry.font);
        }
    }

    if (m_system_font_fallback_callback) {
        if (auto fallback = m_system_font_fallback_callback(code_point, first())) {
            m_fonts.append({ fallback.release_nonnull(), {} });
            return cache_and_return(*m_fonts.last().font);
        }
    }

    return cache_and_return(*m_last_resort_font);
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

}
