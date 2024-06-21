/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TextLayout.h"
#include "Font/Emoji.h"
#include <AK/Debug.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibUnicode/Emoji.h>

namespace Gfx {

DrawGlyphOrEmoji prepare_draw_glyph_or_emoji(FloatPoint point, Utf8CodePointIterator& it, Font const& font)
{
    u32 code_point = *it;
    auto next_code_point = it.peek(1);

    ScopeGuard consume_variation_selector = [&, initial_it = it] {
        static auto const variation_selector = Unicode::property_from_string("Variation_Selector"sv);
        if (!variation_selector.has_value())
            return;

        // If we advanced the iterator to consume an emoji sequence, don't look for another variation selector.
        if (initial_it != it)
            return;

        // Otherwise, discard one code point if it's a variation selector.
        if (next_code_point.has_value() && Unicode::code_point_has_property(*next_code_point, *variation_selector))
            ++it;
    };

    // NOTE: We don't check for emoji
    auto font_contains_glyph = font.contains_glyph(code_point);
    auto check_for_emoji = !font.has_color_bitmaps() && Unicode::could_be_start_of_emoji_sequence(it, font_contains_glyph ? Unicode::SequenceType::EmojiPresentation : Unicode::SequenceType::Any);

    // If the font contains the glyph, and we know it's not the start of an emoji, draw a text glyph.
    if (font_contains_glyph && !check_for_emoji) {
        return DrawGlyph {
            .position = point,
            .code_point = code_point,
            .font = font,
        };
    }

    // If we didn't find a text glyph, or have an emoji variation selector or regional indicator, try to draw an emoji glyph.
    if (auto const* emoji = Emoji::emoji_for_code_point_iterator(it)) {
        return DrawEmoji {
            .position = point,
            .emoji = emoji,
            .font = font,
        };
    }

    // If that failed, but we have a text glyph fallback, draw that.
    if (font_contains_glyph) {
        return DrawGlyph {
            .position = point,
            .code_point = code_point,
            .font = font,
        };
    }

    // No suitable glyph found, draw a replacement character.
    dbgln_if(EMOJI_DEBUG, "Failed to find a glyph or emoji for code_point {}", code_point);
    return DrawGlyph {
        .position = point,
        .code_point = 0xFFFD,
        .font = font,
    };
}

}
