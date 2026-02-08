/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
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

Optional<PaintableFragment::SelectionOffsets> PaintableFragment::compute_selection_offsets(Paintable::SelectionState selection_state, size_t start_offset_in_code_units, size_t end_offset_in_code_units) const
{
    auto const start_index = m_start_offset;
    auto const end_index = m_start_offset + m_length_in_code_units;

    switch (selection_state) {
    case Paintable::SelectionState::None:
        return {};
    case Paintable::SelectionState::Full:
        return SelectionOffsets { 0, m_length_in_code_units, true };
    case Paintable::SelectionState::StartAndEnd:
        if (start_index > end_offset_in_code_units || end_index < start_offset_in_code_units)
            return {};
        return SelectionOffsets {
            start_offset_in_code_units - min(start_offset_in_code_units, m_start_offset),
            min(end_offset_in_code_units - m_start_offset, m_length_in_code_units),
            end_offset_in_code_units >= end_index,
        };
    case Paintable::SelectionState::Start:
        if (end_index < start_offset_in_code_units)
            return {};
        return SelectionOffsets {
            start_offset_in_code_units - min(start_offset_in_code_units, m_start_offset),
            m_length_in_code_units,
            true,
        };
    case Paintable::SelectionState::End:
        if (start_index > end_offset_in_code_units)
            return {};
        return SelectionOffsets {
            0,
            min(end_offset_in_code_units - m_start_offset, m_length_in_code_units),
            end_offset_in_code_units >= end_index,
        };
    }
    VERIFY_NOT_REACHED();
}

CSSPixelRect PaintableFragment::range_rect(Paintable::SelectionState selection_state, size_t start_offset_in_code_units, size_t end_offset_in_code_units) const
{
    auto offsets = compute_selection_offsets(selection_state, start_offset_in_code_units, end_offset_in_code_units);
    if (!offsets.has_value())
        return {};

    auto rect = absolute_rect();
    auto const& font = glyph_run() ? glyph_run()->font() : layout_node().first_available_font();

    CSSPixels pixel_offset;
    CSSPixels pixel_width;

    // When entire fragment is selected, use the rect's existing dimensions rather than recalculating from text.
    if (offsets->start == 0 && offsets->end == m_length_in_code_units && m_length_in_code_units > 0) {
        pixel_offset = 0;
        pixel_width = rect.primary_size_for_orientation(orientation());
    } else {
        pixel_offset = CSSPixels::nearest_value_for(font.width(text().substring_view(0, offsets->start)));

        // When start equals end, this is a cursor position.
        if (offsets->start == offsets->end) {
            pixel_width = 1;
        } else {
            pixel_width = CSSPixels::nearest_value_for(font.width(text().substring_view(offsets->start, offsets->end - offsets->start)));
        }
    }

    if (m_has_trailing_whitespace && offsets->include_trailing_whitespace && offsets->start != offsets->end)
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

Optional<PaintableFragment::SelectionOffsets> PaintableFragment::selection_range_for_text_control() const
{
    // For focused text controls (input/textarea), determine selection from the control's internal state.
    auto const* text_control = as_if<HTML::FormAssociatedTextControlElement>(paintable().document().focused_area().ptr());
    if (!text_control)
        return {};
    if (paintable().dom_node() != text_control->form_associated_element_to_text_node())
        return {};

    auto selection_start = text_control->selection_start();
    auto selection_end = text_control->selection_end();
    if (selection_start == selection_end)
        return {};

    return SelectionOffsets { selection_start, selection_end };
}

Optional<PaintableFragment::SelectionOffsets> PaintableFragment::selection_offsets() const
{
    if (auto offsets = selection_range_for_text_control(); offsets.has_value())
        return compute_selection_offsets(Paintable::SelectionState::StartAndEnd, offsets->start, offsets->end);

    auto selection_state = paintable().selection_state();
    if (selection_state == Paintable::SelectionState::None)
        return {};

    auto selection = paintable().document().get_selection();
    if (!selection)
        return {};
    auto range = selection->range();
    if (!range)
        return {};

    return compute_selection_offsets(selection_state, range->start_offset(), range->end_offset());
}

CSSPixelRect PaintableFragment::selection_rect() const
{
    if (auto offsets = selection_range_for_text_control(); offsets.has_value())
        return range_rect(Paintable::SelectionState::StartAndEnd, offsets->start, offsets->end);

    auto const selection_state = paintable().selection_state();
    if (selection_state == Paintable::SelectionState::None)
        return {};

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
