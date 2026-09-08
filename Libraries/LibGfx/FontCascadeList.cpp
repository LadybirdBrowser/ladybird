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
    m_first_available_font_cache = nullptr;
    m_fonts.append({ move(font), {} });
}

void FontCascadeList::add(NonnullRefPtr<Font const> font, Vector<UnicodeRange> unicode_ranges)
{
    m_first_available_font_cache = nullptr;
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

void FontCascadeList::add_pending_face(Vector<UnicodeRange> unicode_ranges, Function<PendingFontState()> resolve)
{
    m_ascii_cache.fill(nullptr);
    if (unicode_ranges.is_empty())
        return;

    u32 lowest_code_point = 0xFFFFFFFF;
    u32 highest_code_point = 0;
    for (auto const& range : unicode_ranges) {
        lowest_code_point = min(lowest_code_point, range.min_code_point());
        highest_code_point = max(highest_code_point, range.max_code_point());
    }

    m_pending_faces.append({ m_fonts.size(), adopt_ref(*new PendingFace(UnicodeRange { lowest_code_point, highest_code_point }, move(unicode_ranges), move(resolve))) });
}

void FontCascadeList::extend(FontCascadeList const& other)
{
    m_ascii_cache.fill(nullptr);
    m_first_available_font_cache = nullptr;
    for (auto const& pending : other.m_pending_faces)
        m_pending_faces.append({ m_fonts.size() + pending.font_index, pending.face });
    m_fonts.extend(other.m_fonts);
}

void FontCascadeList::extend_fallback(FontCascadeList const& other)
{
    m_fallback_fonts.extend(other.m_fonts);
}

// https://drafts.csswg.org/css-fonts/#first-available-font
Gfx::Font const& FontCascadeList::first_available_font() const
{
    if (m_first_available_font_cache)
        return *m_first_available_font_cache;

    // The first available font, used for example in the definition of font-relative lengths such as ex or in the
    // definition of the line-height property, is defined to be the first font for which the character U+0020 (space)
    // is not excluded by a unicode-range, given the font families in the font-family list (or a user agent’s default
    // font if none are available).
    static constexpr u32 space_code_point = 0x20;

    for (auto const& entry : m_fonts) {
        if (!entry.range_data.has_value()) {
            m_first_available_font_cache = entry.font.ptr();
            return *m_first_available_font_cache;
        }
        if (!entry.range_data->enclosing_range.contains(space_code_point))
            continue;

        for (auto const& range : entry.range_data->unicode_ranges) {
            if (range.contains(space_code_point)) {
                m_first_available_font_cache = entry.font.ptr();
                return *m_first_available_font_cache;
            }
        }
    }

    m_first_available_font_cache = m_last_resort_font.ptr();
    return *m_first_available_font_cache;
}

Gfx::Font const& FontCascadeList::font_for_code_point(u32 code_point, EmojiPresentationResult emoji_presentation) const
{
    auto use_ascii_cache = code_point < m_ascii_cache.size() && emoji_presentation.presentation == EmojiPresentation::Text && emoji_presentation.forced == ForcedPresentation::No;
    if (use_ascii_cache) {
        if (auto const* cached = m_ascii_cache[code_point])
            return *cached;
    }

    bool invisible = false;
    auto cache_and_return = [&](Font const& font) -> Font const& {
        auto const* selected_font = &font;
        if (invisible) {
            // https://drafts.csswg.org/css-fonts-4/#invisible-fallback
            // Create an anonymous font face with the same metrics as the selected font face
            // but with all glyphs "invisible" (containing no "ink"), and use that for rendering text.
            selected_font = m_invisible_fonts.ensure(&font, [&] { return font.invisible_variant(); }).ptr();
        }
        if (use_ascii_cache)
            m_ascii_cache[code_point] = selected_font;
        return *selected_font;
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

    size_t pending_index = 0;
    bool rendering_with_fallback = false;
    auto resolve_pending_faces = [&](size_t font_index) {
        while (!rendering_with_fallback && pending_index < m_pending_faces.size()
            && m_pending_faces[pending_index].font_index <= font_index) {
            auto const& pending = m_pending_faces[pending_index++].face;
            if (!pending->covers(code_point))
                continue;
            auto state = pending->resolve();
            if (state == PendingFontState::Failed)
                continue;
            invisible = state == PendingFontState::Invisible;
            // https://drafts.csswg.org/css-fonts-4/#font-display-timeline
            // Doing this must not trigger loads of any of the fallback fonts.
            rendering_with_fallback = true;
        }
    };

    Font const* author_glyph_match = nullptr;
    for (size_t font_index = 0; font_index < m_fonts.size(); ++font_index) {
        resolve_pending_faces(font_index);
        auto const& entry = m_fonts[font_index];
        if (!entry_contains_glyph(entry))
            continue;
        if (emoji_presentation.forced == ForcedPresentation::No || presentation_matches(*entry.font))
            return cache_and_return(*entry.font);
        if (!author_glyph_match)
            author_glyph_match = entry.font.ptr();
    }

    resolve_pending_faces(m_fonts.size());

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
    if (!m_pending_faces.is_empty() || !other.m_pending_faces.is_empty())
        return false;
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
void const* ladybird_gfx_font_cascade_list_font_for_code_point(void const*, u32, bool, bool);
void ladybird_gfx_font_cascade_list_ref(void const*);
void ladybird_gfx_font_cascade_list_unref(void const*);
u8 ladybird_gfx_emoji_presentation_for_code_point(u32, u32, bool);
}

extern "C" void const* ladybird_gfx_font_cascade_list_font_for_code_point(void const* list, u32 code_point, bool emoji_presentation, bool forced_presentation)
{
    VERIFY(list);
    auto const& cascade_list = *static_cast<Gfx::FontCascadeList const*>(list);
    return &cascade_list.font_for_code_point(
        code_point,
        { emoji_presentation ? Gfx::EmojiPresentation::Emoji : Gfx::EmojiPresentation::Text,
            forced_presentation ? Gfx::ForcedPresentation::Yes : Gfx::ForcedPresentation::No });
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
