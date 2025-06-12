/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf8View.h>
#include <LibWeb/Layout/LayoutState.h>
#include <LibWeb/Layout/TextNode.h>
#include <ctype.h>

namespace Web::Layout {

LineBoxFragment::LineBoxFragment(Node const& layout_node, int start, int length, CSSPixels inline_offset, CSSPixels block_offset, CSSPixels inline_length, CSSPixels block_length, CSSPixels border_box_top, CSS::Direction direction, CSS::WritingMode writing_mode, RefPtr<Gfx::GlyphRun> glyph_run)
    : m_layout_node(layout_node)
    , m_start(start)
    , m_length(length)
    , m_inline_offset(inline_offset)
    , m_block_offset(block_offset)
    , m_inline_length(inline_length)
    , m_block_length(block_length)
    , m_border_box_top(border_box_top)
    , m_direction(direction)
    , m_writing_mode(writing_mode)
    , m_glyph_run(move(glyph_run))
{
    if (m_glyph_run) {
        m_current_insert_direction = resolve_glyph_run_direction(m_glyph_run->text_type());
        if (m_direction == CSS::Direction::Rtl)
            m_insert_position = m_inline_length.to_float();
    }
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
    auto text = this->text();
    if (text.is_empty())
        return false;
    return isspace(text[text.length() - 1]);
}

bool LineBoxFragment::is_justifiable_whitespace() const
{
    return text() == " ";
}

StringView LineBoxFragment::text() const
{
    if (!is<TextNode>(layout_node()))
        return {};
    return as<TextNode>(layout_node()).text_for_rendering().bytes_as_string_view().substring_view(m_start, m_length);
}

bool LineBoxFragment::is_atomic_inline() const
{
    return layout_node().is_replaced_box() || (layout_node().display().is_inline_outside() && !layout_node().display().is_flow_inside());
}

CSS::Direction LineBoxFragment::resolve_glyph_run_direction(Gfx::GlyphRun::TextType text_type) const
{
    switch (text_type) {
    case Gfx::GlyphRun::TextType::Common:
    case Gfx::GlyphRun::TextType::ContextDependent:
    case Gfx::GlyphRun::TextType::EndPadding:
        return m_direction;
    case Gfx::GlyphRun::TextType::Ltr:
        return CSS::Direction::Ltr;
    case Gfx::GlyphRun::TextType::Rtl:
        return CSS::Direction::Rtl;
    default:
        VERIFY_NOT_REACHED();
    }
}

void LineBoxFragment::append_glyph_run(RefPtr<Gfx::GlyphRun> const& glyph_run, CSSPixels run_width)
{
    switch (m_direction) {
    case CSS::Direction::Ltr:
        append_glyph_run_ltr(glyph_run, run_width);
        break;
    case CSS::Direction::Rtl:
        append_glyph_run_rtl(glyph_run, run_width);
        break;
    }
}

void LineBoxFragment::append_glyph_run_ltr(RefPtr<Gfx::GlyphRun> const& glyph_run, CSSPixels run_width)
{
    auto run_direction = resolve_glyph_run_direction(glyph_run->text_type());

    if (m_current_insert_direction != run_direction) {
        if (run_direction == CSS::Direction::Rtl)
            m_insert_position = m_inline_length.to_float();
        m_current_insert_direction = run_direction;
    }

    switch (run_direction) {
    case CSS::Direction::Ltr:
        for (auto& glyph : glyph_run->glyphs()) {
            glyph.position.translate_by(m_inline_length.to_float(), 0);
            m_glyph_run->append(glyph);
        }
        break;
    case CSS::Direction::Rtl:
        for (auto& glyph : m_glyph_run->glyphs()) {
            if (glyph.position.x() >= m_insert_position)
                glyph.position.translate_by(run_width.to_float(), 0);
        }
        for (auto& glyph : glyph_run->glyphs()) {
            glyph.position.translate_by(m_insert_position, 0);
            m_glyph_run->append(glyph);
        }
        break;
    }

    m_inline_length += run_width;
}

void LineBoxFragment::append_glyph_run_rtl(RefPtr<Gfx::GlyphRun> const& glyph_run, CSSPixels run_width)
{
    auto run_direction = resolve_glyph_run_direction(glyph_run->text_type());

    if (m_current_insert_direction != run_direction) {
        if (run_direction == CSS::Direction::Ltr)
            m_insert_position = 0;
        m_current_insert_direction = run_direction;
    }

    switch (run_direction) {
    case CSS::Direction::Ltr:
        for (auto& glyph : m_glyph_run->glyphs()) {
            if (glyph.position.x() >= m_insert_position)
                glyph.position.translate_by(run_width.to_float(), 0);
        }
        for (auto& glyph : glyph_run->glyphs()) {
            glyph.position.translate_by(m_insert_position, 0);
            m_glyph_run->append(glyph);
        }
        break;
    case CSS::Direction::Rtl:
        if (glyph_run->text_type() != Gfx::GlyphRun::TextType::EndPadding) {
            for (auto& glyph : m_glyph_run->glyphs()) {
                glyph.position.translate_by(run_width.to_float(), 0);
            }
        }
        for (auto& glyph : glyph_run->glyphs()) {
            m_glyph_run->append(glyph);
        }
        break;
    }

    m_inline_length += run_width;
    m_insert_position += run_width.to_float();
}

}
