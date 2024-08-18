/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/LineBuilder.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web::Layout {

LineBuilder::LineBuilder(InlineFormattingContext& context, LayoutState& layout_state, LayoutState::UsedValues& containing_block_used_values, CSS::Direction direction)
    : m_context(context)
    , m_layout_state(layout_state)
    , m_containing_block_used_values(containing_block_used_values)
    , m_direction(direction)
{
    m_text_indent = m_context.containing_block().computed_values().text_indent().to_px(m_context.containing_block(), m_containing_block_used_values.content_width());
    begin_new_line(false);
}

LineBuilder::~LineBuilder()
{
    if (m_last_line_needs_update)
        update_last_line();
}

void LineBuilder::break_line(ForcedBreak forced_break, Optional<CSSPixels> next_item_width)
{
    auto& last_line_box = ensure_last_line_box();
    last_line_box.m_has_break = true;
    last_line_box.m_has_forced_break = forced_break == ForcedBreak::Yes;

    update_last_line();
    size_t break_count = 0;
    bool floats_intrude_at_current_y = false;
    do {
        m_containing_block_used_values.line_boxes.append(LineBox(m_direction));
        begin_new_line(true, break_count == 0);
        break_count++;
        floats_intrude_at_current_y = m_context.any_floats_intrude_at_y(m_current_y);
    } while ((floats_intrude_at_current_y && !m_context.can_fit_new_line_at_y(m_current_y))
        || (next_item_width.has_value()
            && next_item_width.value() > m_available_width_for_current_line
            && floats_intrude_at_current_y));
}

void LineBuilder::begin_new_line(bool increment_y, bool is_first_break_in_sequence)
{
    if (increment_y) {
        if (is_first_break_in_sequence) {
            // First break is simple, just go to the start of the next line.
            m_current_y += max(m_max_height_on_current_line, m_context.containing_block().computed_values().line_height());
        } else {
            // We're doing more than one break in a row.
            // This means we're trying to squeeze past intruding floats.
            // Scan 1px at a time until we find a Y value where a new line can fit.
            // FIXME: This is super dumb and inefficient.
            CSSPixels candidate_y = m_current_y + 1;
            while (true) {
                if (m_context.can_fit_new_line_at_y(candidate_y))
                    break;
                ++candidate_y;
            }
            m_current_y = candidate_y;
        }
    }
    recalculate_available_space();
    ensure_last_line_box().m_original_available_width = m_available_width_for_current_line;
    m_max_height_on_current_line = 0;
    m_last_line_needs_update = true;

    // FIXME: Support text-indent with "each-line".
    if (m_containing_block_used_values.line_boxes.size() <= 1) {
        ensure_last_line_box().m_width += m_text_indent;
    }
}

LineBox& LineBuilder::ensure_last_line_box()
{
    auto& line_boxes = m_containing_block_used_values.line_boxes;
    if (line_boxes.is_empty())
        line_boxes.append(LineBox(m_direction));
    return line_boxes.last();
}

void LineBuilder::append_box(Box const& box, CSSPixels leading_size, CSSPixels trailing_size, CSSPixels leading_margin, CSSPixels trailing_margin)
{
    auto& box_state = m_layout_state.get_mutable(box);
    auto& line_box = ensure_last_line_box();
    line_box.add_fragment(box, 0, 0, leading_size, trailing_size, leading_margin, trailing_margin, box_state.content_width(), box_state.content_height(), box_state.border_box_top(), box_state.border_box_bottom());
    m_max_height_on_current_line = max(m_max_height_on_current_line, box_state.margin_box_height());

    box_state.containing_line_box_fragment = LineBoxFragmentCoordinate {
        .line_box_index = m_containing_block_used_values.line_boxes.size() - 1,
        .fragment_index = line_box.fragments().size() - 1,
    };
}

void LineBuilder::append_text_chunk(TextNode const& text_node, size_t offset_in_node, size_t length_in_node, CSSPixels leading_size, CSSPixels trailing_size, CSSPixels leading_margin, CSSPixels trailing_margin, CSSPixels content_width, CSSPixels content_height, RefPtr<Gfx::GlyphRun> glyph_run)
{
    ensure_last_line_box().add_fragment(text_node, offset_in_node, length_in_node, leading_size, trailing_size, leading_margin, trailing_margin, content_width, content_height, 0, 0, move(glyph_run));
    m_max_height_on_current_line = max(m_max_height_on_current_line, content_height);
}

CSSPixels LineBuilder::y_for_float_to_be_inserted_here(Box const& box)
{
    auto const& box_state = m_layout_state.get(box);
    CSSPixels const width = box_state.margin_box_width();
    CSSPixels const height = box_state.margin_box_height();

    CSSPixels candidate_y = m_current_y;

    auto const& current_line = ensure_last_line_box();
    // If there's already inline content on the current line, check if the new float can fit
    // alongside the content. If not, place it on the next line.
    if (current_line.width() > 0 && (current_line.width() + width) > m_available_width_for_current_line)
        candidate_y += current_line.height();

    // Then, look for the next Y position where we can fit the new float.
    // FIXME: This is super dumb, we move 1px downwards per iteration and stop
    //        when we find an Y value where we don't collide with other floats.
    while (true) {
        auto space_at_y_top = m_context.available_space_for_line(candidate_y);
        auto space_at_y_bottom = m_context.available_space_for_line(candidate_y + height);
        if (width > space_at_y_top || width > space_at_y_bottom) {
            if (!m_context.any_floats_intrude_at_y(candidate_y) && !m_context.any_floats_intrude_at_y(candidate_y + height)) {
                return candidate_y;
            }
        } else {
            return candidate_y;
        }
        candidate_y += 1;
    }
}

bool LineBuilder::should_break(CSSPixels next_item_width)
{
    if (m_available_width_for_current_line.is_max_content())
        return false;

    auto const& line_boxes = m_containing_block_used_values.line_boxes;
    if (line_boxes.is_empty() || line_boxes.last().is_empty()) {
        // If we don't have a single line box yet *and* there are no floats intruding
        // at this Y coordinate, we don't need to break before inserting anything.
        if (!m_context.any_floats_intrude_at_y(m_current_y))
            return false;
        if (!m_context.any_floats_intrude_at_y(m_current_y + m_context.containing_block().computed_values().line_height()))
            return false;
    }
    auto current_line_width = ensure_last_line_box().width();
    return (current_line_width + next_item_width) > m_available_width_for_current_line;
}

void LineBuilder::update_last_line()
{
    m_last_line_needs_update = false;
    auto& line_boxes = m_containing_block_used_values.line_boxes;

    if (line_boxes.is_empty())
        return;

    auto& line_box = line_boxes.last();

    auto text_align = m_context.containing_block().computed_values().text_align();
    auto direction = m_context.containing_block().computed_values().direction();

    auto current_line_height = max(m_max_height_on_current_line, m_context.containing_block().computed_values().line_height());
    CSSPixels x_offset_top = m_context.leftmost_x_offset_at(m_current_y);
    CSSPixels x_offset_bottom = m_context.leftmost_x_offset_at(m_current_y + current_line_height - 1);
    CSSPixels x_offset = max(x_offset_top, x_offset_bottom);

    CSSPixels excess_horizontal_space = m_available_width_for_current_line.to_px_or_zero() - line_box.width();

    // If (after justification, if any) the inline contents of a line box are too long to fit within it,
    // then the contents are start-aligned: any content that doesn't fit overflows the line box’s end edge.
    if (excess_horizontal_space > 0) {
        switch (text_align) {
        case CSS::TextAlign::Center:
        case CSS::TextAlign::LibwebCenter:
            x_offset += excess_horizontal_space / 2;
            break;
        case CSS::TextAlign::Start:
            if (direction == CSS::Direction::Rtl)
                x_offset += excess_horizontal_space;
            break;
        case CSS::TextAlign::End:
            if (direction == CSS::Direction::Ltr)
                x_offset += excess_horizontal_space;
            break;
        case CSS::TextAlign::Right:
        case CSS::TextAlign::LibwebRight:
            x_offset += excess_horizontal_space;
            break;
        case CSS::TextAlign::Left:
        case CSS::TextAlign::LibwebLeft:
        case CSS::TextAlign::Justify:
        default:
            break;
        }
    }

    auto strut_baseline = [&] {
        auto& font = m_context.containing_block().first_available_font();
        auto const line_height = m_context.containing_block().computed_values().line_height();
        auto const font_metrics = font.pixel_metrics();
        auto const typographic_height = CSSPixels::nearest_value_for(font_metrics.ascent + font_metrics.descent);
        auto const leading = line_height - typographic_height;
        auto const half_leading = leading / 2;
        return CSSPixels::nearest_value_for(font_metrics.ascent) + half_leading;
    }();

    auto line_box_baseline = [&] {
        CSSPixels line_box_baseline = strut_baseline;
        for (auto& fragment : line_box.fragments()) {
            auto const& font = fragment.layout_node().first_available_font();
            auto const line_height = fragment.layout_node().computed_values().line_height();
            auto const font_metrics = font.pixel_metrics();
            auto const typographic_height = CSSPixels::nearest_value_for(font_metrics.ascent + font_metrics.descent);
            auto const leading = line_height - typographic_height;
            auto const half_leading = leading / 2;

            // The CSS specification calls this AD (A+D, Ascent + Descent).

            CSSPixels fragment_baseline = 0;
            if (fragment.layout_node().is_text_node()) {
                fragment_baseline = CSSPixels::nearest_value_for(font_metrics.ascent) + half_leading;
            } else {
                auto const& box = verify_cast<Layout::Box>(fragment.layout_node());
                fragment_baseline = m_context.box_baseline(box);
            }

            // Remember the baseline used for this fragment. This will be used when painting the fragment.
            fragment.set_baseline(fragment_baseline);

            // NOTE: For fragments with a <length> vertical-align, shift the line box baseline down by the length.
            //       This ensures that we make enough vertical space on the line for any manually-aligned fragments.
            if (auto const* length_percentage = fragment.layout_node().computed_values().vertical_align().get_pointer<CSS::LengthPercentage>()) {
                if (length_percentage->is_length())
                    fragment_baseline += length_percentage->length().to_px(fragment.layout_node());
                else if (length_percentage->is_percentage())
                    fragment_baseline += line_height.scaled(length_percentage->percentage().as_fraction());
            }

            line_box_baseline = max(line_box_baseline, fragment_baseline);
        }
        return line_box_baseline;
    }();

    // Start with the "strut", an imaginary zero-width box at the start of each line box.
    auto strut_top = m_current_y;
    auto strut_bottom = m_current_y + m_context.containing_block().computed_values().line_height();

    CSSPixels uppermost_box_top = strut_top;
    CSSPixels lowermost_box_bottom = strut_bottom;

    for (size_t i = 0; i < line_box.fragments().size(); ++i) {
        auto& fragment = line_box.fragments()[i];

        CSSPixels new_fragment_x = round(x_offset + fragment.offset().x());
        CSSPixels new_fragment_y = 0;

        auto y_value_for_alignment = [&](CSS::VerticalAlign vertical_align) {
            CSSPixels effective_box_top_offset = fragment.border_box_top();
            CSSPixels effective_box_bottom_offset = fragment.border_box_top();
            if (fragment.is_atomic_inline()) {
                auto const& fragment_box_state = m_layout_state.get(static_cast<Box const&>(fragment.layout_node()));
                effective_box_top_offset = fragment_box_state.margin_box_top();
                effective_box_bottom_offset = fragment_box_state.margin_box_bottom();
            }

            switch (vertical_align) {
            case CSS::VerticalAlign::Baseline:
                return m_current_y + line_box_baseline - fragment.baseline() + effective_box_top_offset;
            case CSS::VerticalAlign::Top:
                return m_current_y + effective_box_top_offset;
            case CSS::VerticalAlign::Middle: {
                // Align the vertical midpoint of the box with the baseline of the parent box
                // plus half the x-height of the parent.
                auto const x_height = CSSPixels::nearest_value_for(m_context.containing_block().first_available_font().pixel_metrics().x_height);
                return m_current_y + line_box_baseline + ((effective_box_top_offset - effective_box_bottom_offset - x_height - fragment.height()) / 2);
            }
            case CSS::VerticalAlign::Bottom:
            case CSS::VerticalAlign::Sub:
            case CSS::VerticalAlign::Super:
            case CSS::VerticalAlign::TextBottom:
            case CSS::VerticalAlign::TextTop:
                // FIXME: These are all 'baseline'
                return m_current_y + line_box_baseline - fragment.baseline() + effective_box_top_offset;
            }
            VERIFY_NOT_REACHED();
        };

        auto const& vertical_align = fragment.layout_node().computed_values().vertical_align();
        if (vertical_align.has<CSS::VerticalAlign>()) {
            new_fragment_y = y_value_for_alignment(vertical_align.get<CSS::VerticalAlign>());
        } else {
            if (auto const* length_percentage = vertical_align.get_pointer<CSS::LengthPercentage>()) {
                if (length_percentage->is_length()) {
                    auto vertical_align_amount = length_percentage->length().to_px(fragment.layout_node());
                    new_fragment_y = y_value_for_alignment(CSS::VerticalAlign::Baseline) - vertical_align_amount;
                } else if (length_percentage->is_percentage()) {
                    auto vertical_align_amount = m_context.containing_block().computed_values().line_height().scaled(length_percentage->percentage().as_fraction());
                    new_fragment_y = y_value_for_alignment(CSS::VerticalAlign::Baseline) - vertical_align_amount;
                }
            }
        }

        fragment.set_offset({ new_fragment_x, floor(new_fragment_y) });

        CSSPixels top_of_inline_box = 0;
        CSSPixels bottom_of_inline_box = 0;
        {
            // FIXME: Support inline-table elements.
            if (fragment.is_atomic_inline()) {
                auto const& fragment_box_state = m_layout_state.get(static_cast<Box const&>(fragment.layout_node()));
                top_of_inline_box = (fragment.offset().y() - fragment_box_state.margin_box_top());
                bottom_of_inline_box = (fragment.offset().y() + fragment_box_state.content_height() + fragment_box_state.margin_box_bottom());
            } else {
                auto font_metrics = fragment.layout_node().first_available_font().pixel_metrics();
                auto typographic_height = CSSPixels::nearest_value_for(font_metrics.ascent + font_metrics.descent);
                auto leading = fragment.layout_node().computed_values().line_height() - typographic_height;
                auto half_leading = leading / 2;
                top_of_inline_box = (fragment.offset().y() + fragment.baseline() - CSSPixels::nearest_value_for(font_metrics.ascent) - half_leading);
                bottom_of_inline_box = (fragment.offset().y() + fragment.baseline() + CSSPixels::nearest_value_for(font_metrics.descent) + half_leading);
            }
            if (auto const* length_percentage = fragment.layout_node().computed_values().vertical_align().get_pointer<CSS::LengthPercentage>()) {
                if (length_percentage->is_length())
                    bottom_of_inline_box += length_percentage->length().to_px(fragment.layout_node());
                else if (length_percentage->is_percentage())
                    bottom_of_inline_box += m_context.containing_block().computed_values().line_height().scaled(length_percentage->percentage().as_fraction());
            }
        }

        uppermost_box_top = min(uppermost_box_top, top_of_inline_box);
        lowermost_box_bottom = max(lowermost_box_bottom, bottom_of_inline_box);
    }

    // 3. The line box height is the distance between the uppermost box top and the lowermost box bottom.
    line_box.m_height = lowermost_box_bottom - uppermost_box_top;

    line_box.m_bottom = m_current_y + line_box.m_height;
    line_box.m_baseline = line_box_baseline;
}

void LineBuilder::remove_last_line_if_empty()
{
    // If there's an empty line box at the bottom, just remove it instead of giving it height.
    auto& line_boxes = m_containing_block_used_values.line_boxes;
    if (!line_boxes.is_empty() && line_boxes.last().is_empty()) {
        line_boxes.take_last();
        m_last_line_needs_update = false;
    }
}

void LineBuilder::recalculate_available_space()
{
    auto current_line_height = max(m_max_height_on_current_line, m_context.containing_block().computed_values().line_height());
    auto available_at_top_of_line_box = m_context.available_space_for_line(m_current_y);
    auto available_at_bottom_of_line_box = m_context.available_space_for_line(m_current_y + current_line_height - 1);
    m_available_width_for_current_line = min(available_at_bottom_of_line_box, available_at_top_of_line_box);
    if (!m_containing_block_used_values.line_boxes.is_empty())
        m_containing_block_used_values.line_boxes.last().m_original_available_width = m_available_width_for_current_line;
}

}
