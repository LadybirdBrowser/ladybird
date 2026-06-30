/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/LineBuilder.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web::Layout {

LineBuilder::LineBuilder(InlineFormattingContext& context, LayoutState& layout_state, LayoutState::UsedValues& containing_block_used_values, CSS::Direction direction, CSS::WritingMode writing_mode)
    : m_context(context)
    , m_layout_state(layout_state)
    , m_containing_block_used_values(containing_block_used_values)
    , m_direction(direction)
    , m_writing_mode(writing_mode)
{
    auto text_indent = m_context.containing_block().computed_values().text_indent();
    m_text_indent = text_indent.length_percentage.to_px(m_containing_block_used_values.content_width());
    m_text_indent_each_line = text_indent.each_line;
    m_text_indent_hanging = text_indent.hanging;
    begin_new_line(false);
}

void LineBuilder::break_line(ForcedBreak forced_break, Optional<CSSPixels> next_item_width)
{
    // FIXME: Respect inline direction.

    auto& last_line_box = ensure_last_line_box();
    last_line_box.m_has_break = true;
    last_line_box.m_has_forced_break = forced_break == ForcedBreak::Yes;

    m_last_line_needs_update = true;
    update_last_line();

    size_t break_count = 0;
    bool floats_intrude_at_current_y = false;
    do {
        m_containing_block_used_values.line_boxes.append(LineBox(m_direction, m_writing_mode));
        begin_new_line(true, break_count == 0, forced_break);
        break_count++;
        auto current_line_height = max(m_max_height_on_current_line, m_context.containing_block().computed_values().line_height());
        floats_intrude_at_current_y = m_context.any_floats_intrude_in_block_range(m_current_block_offset, m_current_block_offset + current_line_height);
    } while (floats_intrude_at_current_y
        && (!m_context.can_fit_new_line_at_block_offset(m_current_block_offset, m_context.containing_block().computed_values().line_height())
            || (next_item_width.value_or(0) > m_available_width_for_current_line)));
}

void LineBuilder::begin_new_line(bool increment_y, bool is_first_break_in_sequence, ForcedBreak forced_break)
{
    if (increment_y) {
        if (is_first_break_in_sequence) {
            // First break is simple, just go to the start of the next line.
            if (m_should_advance_to_last_line_box_bottom && m_containing_block_used_values.line_boxes.size() > 1)
                m_current_block_offset = m_containing_block_used_values.line_boxes[m_containing_block_used_values.line_boxes.size() - 2].bottom();
            else
                m_current_block_offset += max(m_max_height_on_current_line, m_context.containing_block().computed_values().line_height());
        } else {
            // We're doing more than one break in a row.
            // This means we're trying to squeeze past intruding floats.
            if (auto next_band_start = m_context.next_float_band_block_start_after(m_current_block_offset); next_band_start.has_value())
                m_current_block_offset = next_band_start.value();
            else
                m_current_block_offset += max(m_max_height_on_current_line, m_context.containing_block().computed_values().line_height());
        }
    }
    recalculate_available_space();
    auto& line_box = ensure_last_line_box();
    line_box.m_original_available_width = m_available_width_for_current_line;
    m_max_height_on_current_line = 0;
    m_last_line_needs_update = true;
    m_should_advance_to_last_line_box_bottom = false;

    bool should_indent = m_containing_block_used_values.line_boxes.size() <= 1
        || (m_text_indent_each_line && forced_break == ForcedBreak::Yes);

    if (m_text_indent_hanging)
        should_indent = !should_indent;

    if (should_indent)
        line_box.m_inline_length += m_text_indent;
}

LineBox& LineBuilder::ensure_last_line_box()
{
    auto& line_boxes = m_containing_block_used_values.line_boxes;
    if (line_boxes.is_empty())
        line_boxes.append(LineBox(m_direction, m_writing_mode));
    return line_boxes.last();
}

void LineBuilder::append_box(Box const& box, CSSPixels leading_size, CSSPixels trailing_size, CSSPixels leading_margin, CSSPixels trailing_margin)
{
    auto& box_state = m_layout_state.get_mutable(box);
    auto& line_box = ensure_last_line_box();
    line_box.add_fragment(box, 0, 0, leading_size, trailing_size, leading_margin, trailing_margin,
        box_state.content_width(), box_state.content_height(), box_state.border_box_top(), box_state.border_box_bottom());
    m_max_height_on_current_line = max(m_max_height_on_current_line, box_state.margin_box_height());

    box_state.containing_line_box_fragment = {};

    // https://drafts.csswg.org/css-display/#atomic-inline
    // Inline-level boxes that are not inline boxes are called atomic inline-level boxes because they
    // participate in their inline formatting context as a single opaque box.
    if (box.is_atomic_inline()) {
        box_state.containing_line_box_fragment = LineBoxFragmentCoordinate {
            .line_box_index = m_containing_block_used_values.line_boxes.size() - 1,
            .fragment_index = line_box.fragments().size() - 1,
        };
    }
}

void LineBuilder::append_text_chunk(TextNode const& text_node, size_t offset_in_node, size_t length_in_node, CSSPixels leading_size, CSSPixels trailing_size, CSSPixels leading_margin, CSSPixels trailing_margin, CSSPixels content_width, CSSPixels content_height, RefPtr<Gfx::GlyphRun> glyph_run)
{
    auto& line_box = ensure_last_line_box();
    line_box.add_fragment(text_node, offset_in_node, length_in_node, leading_size, trailing_size, leading_margin,
        trailing_margin, content_width, content_height, 0, 0, move(glyph_run));

    m_max_height_on_current_line = max(m_max_height_on_current_line, line_box.block_length());
}

void LineBuilder::append_static_position_marker(Box const& box)
{
    ensure_last_line_box().add_static_position_marker(box);
}

CSSPixels LineBuilder::ceiling_for_float_to_be_inserted_here(Box const& box)
{
    auto const& box_state = m_layout_state.get(box);
    CSSPixels const width = box_state.margin_box_width();

    CSSPixels candidate_block_offset = m_current_block_offset;

    // Determine the current line width and subtract trailing whitespace, since those have not yet been removed while
    // placing floating boxes.
    auto const& current_line = ensure_last_line_box();
    auto current_line_width = current_line.width() - current_line.get_trailing_whitespace_width();

    // If there's already inline content on the current line, check if the new float can fit
    // alongside the content. If not, place it on the next line.
    if (current_line_width > 0 && (current_line_width + width) > m_available_width_for_current_line)
        candidate_block_offset += current_line.height();

    return max(candidate_block_offset, m_context.vertical_float_clearance());
}

bool LineBuilder::should_break(CSSPixels next_item_width)
{
    if (m_available_width_for_current_line.is_max_content())
        return false;

    auto const& line_boxes = m_containing_block_used_values.line_boxes;
    if (line_boxes.is_empty() || line_boxes.last().is_empty()) {
        // If we don't have a single line box yet *and* there are no floats intruding
        // into this line box, we don't need to break before inserting anything.
        auto line_height = m_context.containing_block().computed_values().line_height();
        if (!m_context.any_floats_intrude_in_block_range(m_current_block_offset, m_current_block_offset + line_height))
            return false;
    }
    auto current_line_width = ensure_last_line_box().width();
    return (current_line_width + next_item_width) > m_available_width_for_current_line;
}

void LineBuilder::update_last_line()
{
    if (!m_last_line_needs_update)
        return;
    m_last_line_needs_update = false;

    auto& line_boxes = m_containing_block_used_values.line_boxes;
    if (line_boxes.is_empty())
        return;

    auto& line_box = line_boxes.last();

    auto text_align = m_context.containing_block().computed_values().text_align();
    auto direction = m_context.containing_block().computed_values().direction();

    auto current_line_height = max(m_max_height_on_current_line, m_context.containing_block().computed_values().line_height());
    CSSPixels start_inline_offset = m_context.leftmost_inline_offset_at(m_current_block_offset, current_line_height);
    CSSPixels inline_offset = start_inline_offset;
    CSSPixels block_offset = 0;

    // FIXME: Respect inline direction.
    CSSPixels excess_inline_space = m_available_width_for_current_line.to_px_or_zero() - line_box.inline_length();

    if (m_writing_mode != CSS::WritingMode::HorizontalTb) {
        block_offset = m_available_width_for_current_line.to_px_or_zero() - line_box.block_length();
    }

    // If (after justification, if any) the inline contents of a line box are too long to fit within it,
    // then the contents are start-aligned: any content that doesn't fit overflows the line box’s end edge.
    if (excess_inline_space > 0) {
        switch (text_align) {
        case CSS::TextAlign::Center:
        case CSS::TextAlign::LibwebCenter:
        case CSS::TextAlign::LibwebInheritOrCenter:
            inline_offset += excess_inline_space / 2;
            break;
        case CSS::TextAlign::Start:
            if (direction == CSS::Direction::Rtl)
                inline_offset += excess_inline_space;
            break;
        case CSS::TextAlign::End:
            if (direction == CSS::Direction::Ltr)
                inline_offset += excess_inline_space;
            break;
        case CSS::TextAlign::Right:
        case CSS::TextAlign::LibwebRight:
            inline_offset += excess_inline_space;
            break;
        case CSS::TextAlign::MatchParent:
            // This should have been replaced before this point.
            VERIFY_NOT_REACHED();
        case CSS::TextAlign::Left:
        case CSS::TextAlign::LibwebLeft:
        case CSS::TextAlign::Justify:
        default:
            break;
        }
    }

    auto baseline_for_font = [](Gfx::FontPixelMetrics const& font_metrics, CSSPixels line_height) {
        auto const typographic_height = CSSPixels::nearest_value_for(font_metrics.ascent + font_metrics.descent);
        auto const half_leading = (line_height - typographic_height) / 2;
        return CSSPixels::nearest_value_for(font_metrics.ascent) + half_leading;
    };

    auto strut_baseline = [&] {
        auto& font = m_context.containing_block().first_available_font();
        auto const line_height = m_context.containing_block().computed_values().line_height();
        return baseline_for_font(font.pixel_metrics(), line_height);
    }();

    bool should_align_strut_to_line_box_baseline = false;
    auto line_box_baseline = [&] {
        CSSPixels line_box_baseline = strut_baseline;
        for (auto& fragment : line_box.fragments()) {
            auto const line_height = fragment.layout_node().computed_values().line_height();

            CSSPixels fragment_baseline = 0;
            if (fragment.layout_node().is_text_node()) {
                fragment_baseline = baseline_for_font(fragment.layout_node().first_available_font().pixel_metrics(), line_height);
            } else {
                auto const& box = as<Layout::Box>(fragment.layout_node());
                fragment_baseline = m_context.box_baseline(box, FormattingContext::BaselineSet::Last);
            }

            // Remember the baseline used for this fragment. This will be used when painting the fragment.
            fragment.set_baseline(fragment_baseline);

            // NOTE: For fragments with a <length-percentage> vertical-align, shift the line box baseline down by the resolved amount.
            //       This ensures that we make enough vertical space on the line for any manually-aligned fragments.
            if (auto const* length_percentage = fragment.layout_node().computed_values().vertical_align().get_pointer<CSS::LengthPercentage>()) {
                fragment_baseline += length_percentage->to_px(line_height);
            }

            if (fragment_baseline > line_box_baseline) {
                if (!fragment.layout_node().is_text_node()) {
                    auto const& box = as<Layout::Box>(fragment.layout_node());
                    auto const& vertical_align = fragment.layout_node().computed_values().vertical_align();
                    should_align_strut_to_line_box_baseline |= box.display().is_inline_outside()
                        && box.display().is_flex_inside()
                        && vertical_align.has<CSS::VerticalAlign>()
                        && vertical_align.get<CSS::VerticalAlign>() == CSS::VerticalAlign::Baseline;
                }
                line_box_baseline = fragment_baseline;
            }
        }
        return line_box_baseline;
    }();

    // Start with the "strut", an imaginary zero-width box at the start of each line box.
    auto const strut_line_height = m_context.containing_block().computed_values().line_height();
    auto strut_top = m_current_block_offset;
    auto strut_bottom = should_align_strut_to_line_box_baseline
        ? m_current_block_offset + line_box_baseline + (strut_line_height - strut_baseline)
        : m_current_block_offset + strut_line_height;

    CSSPixels uppermost_box_top = strut_top;
    CSSPixels lowermost_box_bottom = strut_bottom;

    struct VerticalAlignMetrics {
        CSSPixels baseline { 0 };
        CSSPixels height { 0 };
        CSSPixels effective_box_top_offset { 0 };
        CSSPixels effective_box_bottom_offset { 0 };
        CSSPixels line_height { 0 };
    };

    auto block_offset_value_for_alignment = [&](Variant<CSS::VerticalAlign, CSS::LengthPercentage> const& vertical_align, VerticalAlignMetrics const& metrics) -> CSSPixels {
        auto alphabetic_baseline = m_current_block_offset + line_box_baseline - metrics.baseline + metrics.effective_box_top_offset;

        if (auto const* length_percentage = vertical_align.get_pointer<CSS::LengthPercentage>())
            return alphabetic_baseline - length_percentage->to_px(metrics.line_height);

        switch (vertical_align.get<CSS::VerticalAlign>()) {
        case CSS::VerticalAlign::Baseline:
            return alphabetic_baseline;
        case CSS::VerticalAlign::Top:
            return m_current_block_offset + metrics.effective_box_top_offset;
        case CSS::VerticalAlign::Middle: {
            // Align the vertical midpoint of the box with the baseline of the parent box
            // plus half the x-height of the parent.
            // FIXME: Per CSS2 §10.8.1 this should use the parent inline box's x-height, not the containing block's.
            auto const x_height = CSSPixels::nearest_value_for(m_context.containing_block().first_available_font().pixel_metrics().x_height);
            return m_current_block_offset + line_box_baseline + ((metrics.effective_box_top_offset - metrics.effective_box_bottom_offset - x_height - metrics.height) / 2);
        }
        case CSS::VerticalAlign::Sub:
            // https://drafts.csswg.org/css-inline/#valdef-baseline-shift-sub
            // Lower by the offset appropriate for subscripts of the parent’s box.
            // The UA may use the parent’s font metrics to find this offset; otherwise it defaults to dropping by one fifth of the parent’s used font-size.
            // FIXME: Use font metrics to find a more appropriate offset, if possible
            return alphabetic_baseline + m_context.containing_block().computed_values().font_size() / 5;
        case CSS::VerticalAlign::Super:
            // https://drafts.csswg.org/css-inline/#valdef-baseline-shift-super
            // Raise by the offset appropriate for superscripts of the parent’s box.
            // The UA may use the parent’s font metrics to find this offset; otherwise it defaults to raising by one third of the parent’s used font-size.
            // FIXME: Use font metrics to find a more appropriate offset, if possible
            return alphabetic_baseline - m_context.containing_block().computed_values().font_size() / 3;
        case CSS::VerticalAlign::Bottom:
        case CSS::VerticalAlign::TextBottom:
        case CSS::VerticalAlign::TextTop:
            // FIXME: These are all 'baseline'
            return alphabetic_baseline;
        }
        VERIFY_NOT_REACHED();
    };

    auto inline_box_alignment_metrics = [&](NodeWithStyle const& inline_box) {
        auto const line_height = inline_box.computed_values().line_height();
        auto const& used_values = m_layout_state.get(inline_box);
        return VerticalAlignMetrics {
            .baseline = baseline_for_font(inline_box.first_available_font().pixel_metrics(), line_height),
            .height = line_height,
            .effective_box_top_offset = used_values.border_box_top(),
            .effective_box_bottom_offset = used_values.border_box_bottom(),
            .line_height = line_height,
        };
    };

    for (auto& fragment : line_box.fragments()) {
        CSSPixels new_fragment_inline_offset = inline_offset + fragment.inline_offset();

        VerticalAlignMetrics fragment_metrics {
            .baseline = fragment.baseline(),
            .height = fragment.height(),
            .effective_box_top_offset = fragment.border_box_top(),
            .effective_box_bottom_offset = fragment.border_box_top(),
            .line_height = fragment.layout_node().computed_values().line_height(),
        };
        if (fragment.is_atomic_inline()) {
            auto const& fragment_box_state = m_layout_state.get(static_cast<Box const&>(fragment.layout_node()));
            fragment_metrics.effective_box_top_offset = fragment_box_state.margin_box_top();
            fragment_metrics.effective_box_bottom_offset = fragment_box_state.margin_box_bottom();
        }

        // Position the fragment according to the vertical-align of its own styled inline element.
        auto const& own_vertical_align = fragment.layout_node().computed_values().vertical_align();
        CSSPixels new_fragment_block_offset = block_offset_value_for_alignment(own_vertical_align, fragment_metrics);

        // A 'top'- or 'bottom'-aligned box forms the root of its own aligned subtree and is positioned relative to the
        // line box, so it must ignore the vertical-align of its ancestors.
        auto* own_alignment = own_vertical_align.get_pointer<CSS::VerticalAlign>();
        bool own_alignment_is_line_relative = own_alignment && first_is_one_of(*own_alignment, CSS::VerticalAlign::Top, CSS::VerticalAlign::Bottom);

        auto const& node = fragment.layout_node().has_style()
            ? static_cast<NodeWithStyle const&>(fragment.layout_node())
            : *fragment.layout_node().parent();
        auto const* containing_block = &m_context.containing_block();
        auto const* first_ancestor = &node == containing_block ? nullptr : node.parent();
        for (auto const* ancestor = first_ancestor; !own_alignment_is_line_relative && ancestor && ancestor->is_inline_node() && ancestor != containing_block; ancestor = ancestor->parent()) {
            auto const& ancestor_vertical_align = ancestor->computed_values().vertical_align();
            if (ancestor_vertical_align.has<CSS::VerticalAlign>()) {
                auto keyword = ancestor_vertical_align.get<CSS::VerticalAlign>();
                if (keyword == CSS::VerticalAlign::Baseline)
                    continue;
                // FIXME: Implement aligning a 'top'- or 'bottom'-aligned ancestor's aligned subtree to the line box
                if (first_is_one_of(keyword, CSS::VerticalAlign::Top, CSS::VerticalAlign::Bottom))
                    break;
            }
            auto ancestor_metrics = inline_box_alignment_metrics(*ancestor);
            new_fragment_block_offset += block_offset_value_for_alignment(ancestor_vertical_align, ancestor_metrics)
                - block_offset_value_for_alignment(CSS::VerticalAlign::Baseline, ancestor_metrics);
        }

        fragment.set_inline_offset(new_fragment_inline_offset);
        fragment.set_block_offset(floor(new_fragment_block_offset) + block_offset);

        CSSPixels top_of_inline_box = 0;
        CSSPixels bottom_of_inline_box = 0;
        {
            // FIXME: Support inline-table elements.
            if (fragment.is_atomic_inline()) {
                auto const& fragment_box_state = m_layout_state.get(static_cast<Box const&>(fragment.layout_node()));
                top_of_inline_box = (fragment.block_offset() - fragment_box_state.margin_box_top());
                bottom_of_inline_box = (fragment.block_offset() + fragment_box_state.content_height() + fragment_box_state.margin_box_bottom());
            } else {
                auto font_metrics = fragment.layout_node().first_available_font().pixel_metrics();
                auto typographic_height = CSSPixels::nearest_value_for(font_metrics.ascent + font_metrics.descent);
                auto leading = fragment.layout_node().computed_values().line_height() - typographic_height;
                auto half_leading = leading / 2;
                top_of_inline_box = (fragment.block_offset() + fragment.baseline() - CSSPixels::nearest_value_for(font_metrics.ascent) - half_leading);
                bottom_of_inline_box = (fragment.block_offset() + fragment.baseline() + CSSPixels::nearest_value_for(font_metrics.descent) + half_leading);
            }
            if (auto const* length_percentage = fragment.layout_node().computed_values().vertical_align().get_pointer<CSS::LengthPercentage>()) {
                bottom_of_inline_box += length_percentage->to_px(fragment.layout_node().computed_values().line_height());
            }
        }

        uppermost_box_top = min(uppermost_box_top, top_of_inline_box);
        lowermost_box_bottom = max(lowermost_box_bottom, bottom_of_inline_box);
    }

    auto marker_inline_offset = line_box.fragments().is_empty()
        ? start_inline_offset
        : inline_offset;
    for (auto& marker : line_box.static_position_markers()) {
        marker.inline_offset += marker_inline_offset;
        marker.block_offset += block_offset + m_current_block_offset;
    }

    // 3. The line box height is the distance between the uppermost box top and the lowermost box bottom.
    line_box.m_block_length = lowermost_box_bottom - uppermost_box_top;
    m_should_advance_to_last_line_box_bottom = should_align_strut_to_line_box_baseline;

    line_box.m_bottom = m_current_block_offset + line_box.m_block_length;
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
    m_available_width_for_current_line = m_context.available_space_for_line(m_current_block_offset, current_line_height);
    if (!m_containing_block_used_values.line_boxes.is_empty())
        m_containing_block_used_values.line_boxes.last().m_original_available_width = m_available_width_for_current_line;
}

void LineBuilder::did_introduce_clearance(CSSPixels clearance)
{
    // If clearance was introduced but our current line box starts beyond it, we don't need to do anything.
    if (clearance <= m_current_block_offset)
        return;

    // Increase the height of the previous line box so it matches the clearance, because the element's height is first
    // determined by the bottom of the last line box (after trimming empty/whitespace boxes).
    auto& line_boxes = m_containing_block_used_values.line_boxes;
    if (line_boxes.size() > 1) {
        auto& previous_line_box = line_boxes[line_boxes.size() - 2];
        previous_line_box.m_bottom = clearance;
    }

    // The current line box will start directly after any cleared floats.
    m_current_block_offset = clearance;
}

void LineBuilder::set_trailing_whitespace_on_previous_line()
{
    // When a line breaks at whitespace, that whitespace is not added to any line. For text
    // selection purposes, we record this on the last fragment of the previous line.
    auto& line_boxes = m_containing_block_used_values.line_boxes;
    if (line_boxes.size() < 2)
        return;

    auto& previous_line_box = line_boxes[line_boxes.size() - 2];
    if (previous_line_box.m_fragments.is_empty())
        return;

    auto& last_fragment = previous_line_box.m_fragments.last();
    last_fragment.set_has_trailing_whitespace(true);
}

}
