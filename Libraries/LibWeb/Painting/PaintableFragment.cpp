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
    , m_baseline(fragment.baseline())
    , m_start_offset(fragment.start())
    , m_length_in_code_units(fragment.length())
    , m_glyph_run(fragment.glyph_run())
    , m_writing_mode(fragment.writing_mode())
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
    if (selection_state == Paintable::SelectionState::None)
        return {};

    if (selection_state == Paintable::SelectionState::Full)
        return absolute_rect();

    auto const start_index = m_start_offset;
    auto const end_index = m_start_offset + m_length_in_code_units;

    auto const& font = glyph_run() ? glyph_run()->font() : layout_node().first_available_font();
    auto text = this->text();

    if (selection_state == Paintable::SelectionState::StartAndEnd) {
        // we are in the start/end node (both the same)
        if (start_index > end_offset_in_code_units)
            return {};
        if (end_index < start_offset_in_code_units)
            return {};

        if (start_offset_in_code_units == end_offset_in_code_units)
            return {};

        auto selection_start_in_this_fragment = max(0, start_offset_in_code_units - m_start_offset);
        auto selection_end_in_this_fragment = min(m_length_in_code_units, end_offset_in_code_units - m_start_offset);
        auto pixel_distance_to_first_selected_character = CSSPixels::nearest_value_for(font.width(text.substring_view(0, selection_start_in_this_fragment)));
        auto pixel_width_of_selection = CSSPixels::nearest_value_for(font.width(text.substring_view(selection_start_in_this_fragment, selection_end_in_this_fragment - selection_start_in_this_fragment))) + 1;

        auto rect = absolute_rect();
        switch (orientation()) {
        case Gfx::Orientation::Horizontal:
            rect.set_x(rect.x() + pixel_distance_to_first_selected_character);
            rect.set_width(pixel_width_of_selection);
            break;
        case Gfx::Orientation::Vertical:
            rect.set_y(rect.y() + pixel_distance_to_first_selected_character);
            rect.set_height(pixel_width_of_selection);
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        return rect;
    }
    if (selection_state == Paintable::SelectionState::Start) {
        // we are in the start node
        if (end_index < start_offset_in_code_units)
            return {};

        auto selection_start_in_this_fragment = max(0, start_offset_in_code_units - m_start_offset);
        auto selection_end_in_this_fragment = m_length_in_code_units;
        auto pixel_distance_to_first_selected_character = CSSPixels::nearest_value_for(font.width(text.substring_view(0, selection_start_in_this_fragment)));
        auto pixel_width_of_selection = CSSPixels::nearest_value_for(font.width(text.substring_view(selection_start_in_this_fragment, selection_end_in_this_fragment - selection_start_in_this_fragment))) + 1;

        auto rect = absolute_rect();
        switch (orientation()) {
        case Gfx::Orientation::Horizontal:
            rect.set_x(rect.x() + pixel_distance_to_first_selected_character);
            rect.set_width(pixel_width_of_selection);
            break;
        case Gfx::Orientation::Vertical:
            rect.set_y(rect.y() + pixel_distance_to_first_selected_character);
            rect.set_height(pixel_width_of_selection);
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        return rect;
    }
    if (selection_state == Paintable::SelectionState::End) {
        // we are in the end node
        if (start_index > end_offset_in_code_units)
            return {};

        auto selection_start_in_this_fragment = 0;
        auto selection_end_in_this_fragment = min<int>(end_offset_in_code_units - m_start_offset, m_length_in_code_units);
        auto pixel_distance_to_first_selected_character = CSSPixels::nearest_value_for(font.width(text.substring_view(0, selection_start_in_this_fragment)));
        auto pixel_width_of_selection = CSSPixels::nearest_value_for(font.width(text.substring_view(selection_start_in_this_fragment, selection_end_in_this_fragment - selection_start_in_this_fragment))) + 1;

        auto rect = absolute_rect();
        switch (orientation()) {
        case Gfx::Orientation::Horizontal:
            rect.set_x(rect.x() + pixel_distance_to_first_selected_character);
            rect.set_width(pixel_width_of_selection);
            break;
        case Gfx::Orientation::Vertical:
            rect.set_y(rect.y() + pixel_distance_to_first_selected_character);
            rect.set_height(pixel_width_of_selection);
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        return rect;
    }
    return {};
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
    if (auto focused_area = paintable().document().focused_area(); is<HTML::FormAssociatedTextControlElement>(focused_area.ptr())) {
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
        return range_rect(paintable().selection_state(), selection_start, selection_end);
    }

    auto selection = paintable().document().get_selection();
    if (!selection)
        return {};
    auto range = selection->range();
    if (!range)
        return {};

    return range_rect(paintable().selection_state(), range->start_offset(), range->end_offset());
}

Utf16View PaintableFragment::text() const
{
    if (!is<TextPaintable>(paintable()))
        return {};
    return as<TextPaintable>(paintable()).layout_node().text_for_rendering().substring_view(m_start_offset, m_length_in_code_units);
}

}
