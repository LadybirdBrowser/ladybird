/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/TypeCasts.h>
#include <AK/Utf8View.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/BreakNode.h>
#include <LibWeb/Layout/LineBox.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web::Layout {

void LineBox::add_fragment(Node const& layout_node, int start, int length, CSSPixels leading_size, CSSPixels trailing_size, CSSPixels leading_margin, CSSPixels trailing_margin, CSSPixels content_width, CSSPixels content_height, CSSPixels border_box_top, CSSPixels border_box_bottom, RefPtr<Gfx::GlyphRun> glyph_run)
{
    bool text_align_is_justify = layout_node.computed_values().text_align() == CSS::TextAlign::Justify;
    if (glyph_run && !text_align_is_justify && !m_fragments.is_empty() && &m_fragments.last().layout_node() == &layout_node && &m_fragments.last().m_glyph_run->font() == &glyph_run->font()) {
        // The fragment we're adding is from the last Layout::Node on the line.
        // Expand the last fragment instead of adding a new one with the same Layout::Node.
        m_fragments.last().m_length = (start - m_fragments.last().m_start) + length;
        m_fragments.last().append_glyph_run(glyph_run, content_width);
    } else {
        CSSPixels x_offset = leading_margin + leading_size + m_width;
        CSSPixels y_offset = 0;
        m_fragments.append(LineBoxFragment { layout_node, start, length, CSSPixelPoint(x_offset, y_offset), CSSPixelSize(content_width, content_height), border_box_top, m_direction, move(glyph_run) });
    }
    m_width += leading_margin + leading_size + content_width + trailing_size + trailing_margin;
    m_height = max(m_height, content_height + border_box_top + border_box_bottom);
}

void LineBox::trim_trailing_whitespace()
{
    auto should_trim = [](LineBoxFragment* fragment) {
        auto ws = fragment->layout_node().computed_values().white_space();
        return ws == CSS::WhiteSpace::Normal || ws == CSS::WhiteSpace::Nowrap || ws == CSS::WhiteSpace::PreLine;
    };

    LineBoxFragment* last_fragment = nullptr;
    for (;;) {
        if (m_fragments.is_empty())
            return;
        // last_fragment cannot be null from here on down, as m_fragments is not empty.
        last_fragment = &m_fragments.last();
        auto dom_node = last_fragment->layout_node().dom_node();
        if (dom_node && dom_node->is_editable() && dom_node->document().cursor_position())
            return;
        if (!should_trim(last_fragment))
            return;
        if (last_fragment->is_justifiable_whitespace()) {
            m_width -= last_fragment->width();
            m_fragments.remove(m_fragments.size() - 1);
        } else {
            break;
        }
    }

    auto last_text = last_fragment->text();
    if (last_text.is_null())
        return;

    while (last_fragment->length()) {
        auto last_character = last_text[last_fragment->length() - 1];
        if (!is_ascii_space(last_character))
            break;

        // FIXME: Use fragment's glyph run to determine the width of the last character.
        int last_character_width = last_fragment->layout_node().first_available_font().glyph_width(last_character);
        last_fragment->m_length -= 1;
        last_fragment->set_width(last_fragment->width() - last_character_width);
        m_width -= last_character_width;
    }
}

bool LineBox::is_empty_or_ends_in_whitespace() const
{
    if (m_fragments.is_empty())
        return true;

    return m_fragments.last().ends_in_whitespace();
}

}
