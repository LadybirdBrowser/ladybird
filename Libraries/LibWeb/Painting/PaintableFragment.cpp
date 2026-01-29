/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Range.h>
#include <LibWeb/GraphemeEdgeTracker.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/TextPaintable.h>

namespace Web::Painting {

PaintableFragment::PaintableFragment(Layout::LineBoxFragment const& fragment)
    : m_layout_node(fragment.layout_node())
    , m_offset(fragment.offset())
    , m_size(fragment.size())
    , m_start_offset(fragment.start())
    , m_length_in_code_units(fragment.length_in_code_units())
    , m_glyph_run(fragment.glyph_run())
    , m_baseline(fragment.baseline())
    , m_writing_mode(fragment.writing_mode())
    , m_has_trailing_whitespace(fragment.has_trailing_whitespace())
{
}

CSSPixelRect const PaintableFragment::absolute_rect() const
{
    CSSPixelRect rect { {}, size() };
    if (auto const* containing_block = paintable().containing_block())
        rect.set_location(containing_block->absolute_position());
    rect.translate_by(offset());
    return rect;
}

size_t PaintableFragment::index_in_node_for_point(CSSPixelPoint position) const
{
    if (!is<TextPaintable>(paintable()))
        return 0;

    auto relative_inline_offset = [&] {
        switch (orientation()) {
        case Orientation::Horizontal:
            return (position.x() - absolute_rect().x()).to_float();
        case Orientation::Vertical:
            return (position.y() - absolute_rect().y()).to_float();
        }
        VERIFY_NOT_REACHED();
    }();
    if (relative_inline_offset < 0)
        return 0;

    GraphemeEdgeTracker tracker { relative_inline_offset };

    for (auto const& glyph : m_glyph_run->glyphs()) {
        if (tracker.update(glyph.length_in_code_units, glyph.glyph_width) == IterationDecision::Break)
            break;
    }

    return m_start_offset + tracker.resolve();
}

CSSPixelRect PaintableFragment::range_rect(Paintable::SelectionState selection_state, size_t start_offset_in_code_units, size_t end_offset_in_code_units) const
{
    auto const start_index = m_start_offset;
    auto const end_index = m_start_offset + m_length_in_code_units;

    // Determine selection bounds and check for early exit.
    size_t selection_start_in_this_fragment;
    size_t selection_end_in_this_fragment;
    bool include_trailing_whitespace;

    switch (selection_state) {
    case Paintable::SelectionState::Full:
        include_trailing_whitespace = true;
        break;
    case Paintable::SelectionState::StartAndEnd:
    case Paintable::SelectionState::None:
        if (start_index > end_offset_in_code_units || end_index < start_offset_in_code_units)
            return {};
        selection_start_in_this_fragment = max(0, start_offset_in_code_units - m_start_offset);
        selection_end_in_this_fragment = min(m_length_in_code_units, end_offset_in_code_units - m_start_offset);
        include_trailing_whitespace = selection_end_in_this_fragment == m_length_in_code_units;
        break;
    case Paintable::SelectionState::Start:
        if (end_index < start_offset_in_code_units)
            return {};
        selection_start_in_this_fragment = max(0, start_offset_in_code_units - m_start_offset);
        selection_end_in_this_fragment = m_length_in_code_units;
        include_trailing_whitespace = true;
        break;
    case Paintable::SelectionState::End:
        if (start_index > end_offset_in_code_units)
            return {};
        selection_start_in_this_fragment = 0;
        selection_end_in_this_fragment = min(end_offset_in_code_units - m_start_offset, m_length_in_code_units);
        include_trailing_whitespace = selection_end_in_this_fragment == m_length_in_code_units;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    auto rect = absolute_rect();
    auto const& font = glyph_run() ? glyph_run()->font() : layout_node().first_available_font();

    CSSPixels pixel_offset;
    CSSPixels pixel_width;

    // For Full selection, use the rect's existing dimensions rather than recalculating from text.
    if (selection_state == Paintable::SelectionState::Full) {
        pixel_offset = 0;
        pixel_width = rect.primary_size_for_orientation(orientation());
    } else {
        pixel_offset = CSSPixels::nearest_value_for(font.width(text().substring_view(0, selection_start_in_this_fragment)));

        // When start equals end, this is a cursor position.
        if (start_offset_in_code_units == end_offset_in_code_units) {
            pixel_width = 1;
        } else {
            pixel_width = CSSPixels::nearest_value_for(font.width(text().substring_view(selection_start_in_this_fragment,
                selection_end_in_this_fragment - selection_start_in_this_fragment)));
        }
    }

    if (m_has_trailing_whitespace && include_trailing_whitespace)
        pixel_width += CSSPixels::nearest_value_for(font.glyph_width(' '));

    rect.set_primary_offset_for_orientation(orientation(), rect.primary_offset_for_orientation(orientation()) + pixel_offset);
    rect.set_primary_size_for_orientation(orientation(), pixel_width);
    return rect;
}

Gfx::Orientation PaintableFragment::orientation() const
{
    switch (m_writing_mode) {
    case CSS::WritingMode::HorizontalTb:
        return Gfx::Orientation::Horizontal;
    case CSS::WritingMode::VerticalRl:
    case CSS::WritingMode::VerticalLr:
    case CSS::WritingMode::SidewaysRl:
    case CSS::WritingMode::SidewaysLr:
        return Gfx::Orientation::Vertical;
    default:
        VERIFY_NOT_REACHED();
    }
}

CSSPixelRect PaintableFragment::selection_rect() const
{
    auto const selection_state = paintable().selection_state();
    if (selection_state == Paintable::SelectionState::None)
        return {};

    if (auto const* focused_area = as_if<HTML::FormAssociatedTextControlElement>(paintable().document().focused_area().ptr())) {
        HTML::FormAssociatedTextControlElement const* text_control_element = nullptr;
        if (auto const* input_element = as_if<HTML::HTMLInputElement>(*focused_area)) {
            text_control_element = input_element;
        } else if (auto const* text_area_element = as_if<HTML::HTMLTextAreaElement>(*focused_area)) {
            text_control_element = text_area_element;
        } else {
            VERIFY_NOT_REACHED();
        }
        auto selection_start = text_control_element->selection_start();
        auto selection_end = text_control_element->selection_end();
        return range_rect(selection_state, selection_start, selection_end);
    }

    auto selection = paintable().document().get_selection();
    if (!selection)
        return {};
    auto range = selection->range();
    if (!range)
        return {};

    return range_rect(selection_state, range->start_offset(), range->end_offset());
}

Utf16View PaintableFragment::text() const
{
    if (!is<TextPaintable>(paintable()))
        return {};
    return as<TextPaintable>(paintable()).layout_node().text_for_rendering().substring_view(m_start_offset, m_length_in_code_units);
}

}
