/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Range.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/TextPaintable.h>

namespace Web::Painting {

PaintableFragment::PaintableFragment(Layout::LineBoxFragment const& fragment)
    : m_layout_node(fragment.layout_node())
    , m_offset(fragment.offset())
    , m_size(fragment.size())
    , m_baseline(fragment.baseline())
    , m_start(fragment.start())
    , m_length(fragment.length())
    , m_glyph_run(fragment.glyph_run())
{
}

CSSPixelRect const PaintableFragment::absolute_rect() const
{
    CSSPixelRect rect { {}, size() };
    auto const* containing_block = paintable().containing_block();
    if (containing_block)
        rect.set_location(containing_block->absolute_position());
    rect.translate_by(offset());
    return rect;
}

int PaintableFragment::text_index_at(CSSPixels x) const
{
    if (!is<TextPaintable>(paintable()))
        return 0;

    CSSPixels relative_x = x - absolute_rect().x();
    if (relative_x < 0)
        return 0;

    auto const& glyphs = m_glyph_run->glyphs();
    for (size_t i = 0; i < glyphs.size(); ++i) {
        auto glyph_position = CSSPixels::nearest_value_for(glyphs[i].position.x());
        if (i + 1 < glyphs.size()) {
            auto next_glyph_position = CSSPixels::nearest_value_for(glyphs[i + 1].position.x());
            if (relative_x >= glyph_position && relative_x < next_glyph_position)
                return m_start + i;
        } else {
            if (relative_x >= glyph_position)
                return m_start + i;
        }
    }

    return m_start + m_length;
}
CSSPixelRect PaintableFragment::range_rect(Gfx::Font const& font, DOM::Range const& range) const
{
    if (paintable().selection_state() == Paintable::SelectionState::None)
        return {};

    if (paintable().selection_state() == Paintable::SelectionState::Full)
        return absolute_rect();

    // FIXME: m_start and m_length should be unsigned and then we won't need these casts.
    auto const start_index = static_cast<unsigned>(m_start);
    auto const end_index = static_cast<unsigned>(m_start) + static_cast<unsigned>(m_length);

    auto text = string_view();

    if (paintable().selection_state() == Paintable::SelectionState::StartAndEnd) {
        // we are in the start/end node (both the same)
        if (start_index > range.end_offset())
            return {};
        if (end_index < range.start_offset())
            return {};

        if (range.start_offset() == range.end_offset())
            return {};

        auto selection_start_in_this_fragment = max(0, range.start_offset() - m_start);
        auto selection_end_in_this_fragment = min(m_length, range.end_offset() - m_start);
        auto pixel_distance_to_first_selected_character = CSSPixels::nearest_value_for(font.width(text.substring_view(0, selection_start_in_this_fragment)));
        auto pixel_width_of_selection = CSSPixels::nearest_value_for(font.width(text.substring_view(selection_start_in_this_fragment, selection_end_in_this_fragment - selection_start_in_this_fragment))) + 1;

        auto rect = absolute_rect();
        rect.set_x(rect.x() + pixel_distance_to_first_selected_character);
        rect.set_width(pixel_width_of_selection);

        return rect;
    }
    if (paintable().selection_state() == Paintable::SelectionState::Start) {
        // we are in the start node
        if (end_index < range.start_offset())
            return {};

        auto selection_start_in_this_fragment = max(0, range.start_offset() - m_start);
        auto selection_end_in_this_fragment = m_length;
        auto pixel_distance_to_first_selected_character = CSSPixels::nearest_value_for(font.width(text.substring_view(0, selection_start_in_this_fragment)));
        auto pixel_width_of_selection = CSSPixels::nearest_value_for(font.width(text.substring_view(selection_start_in_this_fragment, selection_end_in_this_fragment - selection_start_in_this_fragment))) + 1;

        auto rect = absolute_rect();
        rect.set_x(rect.x() + pixel_distance_to_first_selected_character);
        rect.set_width(pixel_width_of_selection);

        return rect;
    }
    if (paintable().selection_state() == Paintable::SelectionState::End) {
        // we are in the end node
        if (start_index > range.end_offset())
            return {};

        auto selection_start_in_this_fragment = 0;
        auto selection_end_in_this_fragment = min(range.end_offset() - m_start, m_length);
        auto pixel_distance_to_first_selected_character = CSSPixels::nearest_value_for(font.width(text.substring_view(0, selection_start_in_this_fragment)));
        auto pixel_width_of_selection = CSSPixels::nearest_value_for(font.width(text.substring_view(selection_start_in_this_fragment, selection_end_in_this_fragment - selection_start_in_this_fragment))) + 1;

        auto rect = absolute_rect();
        rect.set_x(rect.x() + pixel_distance_to_first_selected_character);
        rect.set_width(pixel_width_of_selection);

        return rect;
    }
    return {};
}

CSSPixelRect PaintableFragment::selection_rect(Gfx::Font const& font) const
{
    if (!paintable().is_selected())
        return {};

    auto selection = paintable().document().get_selection();
    if (!selection)
        return {};
    auto range = selection->range();
    if (!range)
        return {};

    return range_rect(font, *range);
}

StringView PaintableFragment::string_view() const
{
    if (!is<TextPaintable>(paintable()))
        return {};
    return static_cast<TextPaintable const&>(paintable()).text_for_rendering().bytes_as_string_view().substring_view(m_start, m_length);
}

}
