/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Utf8View.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LineBox.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web::Layout {

CSSPixels LineBox::width() const
{
    if (m_writing_mode != CSS::WritingMode::HorizontalTb)
        return m_block_length;
    return m_inline_length;
}

CSSPixels LineBox::height() const
{
    if (m_writing_mode != CSS::WritingMode::HorizontalTb)
        return m_inline_length;
    return m_block_length;
}

CSSPixels LineBox::bottom() const
{
    if (m_writing_mode != CSS::WritingMode::HorizontalTb)
        return m_inline_length;
    return m_bottom;
}

void LineBox::add_fragment(Node const& layout_node, int start, int length, CSSPixels leading_size, CSSPixels trailing_size, CSSPixels leading_margin, CSSPixels trailing_margin, CSSPixels content_width, CSSPixels content_height, CSSPixels border_box_top, CSSPixels border_box_bottom, RefPtr<Gfx::GlyphRun> glyph_run)
{
    bool text_align_is_justify = layout_node.computed_values().text_align() == CSS::TextAlign::Justify;
    if (glyph_run && !text_align_is_justify && !m_fragments.is_empty() && &m_fragments.last().layout_node() == &layout_node && &m_fragments.last().m_glyph_run->font() == &glyph_run->font()) {
        // The fragment we're adding is from the last Layout::Node on the line.
        // Expand the last fragment instead of adding a new one with the same Layout::Node.
        m_fragments.last().m_length = (start - m_fragments.last().m_start) + length;
        m_fragments.last().append_glyph_run(glyph_run, content_width);
    } else {
        CSSPixels inline_offset = leading_margin + leading_size + m_inline_length;
        CSSPixels block_offset = 0;
        m_fragments.append(LineBoxFragment { layout_node, start, length, inline_offset, block_offset, content_width, content_height, border_box_top, m_direction, m_writing_mode, move(glyph_run) });
    }
    m_inline_length += leading_margin + leading_size + content_width + trailing_size + trailing_margin;
    m_block_length = max(m_block_length, content_height + border_box_top + border_box_bottom);
}

CSSPixels LineBox::calculate_or_trim_trailing_whitespace(RemoveTrailingWhitespace should_remove)
{
    auto should_trim = [](LineBoxFragment* fragment) {
        auto ws = fragment->layout_node().computed_values().white_space();
        return ws == CSS::WhiteSpace::Normal || ws == CSS::WhiteSpace::Nowrap || ws == CSS::WhiteSpace::PreLine;
    };

    CSSPixels whitespace_width = 0;
    LineBoxFragment* last_fragment = nullptr;
    size_t fragment_index = m_fragments.size();
    for (;;) {
        if (fragment_index == 0)
            return whitespace_width;

        last_fragment = &m_fragments[--fragment_index];
        auto const* dom_node = last_fragment->layout_node().dom_node();
        if (dom_node) {
            auto cursor_position = dom_node->document().cursor_position();
            if (cursor_position && cursor_position->node() == dom_node)
                return whitespace_width;
        }
        if (!should_trim(last_fragment))
            return whitespace_width;
        if (!last_fragment->is_justifiable_whitespace())
            break;

        whitespace_width += last_fragment->inline_length();
        if (should_remove == RemoveTrailingWhitespace::Yes) {
            m_inline_length -= last_fragment->inline_length();
            m_fragments.remove(fragment_index);
        }
    }

    auto last_text = last_fragment->text();
    if (last_text.is_null())
        return whitespace_width;

    size_t last_fragment_length = last_fragment->length();
    while (last_fragment_length) {
        auto last_character = last_text[--last_fragment_length];
        if (!is_ascii_space(last_character))
            break;

        auto const& font = last_fragment->glyph_run() ? last_fragment->glyph_run()->font() : last_fragment->layout_node().first_available_font();
        int last_character_width = font.glyph_width(last_character);
        whitespace_width += last_character_width;
        if (should_remove == RemoveTrailingWhitespace::Yes) {
            last_fragment->m_length -= 1;
            last_fragment->set_inline_length(last_fragment->inline_length() - last_character_width);
            m_inline_length -= last_character_width;
        }
    }

    return whitespace_width;
}

CSSPixels LineBox::get_trailing_whitespace_width() const
{
    return const_cast<LineBox&>(*this).calculate_or_trim_trailing_whitespace(RemoveTrailingWhitespace::No);
}

void LineBox::trim_trailing_whitespace()
{
    calculate_or_trim_trailing_whitespace(RemoveTrailingWhitespace::Yes);
}

bool LineBox::is_empty_or_ends_in_whitespace() const
{
    if (m_fragments.is_empty())
        return true;

    return m_fragments.last().ends_in_whitespace();
}

}
