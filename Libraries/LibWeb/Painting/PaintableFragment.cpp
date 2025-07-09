/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Range.h>
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
    , m_start_byte_offset(fragment.start())
    , m_length_in_bytes(fragment.length())
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

size_t PaintableFragment::index_in_node_for_byte_offset(size_t byte_offset) const
{
    if (m_length_in_bytes == 0)
        return 0;
    if (byte_offset >= m_start_byte_offset + m_length_in_bytes)
        return utf16_view().length_in_code_units();
    auto code_point_offset = utf8_view().code_point_offset_of(byte_offset);
    return utf16_view().code_unit_offset_of(code_point_offset);
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

    // Find the code point offset of the glyph matching the position.
    auto code_point_offset = utf8_view().code_point_offset_of(m_start_byte_offset);
    auto const& glyphs = m_glyph_run->glyphs();
    auto smallest_distance = AK::NumericLimits<float>::max();
    for (size_t i = 0; i < glyphs.size(); ++i) {
        auto distance_to_position = AK::abs(glyphs[i].position.x() - relative_inline_offset);

        // The last distance was smaller than this new distance, so we've found the closest glyph.
        if (distance_to_position > smallest_distance)
            break;
        smallest_distance = distance_to_position;

        ++code_point_offset;
    }

    // Return the code unit offset in the UTF-16 string.
    return utf16_view().code_unit_offset_of(code_point_offset - 1);
}

CSSPixelRect PaintableFragment::range_rect(Paintable::SelectionState selection_state, size_t start_offset_in_code_units, size_t end_offset_in_code_units) const
{
    if (selection_state == Paintable::SelectionState::None)
        return {};

    if (selection_state == Paintable::SelectionState::Full)
        return absolute_rect();

    auto const& font = glyph_run() ? glyph_run()->font() : layout_node().first_available_font();

    // We are invoked with offsets coming from a Range, which means they are expressed in UTF-16 code units. We need to
    // convert them to the byte offsets in the UTF-8 string. This is inefficient, but we only need to do it for
    // fragments with a partial selection.
    auto code_unit_to_byte_offset = [&](size_t offset_in_code_units) -> size_t {
        auto text_in_utf16 = utf16_view();
        if (offset_in_code_units >= text_in_utf16.length_in_code_units())
            return m_length_in_bytes;
        auto offset_code_point = text_in_utf16.code_point_offset_of(offset_in_code_units);
        auto byte_offset = utf8_view().byte_offset_of(offset_code_point);
        if (byte_offset <= m_start_byte_offset)
            return 0;
        if (byte_offset > m_start_byte_offset + m_length_in_bytes)
            return m_length_in_bytes;
        return byte_offset - m_start_byte_offset;
    };

    // We operate on the UTF-8 string that is part of this fragment.
    auto text = utf8_view().substring_view(m_start_byte_offset, m_length_in_bytes);

    if (selection_state == Paintable::SelectionState::StartAndEnd) {
        auto selection_start_in_this_fragment = code_unit_to_byte_offset(start_offset_in_code_units);
        auto selection_end_in_this_fragment = code_unit_to_byte_offset(end_offset_in_code_units);

        // we are in the start/end node (both the same)
        if (selection_start_in_this_fragment >= m_length_in_bytes)
            return {};
        if (selection_end_in_this_fragment == 0)
            return {};
        if (selection_start_in_this_fragment == selection_end_in_this_fragment)
            return {};

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
        auto selection_start_in_this_fragment = code_unit_to_byte_offset(start_offset_in_code_units);
        auto selection_end_in_this_fragment = m_length_in_bytes;

        // we are in the start node
        if (selection_start_in_this_fragment >= m_length_in_bytes)
            return {};

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
        auto selection_start_in_this_fragment = 0u;
        auto selection_end_in_this_fragment = code_unit_to_byte_offset(end_offset_in_code_units);

        // we are in the end node
        if (selection_end_in_this_fragment == 0)
            return {};

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
    if (auto const* focused_element = paintable().document().focused_element(); focused_element && is<HTML::FormAssociatedTextControlElement>(*focused_element)) {
        HTML::FormAssociatedTextControlElement const* text_control_element = nullptr;
        if (is<HTML::HTMLInputElement>(*focused_element)) {
            text_control_element = static_cast<HTML::HTMLInputElement const*>(focused_element);
        } else if (is<HTML::HTMLTextAreaElement>(*focused_element)) {
            text_control_element = static_cast<HTML::HTMLTextAreaElement const*>(focused_element);
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

Utf8View PaintableFragment::utf8_view() const
{
    if (!is<TextPaintable>(paintable()))
        return {};
    return Utf8View { static_cast<TextPaintable const&>(paintable()).text_for_rendering() };
}

Utf16View PaintableFragment::utf16_view() const
{
    if (!is<TextPaintable>(paintable()))
        return {};

    if (!m_text_in_utf16.has_value())
        m_text_in_utf16 = Utf16String::from_utf8(utf8_view().as_string());
    return *m_text_in_utf16;
}

}
