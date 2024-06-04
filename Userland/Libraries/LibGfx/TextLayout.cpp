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

enum class BlockType {
    Newline,
    Whitespace,
    Word
};

struct Block {
    BlockType type;
    Utf8View characters;
};

FloatRect TextLayout::bounding_rect(TextWrapping wrapping) const
{
    auto lines = wrap_lines(TextElision::None, wrapping);
    if (lines.is_empty()) {
        return {};
    }

    FloatRect bounding_rect = {
        0, 0, 0, (static_cast<float>(lines.size()) * (m_font_metrics.ascent + m_font_metrics.descent + m_font_metrics.line_gap)) - m_font_metrics.line_gap
    };

    for (auto& line : lines) {
        auto line_width = m_font.width(line);
        if (line_width > bounding_rect.width())
            bounding_rect.set_width(line_width);
    }

    return bounding_rect;
}

Vector<ByteString, 32> TextLayout::wrap_lines(TextElision elision, TextWrapping wrapping) const
{
    Vector<Block> blocks;

    Optional<BlockType> current_block_type;
    size_t block_start_offset = 0;

    size_t offset = 0;
    for (auto it = m_text.begin(); !it.done(); ++it) {
        offset = m_text.iterator_offset(it);

        switch (*it) {
        case '\t':
        case ' ': {
            if (current_block_type.has_value() && current_block_type.value() != BlockType::Whitespace) {
                blocks.append({
                    current_block_type.value(),
                    m_text.substring_view(block_start_offset, offset - block_start_offset),
                });
                current_block_type.clear();
            }

            if (!current_block_type.has_value()) {
                current_block_type = BlockType::Whitespace;
                block_start_offset = offset;
            }

            continue;
        }
        case '\r':
            if (it.peek(1) == static_cast<u32>('\n'))
                ++it;
            [[fallthrough]];
        case '\n': {
            if (current_block_type.has_value()) {
                blocks.append({
                    current_block_type.value(),
                    m_text.substring_view(block_start_offset, offset - block_start_offset),
                });
                current_block_type.clear();
            }

            blocks.append({ BlockType::Newline, Utf8View {} });
            continue;
        }
        default: {
            if (current_block_type.has_value() && current_block_type.value() != BlockType::Word) {
                blocks.append({
                    current_block_type.value(),
                    m_text.substring_view(block_start_offset, offset - block_start_offset),
                });
                current_block_type.clear();
            }

            if (!current_block_type.has_value()) {
                current_block_type = BlockType::Word;
                block_start_offset = offset;
            }
        }
        }
    }

    if (current_block_type.has_value()) {
        blocks.append({
            current_block_type.value(),
            m_text.substring_view(block_start_offset, m_text.byte_length() - block_start_offset),
        });
    }

    Vector<ByteString> lines;
    StringBuilder builder;
    float line_width = 0;
    for (Block& block : blocks) {
        switch (block.type) {
        case BlockType::Newline: {
            lines.append(builder.to_byte_string());
            builder.clear();
            line_width = 0;
            continue;
        }
        case BlockType::Whitespace:
        case BlockType::Word: {
            float block_width = m_font.width(block.characters);
            // FIXME: This should look at the specific advance amount of the
            //        last character, but we don't support that yet.

            if (wrapping == TextWrapping::Wrap && line_width + block_width > m_rect.width()) {
                lines.append(builder.to_byte_string());
                builder.clear();
                line_width = 0;
            }

            builder.append(block.characters.as_string());
            line_width += block_width;
        }
        }
    }

    auto last_line = builder.to_byte_string();
    if (!last_line.is_empty())
        lines.append(last_line);

    switch (elision) {
    case TextElision::None:
        break;
    case TextElision::Right: {
        lines.at(lines.size() - 1) = elide_text_from_right(Utf8View { lines.at(lines.size() - 1) });
        break;
    }
    }

    return lines;
}

ByteString TextLayout::elide_text_from_right(Utf8View text) const
{
    float text_width = m_font.width(text);
    if (text_width > static_cast<float>(m_rect.width())) {
        float ellipsis_width = m_font.width("..."sv);
        float current_width = ellipsis_width;

        // FIXME: This code will break when the font has glyphs with advance
        //        amounts different from the actual width of the glyph
        //        (which is the case with many TrueType fonts).
        if (ellipsis_width < text_width) {
            size_t offset = 0;
            for (auto it = text.begin(); !it.done(); ++it) {
                auto glyph_width = m_font.glyph_or_emoji_width(it);
                // NOTE: Glyph spacing should not be added after the last glyph on the line,
                //       but since we are here because the last glyph does not actually fit on the line,
                //       we don't have to worry about spacing.
                auto width_with_this_glyph_included = current_width + glyph_width;
                if (width_with_this_glyph_included > m_rect.width())
                    break;
                current_width += glyph_width;
                offset = text.iterator_offset(it);
            }

            StringBuilder builder;
            builder.append(text.substring_view(0, offset).as_string());
            builder.append("..."sv);
            return builder.to_byte_string();
        }
    }

    return text.as_string();
}

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
