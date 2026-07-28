/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/FontCascadeList.h>
#include <LibUnicode/CharacterTypes.h>

namespace Gfx {

EmojiPresentationResult emoji_presentation_for_code_point(u32 code_point, Optional<u32> next_code_point)
{
    // VARIATION SELECTOR-16 (emoji)
    if (next_code_point == 0xFE0Fu)
        return { EmojiPresentation::Emoji, ForcedPresentation::Yes };
    // VARIATION SELECTOR-15 (text)
    if (next_code_point == 0xFE0Eu)
        return { EmojiPresentation::Text, ForcedPresentation::Yes };

    if (Unicode::code_point_has_emoji_presentation_property(code_point))
        return { EmojiPresentation::Emoji, ForcedPresentation::No };
    return { EmojiPresentation::Text, ForcedPresentation::No };
}

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

void FontCascadeList::extend_fallback(FontCascadeList const& other)
{
    m_fallback_fonts.extend(other.m_fonts);
}

Gfx::Font const& FontCascadeList::font_for_code_point(u32 code_point, TriggerPendingLoads trigger_pending_loads, EmojiPresentationResult emoji_presentation) const
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

    auto use_ascii_cache = code_point < m_ascii_cache.size() && emoji_presentation.presentation == EmojiPresentation::Text && emoji_presentation.forced == ForcedPresentation::No;
    if (use_ascii_cache) {
        if (auto const* cached = m_ascii_cache[code_point])
            return *cached;
    }

    auto cache_and_return = [&](Font const& font) -> Font const& {
        if (use_ascii_cache)
            m_ascii_cache[code_point] = &font;
        return font;
    };

    auto presentation_matches = [wants_emoji = emoji_presentation.presentation == EmojiPresentation::Emoji](Font const& font) {
        return font.is_emoji_font() == wants_emoji;
    };

    auto entry_contains_glyph = [code_point](Entry const& entry) {
        if (!entry.range_data.has_value())
            return entry.font->contains_glyph(code_point);
        if (!entry.range_data->enclosing_range.contains(code_point))
            return false;
        for (auto const& range : entry.range_data->unicode_ranges) {
            if (range.contains(code_point) && entry.font->contains_glyph(code_point))
                return true;
        }
        return false;
    };

    Font const* author_glyph_match = nullptr;
    for (auto const& entry : m_fonts) {
        if (!entry_contains_glyph(entry))
            continue;
        if (emoji_presentation.forced == ForcedPresentation::No || presentation_matches(*entry.font))
            return cache_and_return(*entry.font);
        if (!author_glyph_match)
            author_glyph_match = entry.font.ptr();
    }

    Font const* fallback_glyph_match = nullptr;
    for (auto const& entry : m_fallback_fonts) {
        if (!entry_contains_glyph(entry))
            continue;
        if (presentation_matches(*entry.font))
            return cache_and_return(*entry.font);
        if (!fallback_glyph_match)
            fallback_glyph_match = entry.font.ptr();
    }

    if (m_system_font_fallback_callback) {
        if (auto fallback = m_system_font_fallback_callback(code_point, emoji_presentation.presentation, first())) {
            if (presentation_matches(*fallback) || (!author_glyph_match && !fallback_glyph_match)) {
                m_fallback_fonts.append({ fallback.release_nonnull(), {} });
                return cache_and_return(*m_fallback_fonts.last().font);
            }
        }
    }

    if (author_glyph_match)
        return cache_and_return(*author_glyph_match);
    if (fallback_glyph_match)
        return cache_and_return(*fallback_glyph_match);

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

extern "C" {
void const* ladybird_gfx_font_cascade_list_font_for_code_point(void const*, u32, bool, bool, bool);
void const* ladybird_gfx_font_cascade_list_first(void const*);
void ladybird_gfx_font_cascade_list_ref(void const*);
void ladybird_gfx_font_cascade_list_unref(void const*);
u8 ladybird_gfx_emoji_presentation_for_code_point(u32, u32, bool);
}

extern "C" void const* ladybird_gfx_font_cascade_list_font_for_code_point(void const* list, u32 code_point, bool trigger_pending_loads, bool emoji_presentation, bool forced_presentation)
{
    VERIFY(list);
    auto const& cascade_list = *static_cast<Gfx::FontCascadeList const*>(list);
    return &cascade_list.font_for_code_point(
        code_point,
        trigger_pending_loads ? Gfx::FontCascadeList::TriggerPendingLoads::Yes : Gfx::FontCascadeList::TriggerPendingLoads::No,
        { emoji_presentation ? Gfx::EmojiPresentation::Emoji : Gfx::EmojiPresentation::Text,
            forced_presentation ? Gfx::ForcedPresentation::Yes : Gfx::ForcedPresentation::No });
}

extern "C" void const* ladybird_gfx_font_cascade_list_first(void const* list)
{
    VERIFY(list);
    return &static_cast<Gfx::FontCascadeList const*>(list)->first();
}

extern "C" void ladybird_gfx_font_cascade_list_ref(void const* list)
{
    VERIFY(list);
    static_cast<Gfx::FontCascadeList const*>(list)->ref();
}

extern "C" void ladybird_gfx_font_cascade_list_unref(void const* list)
{
    VERIFY(list);
    static_cast<Gfx::FontCascadeList const*>(list)->unref();
}

extern "C" u8 ladybird_gfx_emoji_presentation_for_code_point(u32 code_point, u32 next_code_point, bool has_next_code_point)
{
    auto result = Gfx::emoji_presentation_for_code_point(
        code_point, has_next_code_point ? Optional<u32> { next_code_point } : Optional<u32> {});
    u8 encoded = 0;
    if (result.presentation == Gfx::EmojiPresentation::Emoji)
        encoded |= 1;
    if (result.forced == Gfx::ForcedPresentation::Yes)
        encoded |= 2;
    return encoded;
}
