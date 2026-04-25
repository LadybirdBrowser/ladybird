/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf8View.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/Layout/LayoutState.h>
#include <LibWeb/Layout/TextNode.h>
#include <ctype.h>

namespace Web::Layout {

LineBoxFragment::LineBoxFragment(Node const& layout_node, size_t start, size_t length_in_code_units,
    CSSPixels inline_offset, CSSPixels block_offset, CSSPixels inline_length, CSSPixels block_length,
    CSSPixels border_box_top, CSS::WritingMode writing_mode, RefPtr<Gfx::GlyphRun> glyph_run)
    : m_layout_node(layout_node)
    , m_start(start)
    , m_length_in_code_units(length_in_code_units)
    , m_inline_offset(inline_offset)
    , m_block_offset(block_offset)
    , m_inline_length(inline_length)
    , m_block_length(block_length)
    , m_border_box_top(border_box_top)
    , m_writing_mode(writing_mode)
    , m_glyph_run(move(glyph_run))
{
}

CSSPixelPoint LineBoxFragment::offset() const
{
    if (m_writing_mode != CSS::WritingMode::HorizontalTb)
        return { m_block_offset, m_inline_offset };
    return { m_inline_offset, m_block_offset };
}

CSSPixelSize LineBoxFragment::size() const
{
    if (m_writing_mode != CSS::WritingMode::HorizontalTb)
        return { m_block_length, m_inline_length };
    return { m_inline_length, m_block_length };
}

bool LineBoxFragment::ends_in_whitespace() const
{
    if (m_length_in_code_units == 0)
        return false;
    return is_ascii_space(text().code_unit_at(m_length_in_code_units - 1));
}

bool LineBoxFragment::is_justifiable_whitespace() const
{
    return text() == " "sv;
}

Utf16View LineBoxFragment::text() const
{
    if (auto* text_node = as_if<TextNode>(layout_node()))
        return text_node->text_for_rendering().substring_view(m_start, m_length_in_code_units);
    return {};
}

bool LineBoxFragment::is_atomic_inline() const
{
    return layout_node().is_atomic_inline();
}

void LineBoxFragment::append_glyph_run(RefPtr<Gfx::GlyphRun> const& glyph_run, CSSPixels run_width)
{
    auto& glyphs = m_glyph_run->glyphs();
    glyphs.ensure_capacity(glyphs.size() + glyph_run->glyphs().size());

    switch (glyph_run->direction()) {
    case Gfx::GlyphRun::Direction::Ltr: {
        auto inline_offset = m_inline_length.to_float();
        for (auto& glyph : glyph_run->glyphs()) {
            glyph.position.translate_by(inline_offset, 0);
            glyphs.unchecked_append(glyph);
        }
        break;
    }
    case Gfx::GlyphRun::Direction::Rtl: {
        auto run_offset = run_width.to_float();
        for (auto& glyph : m_glyph_run->glyphs()) {
            glyph.position.translate_by(run_offset, 0);
        }
        for (auto& glyph : glyph_run->glyphs()) {
            glyphs.unchecked_append(glyph);
        }
        break;
    }
    }
    m_inline_length += run_width;
}

}
