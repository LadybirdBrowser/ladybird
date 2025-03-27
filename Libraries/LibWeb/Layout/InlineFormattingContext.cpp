/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Length.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/InlineFormattingContext.h>
#include <LibWeb/Layout/InlineLevelIterator.h>
#include <LibWeb/Layout/LineBuilder.h>

namespace Web::Layout {

InlineFormattingContext::InlineFormattingContext(
    LayoutState& state,
    LayoutMode layout_mode,
    BlockContainer const& containing_block,
    LayoutState::UsedValues& containing_block_used_values,
    BlockFormattingContext& parent)
    : FormattingContext(Type::Inline, layout_mode, state, containing_block, &parent)
    , m_containing_block_used_values(containing_block_used_values)
{
}

InlineFormattingContext::~InlineFormattingContext() = default;

BlockFormattingContext& InlineFormattingContext::parent()
{
    return static_cast<BlockFormattingContext&>(*FormattingContext::parent());
}

BlockFormattingContext const& InlineFormattingContext::parent() const
{
    return static_cast<BlockFormattingContext const&>(*FormattingContext::parent());
}

CSSPixels InlineFormattingContext::leftmost_inline_offset_at(CSSPixels y) const
{
    // NOTE: Floats are relative to the BFC root box, not necessarily the containing block of this IFC.
    auto box_in_root_rect = content_box_rect_in_ancestor_coordinate_space(m_containing_block_used_values, parent().root());
    CSSPixels y_in_root = box_in_root_rect.y() + y;
    auto space_and_containing_margin = parent().space_used_and_containing_margin_for_floats(y_in_root);
    auto left_side_floats_limit_to_right = space_and_containing_margin.left_total_containing_margin + space_and_containing_margin.left_used_space;
    if (box_in_root_rect.x() >= left_side_floats_limit_to_right) {
        // The left edge of the containing block is to the right of the rightmost left-side float.
        // We start placing inline content at the left edge of the containing block.
        return 0;
    }
    // The left edge of the containing block is to the left of the rightmost left-side float.
    // We adjust the inline content insertion point by the overlap between the containing block and the float.
    return left_side_floats_limit_to_right - max(CSSPixels(0), box_in_root_rect.x());
}

AvailableSize InlineFormattingContext::available_space_for_line(CSSPixels y) const
{
    auto intrusions = parent().intrusion_by_floats_into_box(m_containing_block_used_values, y);
    if (m_available_space->width.is_definite()) {
        return AvailableSize::make_definite(m_available_space->width.to_px_or_zero() - (intrusions.left + intrusions.right));
    } else {
        return m_available_space->width;
    }
}

CSSPixels InlineFormattingContext::automatic_content_width() const
{
    return m_automatic_content_width;
}

CSSPixels InlineFormattingContext::automatic_content_height() const
{
    return m_automatic_content_height;
}

void InlineFormattingContext::run(AvailableSpace const& available_space)
{
    VERIFY(containing_block().children_are_inline());
    m_available_space = available_space;
    generate_line_boxes();

    CSSPixels content_height = 0;

    for (auto& line_box : m_containing_block_used_values.line_boxes)
        content_height += line_box.height();

    // NOTE: We ask the parent BFC to calculate the automatic content width of this IFC.
    //       This ensures that any floated boxes are taken into account.
    m_automatic_content_width = parent().greatest_child_width(containing_block());
    m_automatic_content_height = content_height;
}

void InlineFormattingContext::dimension_box_on_line(Box const& box, LayoutMode layout_mode)
{
    auto width_of_containing_block = m_available_space->width.to_px_or_zero();
    auto& box_state = m_state.get_mutable(box);
    auto const& computed_values = box.computed_values();

    box_state.margin_left = computed_values.margin().left().to_px(box, width_of_containing_block);
    box_state.border_left = computed_values.border_left().width;
    box_state.padding_left = computed_values.padding().left().to_px(box, width_of_containing_block);

    box_state.margin_right = computed_values.margin().right().to_px(box, width_of_containing_block);
    box_state.border_right = computed_values.border_right().width;
    box_state.padding_right = computed_values.padding().right().to_px(box, width_of_containing_block);

    box_state.margin_top = computed_values.margin().top().to_px(box, width_of_containing_block);
    box_state.border_top = computed_values.border_top().width;
    box_state.padding_top = computed_values.padding().top().to_px(box, width_of_containing_block);

    box_state.padding_bottom = computed_values.padding().bottom().to_px(box, width_of_containing_block);
    box_state.border_bottom = computed_values.border_bottom().width;
    box_state.margin_bottom = computed_values.margin().bottom().to_px(box, width_of_containing_block);

    if (box_is_sized_as_replaced_element(box)) {
        box_state.set_content_width(compute_width_for_replaced_element(box, *m_available_space));
        box_state.set_content_height(compute_height_for_replaced_element(box, *m_available_space));
        auto independent_formatting_context = layout_inside(box, layout_mode, box_state.available_inner_space_or_constraints_from(*m_available_space));
        if (independent_formatting_context)
            independent_formatting_context->parent_context_did_dimension_child_root_box();
        return;
    }

    // Any box that has simple flow inside should have generated line box fragments already.
    if (box.display().is_flow_inside()) {
        dbgln("FIXME: InlineFormattingContext::dimension_box_on_line got unexpected box in inline context:");
        dump_tree(box);
        return;
    }

    auto const& width_value = box.computed_values().width();
    CSSPixels unconstrained_width = 0;
    if (should_treat_width_as_auto(box, *m_available_space)) {
        auto result = calculate_shrink_to_fit_widths(box);

        if (m_available_space->width.is_definite()) {
            auto available_width = m_available_space->width.to_px_or_zero()
                - box_state.margin_left
                - box_state.border_left
                - box_state.padding_left
                - box_state.padding_right
                - box_state.border_right
                - box_state.margin_right;

            unconstrained_width = min(max(result.preferred_minimum_width, available_width), result.preferred_width);
        } else if (m_available_space->width.is_min_content()) {
            unconstrained_width = result.preferred_minimum_width;
        } else {
            unconstrained_width = result.preferred_width;
        }
    } else {
        if (width_value.contains_percentage() && !m_available_space->width.is_definite()) {
            // NOTE: We can't resolve percentages yet. We'll have to wait until after inner layout.
        } else {
            auto inner_width = calculate_inner_width(box, m_available_space->width, width_value);
            unconstrained_width = inner_width;
        }
    }

    CSSPixels width = unconstrained_width;
    if (!should_treat_max_width_as_none(box, m_available_space->width)) {
        auto max_width = calculate_inner_width(box, m_available_space->width, box.computed_values().max_width());
        width = min(width, max_width);
    }

    auto computed_min_width = box.computed_values().min_width();
    if (!computed_min_width.is_auto()) {
        auto min_width = calculate_inner_width(box, m_available_space->width, computed_min_width);
        width = max(width, min_width);
    }

    box_state.set_content_width(width);

    parent().resolve_used_height_if_not_treated_as_auto(box, AvailableSpace(AvailableSize::make_definite(width), AvailableSize::make_indefinite()));

    // NOTE: Flex containers with `auto` height are treated as `max-content`, so we can compute their height early.
    if (box.display().is_flex_inside())
        parent().resolve_used_height_if_treated_as_auto(box, AvailableSpace(AvailableSize::make_definite(width), AvailableSize::make_indefinite()));

    auto independent_formatting_context = layout_inside(box, layout_mode, box_state.available_inner_space_or_constraints_from(*m_available_space));

    if (should_treat_height_as_auto(box, *m_available_space)) {
        // FIXME: (10.6.6) If 'height' is 'auto', the height depends on the element's descendants per 10.6.7.
        parent().resolve_used_height_if_treated_as_auto(box, *m_available_space);
    } else {
        parent().resolve_used_height_if_not_treated_as_auto(box, *m_available_space);
    }

    if (independent_formatting_context)
        independent_formatting_context->parent_context_did_dimension_child_root_box();
}

void InlineFormattingContext::apply_justification_to_fragments(CSS::TextJustify text_justify, LineBox& line_box, bool is_last_line)
{
    switch (text_justify) {
    case CSS::TextJustify::None:
        return;
    // FIXME: These two cases currently fall back to auto, handle them as well.
    case CSS::TextJustify::InterCharacter:
    case CSS::TextJustify::InterWord:
    case CSS::TextJustify::Auto:
        break;
    }

    // https://www.w3.org/TR/css-text-3/#text-align-property
    // Unless otherwise specified by text-align-last, the last line before a forced break or the end of the block is start-aligned.
    // FIXME: Support text-align-last.
    if (is_last_line || line_box.m_has_forced_break)
        return;

    CSSPixels excess_horizontal_space = line_box.original_available_width().to_px_or_zero() - line_box.inline_length();
    CSSPixels excess_horizontal_space_including_whitespace = excess_horizontal_space;
    size_t whitespace_count = 0;
    for (auto& fragment : line_box.fragments()) {
        if (fragment.is_justifiable_whitespace()) {
            ++whitespace_count;
            excess_horizontal_space_including_whitespace += fragment.inline_length();
        }
    }

    CSSPixels justified_space_width = whitespace_count > 0 ? (excess_horizontal_space_including_whitespace / whitespace_count) : 0;

    // This is the amount that each fragment will be offset by. If a whitespace
    // fragment is shorter than the justified space width, it increases to push
    // subsequent fragments, and decreases to pull them back otherwise.
    CSSPixels running_diff = 0;
    for (size_t i = 0; i < line_box.fragments().size(); ++i) {
        auto& fragment = line_box.fragments()[i];
        fragment.set_inline_offset(fragment.inline_offset() + running_diff);

        if (fragment.is_justifiable_whitespace()
            && fragment.inline_length() != justified_space_width) {
            running_diff += justified_space_width - fragment.inline_length();
            fragment.set_inline_length(justified_space_width);
        }
    }
}

void InlineFormattingContext::generate_line_boxes()
{
    auto& line_boxes = m_containing_block_used_values.line_boxes;
    line_boxes.clear_with_capacity();

    auto direction = m_context_box->computed_values().direction();
    auto writing_mode = m_context_box->computed_values().writing_mode();

    InlineLevelIterator iterator(*this, m_state, containing_block(), m_containing_block_used_values, m_layout_mode);
    LineBuilder line_builder(*this, m_state, m_containing_block_used_values, direction, writing_mode);

    // NOTE: When we ignore collapsible whitespace chunks at the start of a line,
    //       we have to remember how much start margin that chunk had in the inline
    //       axis, so that we can add it to the first non-whitespace chunk.
    CSSPixels leading_margin_from_collapsible_whitespace = 0;

    Vector<Box const*> absolute_boxes;

    for (;;) {
        auto item_opt = iterator.next();
        if (!item_opt.has_value())
            break;
        auto& item = item_opt.value();

        // Ignore collapsible whitespace chunks at the start of line, and if the last fragment already ends in whitespace.
        if (item.is_collapsible_whitespace && (line_boxes.is_empty() || line_boxes.last().is_empty_or_ends_in_whitespace())) {
            if (item.node->computed_values().white_space() != CSS::WhiteSpace::Nowrap) {
                auto next_width = iterator.next_non_whitespace_sequence_width();
                if (next_width > 0)
                    line_builder.break_if_needed(next_width);
            }
            leading_margin_from_collapsible_whitespace += item.margin_start;
            continue;
        }

        item.margin_start += leading_margin_from_collapsible_whitespace;
        leading_margin_from_collapsible_whitespace = 0;

        switch (item.type) {
        case InlineLevelIterator::Item::Type::ForcedBreak: {
            line_builder.break_line(LineBuilder::ForcedBreak::Yes);
            if (item.node) {
                auto introduce_clearance = parent().clear_floating_boxes(*item.node, *this);
                if (introduce_clearance == BlockFormattingContext::DidIntroduceClearance::Yes) {
                    line_builder.set_current_block_offset(vertical_float_clearance());
                    parent().reset_margin_state();
                }
            }
            break;
        }
        case InlineLevelIterator::Item::Type::Element: {
            auto& box = as<Layout::Box>(*item.node);
            compute_inset(box, content_box_rect(m_containing_block_used_values).size());
            if (containing_block().computed_values().white_space() != CSS::WhiteSpace::Nowrap) {
                auto minimum_space_needed_on_line = item.border_box_width();
                if (item.margin_start < 0)
                    minimum_space_needed_on_line += item.margin_start;
                if (item.margin_end < 0)
                    minimum_space_needed_on_line += item.margin_end;
                line_builder.break_if_needed(minimum_space_needed_on_line);
            }
            line_builder.append_box(box, item.border_start + item.padding_start, item.padding_end + item.border_end, item.margin_start, item.margin_end);
            break;
        }
        case InlineLevelIterator::Item::Type::AbsolutelyPositionedElement:
            if (is<Box>(*item.node)) {
                auto const& box = static_cast<Layout::Box const&>(*item.node);
                // Calculation of static position for absolute boxes is delayed until trailing whitespaces are removed.
                absolute_boxes.append(&box);
            }
            break;

        case InlineLevelIterator::Item::Type::FloatingElement:
            if (is<Box>(*item.node)) {
                (void)parent().clear_floating_boxes(*item.node, *this);
                // Even if this introduces clearance, we do NOT reset the margin state, because that is clearance
                // between floats and does not contribute to the height of the Inline Formatting Context.
                parent().layout_floating_box(static_cast<Layout::Box const&>(*item.node), containing_block(), *m_available_space, 0, &line_builder);
            }
            break;

        case InlineLevelIterator::Item::Type::Text: {
            auto& text_node = as<Layout::TextNode>(*item.node);

            if (text_node.computed_values().white_space() != CSS::WhiteSpace::Nowrap) {
                bool is_whitespace = false;
                CSSPixels next_width = 0;
                // If we're in a whitespace-collapsing context, we can simply check the flag.
                if (item.is_collapsible_whitespace) {
                    is_whitespace = true;
                    next_width = iterator.next_non_whitespace_sequence_width();
                } else {
                    // In whitespace-preserving contexts (white-space: pre*), we have to check manually.
                    auto view = text_node.text_for_rendering().bytes_as_string_view().substring_view(item.offset_in_node, item.length_in_node);
                    is_whitespace = view.is_whitespace();
                    if (is_whitespace)
                        next_width = iterator.next_non_whitespace_sequence_width();
                }

                // If whitespace caused us to break, we swallow the whitespace instead of
                // putting it on the next line.
                if (is_whitespace && next_width > 0 && line_builder.break_if_needed(item.border_box_width() + next_width))
                    break;
            } else if (text_node.computed_values().text_overflow() == CSS::TextOverflow::Ellipsis
                && text_node.computed_values().overflow_x() != CSS::Overflow::Visible) {
                // We may need to do an ellipsis if the text is too long for the container
                constexpr u32 ellipsis_codepoint = 0x2026;
                if (m_available_space.has_value()
                    && item.width.to_double() > m_available_space.value().width.to_px_or_zero().to_double()) {
                    // Do the ellipsis
                    auto& glyph_run = item.glyph_run;

                    auto available_width = m_available_space.value().width.to_px_or_zero().to_double();
                    auto ellipsis_width = glyph_run->font().glyph_width(ellipsis_codepoint);
                    auto max_text_width = available_width - ellipsis_width;

                    auto& glyphs = glyph_run->glyphs();
                    size_t last_glyph_index = 0;
                    auto last_glyph_position = Gfx::FloatPoint();

                    for (auto const& glyph : glyphs) {
                        if (glyph.position.x() > max_text_width)
                            break;
                        last_glyph_index++;
                        last_glyph_position = glyph.position;
                    }

                    if (last_glyph_index > 1) {
                        auto remove_item_count = glyphs.size() - last_glyph_index;
                        glyphs.remove(last_glyph_index - 1, remove_item_count);
                        glyphs.append(Gfx::DrawGlyph {
                            .position = last_glyph_position,
                            .glyph_id = glyph_run->font().glyph_id_for_code_point(ellipsis_codepoint) });
                    }
                }
            }
            line_builder.append_text_chunk(
                text_node,
                item.offset_in_node,
                item.length_in_node,
                item.border_start + item.padding_start,
                item.padding_end + item.border_end,
                item.margin_start,
                item.margin_end,
                item.width,
                text_node.computed_values().line_height(),
                move(item.glyph_run));
            break;
        }
        }
    }

    for (auto& line_box : line_boxes)
        line_box.trim_trailing_whitespace();

    line_builder.remove_last_line_if_empty();

    auto const& containing_block = this->containing_block();
    auto text_align = containing_block.computed_values().text_align();
    auto text_justify = containing_block.computed_values().text_justify();
    if (text_align == CSS::TextAlign::Justify) {
        for (size_t i = 0; i < line_boxes.size(); i++) {
            auto& line_box = line_boxes[i];
            auto is_last_line = i == line_boxes.size() - 1;
            apply_justification_to_fragments(text_justify, line_box, is_last_line);
        }
    }

    for (auto& line_box : line_boxes) {
        for (auto& fragment : line_box.fragments()) {
            if (fragment.layout_node().is_inline_block()) {
                auto& box = as<Box>(fragment.layout_node());
                auto& box_state = m_state.get_mutable(box);
                box_state.set_content_offset(fragment.offset());
            }
        }
    }

    for (auto* box : absolute_boxes) {
        auto& box_state = m_state.get_mutable(*box);
        box_state.set_static_position_rect(calculate_static_position_rect(*box));
    }
}

bool InlineFormattingContext::any_floats_intrude_at_block_offset(CSSPixels block_offset) const
{
    auto box_in_root_rect = content_box_rect_in_ancestor_coordinate_space(m_containing_block_used_values, parent().root());
    // FIXME: Respect inline direction.
    CSSPixels y_in_root = box_in_root_rect.y() + block_offset;
    auto space_and_containing_margin = parent().space_used_and_containing_margin_for_floats(y_in_root);
    return space_and_containing_margin.left_used_space > 0 || space_and_containing_margin.right_used_space > 0;
}

bool InlineFormattingContext::can_fit_new_line_at_block_offset(CSSPixels block_offset) const
{
    // FIXME: Respect inline direction.

    auto top_intrusions = parent().intrusion_by_floats_into_box(m_containing_block_used_values, block_offset);
    auto bottom_intrusions = parent().intrusion_by_floats_into_box(m_containing_block_used_values, block_offset + containing_block().computed_values().line_height() - 1);

    auto left_edge = [](auto& space) -> CSSPixels {
        return space.left;
    };

    auto right_edge = [this](auto& space) -> CSSPixels {
        return m_available_space->width.to_px_or_zero() - space.right;
    };

    auto top_left_edge = left_edge(top_intrusions);
    auto top_right_edge = right_edge(top_intrusions);
    auto bottom_left_edge = left_edge(bottom_intrusions);
    auto bottom_right_edge = right_edge(bottom_intrusions);

    if (top_left_edge > bottom_right_edge)
        return false;
    if (bottom_left_edge > top_right_edge)
        return false;
    return true;
}

CSSPixels InlineFormattingContext::vertical_float_clearance() const
{
    return m_vertical_float_clearance;
}

void InlineFormattingContext::set_vertical_float_clearance(CSSPixels vertical_float_clearance)
{
    m_vertical_float_clearance = vertical_float_clearance;
}

StaticPositionRect InlineFormattingContext::calculate_static_position_rect(Box const& box) const
{
    CSSPixels x = 0;
    CSSPixels y = 0;

    VERIFY(box.parent());
    VERIFY(box.parent()->children_are_inline());
    // We're an abspos box with inline siblings. This is gonna get messy!
    if (auto const* sibling = box.previous_sibling()) {
        // Hard case: there's a previous sibling. This means there's already inline content
        // preceding the hypothetical static position of `box` within its containing block.
        // If we had been position:static, that inline content would have been wrapped in
        // anonymous block box, so now we get to imagine what the world might have looked like
        // in that scenario..
        // Basically, we find its last associated line box fragment and place `box` under it.
        // FIXME: I'm 100% sure this can be smarter, better and faster.
        LineBoxFragment const* last_fragment = nullptr;
        auto const& cb_state = m_state.get(*sibling->containing_block());
        for (auto const& line_box : cb_state.line_boxes) {
            for (auto const& fragment : line_box.fragments()) {
                if (&fragment.layout_node() == sibling)
                    last_fragment = &fragment;
            }
        }
        if (last_fragment) {
            x = last_fragment->offset().x() + last_fragment->width();
            y = last_fragment->offset().y() + last_fragment->height();
        }
    } else {
        // Easy case: no previous sibling, we're at the top of the containing block.
    }
    StaticPositionRect static_position_rect;
    static_position_rect.rect = { { x, y }, { 0, 0 } };
    return static_position_rect;
}

}
