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

CSSPixels InlineFormattingContext::leftmost_inline_offset_at(CSSPixels block_offset, CSSPixels line_height) const
{
    auto intrusions = parent().intrusion_by_floats_into_box(m_containing_block_used_values, block_offset, block_offset + line_height);
    return intrusions.left;
}

AvailableSize InlineFormattingContext::available_space_for_line(CSSPixels block_offset, CSSPixels line_height) const
{
    if (!m_available_space->width.is_definite())
        return m_available_space->width;

    auto intrusions = parent().intrusion_by_floats_into_box(m_containing_block_used_values, block_offset, block_offset + line_height);
    return AvailableSize::make_definite(m_available_space->width.to_px_or_zero() - intrusions.left - intrusions.right);
}

CSSPixels InlineFormattingContext::automatic_content_width() const
{
    return m_automatic_content_width;
}

CSSPixels InlineFormattingContext::automatic_content_height() const
{
    return m_automatic_content_height;
}

void InlineFormattingContext::run(LayoutInput const& layout_input)
{
    auto const& available_space = layout_input.available_space;
    FORMATTING_CONTEXT_TRACE();
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

    box_state.margin_left = computed_values.margin().left().to_px_or_zero(width_of_containing_block);
    box_state.border_left = computed_values.border_left().width;
    box_state.padding_left = computed_values.padding().left().to_px_or_zero(width_of_containing_block);

    box_state.margin_right = computed_values.margin().right().to_px_or_zero(width_of_containing_block);
    box_state.border_right = computed_values.border_right().width;
    box_state.padding_right = computed_values.padding().right().to_px_or_zero(width_of_containing_block);

    box_state.margin_top = computed_values.margin().top().to_px_or_zero(width_of_containing_block);
    box_state.border_top = computed_values.border_top().width;
    box_state.padding_top = computed_values.padding().top().to_px_or_zero(width_of_containing_block);

    box_state.padding_bottom = computed_values.padding().bottom().to_px_or_zero(width_of_containing_block);
    box_state.border_bottom = computed_values.border_bottom().width;
    box_state.margin_bottom = computed_values.margin().bottom().to_px_or_zero(width_of_containing_block);

    if (box_is_sized_as_replaced_element(box, *m_available_space)) {
        box_state.set_content_width(compute_width_for_replaced_element(box, *m_available_space));
        box_state.set_content_height(compute_height_for_replaced_element(box, *m_available_space));
        auto child_layout_input = LayoutInput { box_state.available_inner_space_or_constraints_from(*m_available_space) };
        auto independent_formatting_context = layout_inside(box, layout_mode, child_layout_input);
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
        if (m_available_space->width.is_definite()) {
            auto available_width = m_available_space->width.to_px_or_zero()
                - box_state.margin_left
                - box_state.border_left
                - box_state.padding_left
                - box_state.padding_right
                - box_state.border_right
                - box_state.margin_right;

            auto preferred_width = calculate_max_content_width(box);
            if (preferred_width <= available_width) {
                unconstrained_width = preferred_width;
            } else {
                auto preferred_minimum_width = calculate_min_content_width(box);
                unconstrained_width = min(max(preferred_minimum_width, available_width), preferred_width);
            }
        } else if (m_available_space->width.is_min_content()) {
            unconstrained_width = calculate_min_content_width(box);
        } else {
            unconstrained_width = calculate_max_content_width(box);
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

    auto child_layout_input = LayoutInput { box_state.available_inner_space_or_constraints_from(*m_available_space) };
    auto independent_formatting_context = layout_inside(box, layout_mode, child_layout_input);

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
            auto diff = justified_space_width - fragment.inline_length();
            running_diff += diff;
            for (auto& marker : line_box.static_position_markers()) {
                if (marker.inline_offset > fragment.inline_offset())
                    marker.inline_offset += diff;
            }
            fragment.set_inline_length(justified_space_width);
        }
    }
}

// https://drafts.csswg.org/css-overflow-4/#text-overflow
void InlineFormattingContext::apply_text_overflow_ellipsis(Vector<LineBox>& line_boxes)
{
    // This property specifies rendering when inline content overflows its line box edge in the inline progression
    // direction of its block container element ("the block") that has overflow other than visible.

    // NB: When inline children are wrapped in anonymous blocks (e.g. due to floats), we look past the anonymous
    //     wrapper to the actual element that has text-overflow and overflow set.
    Box const* block = &containing_block();
    if (block->is_anonymous())
        block = block->non_anonymous_containing_block();

    // FIXME: Support the <string>, fade, and fade() values, as well as the two-value syntax.
    auto const& block_values = block->computed_values();
    if (block_values.text_overflow() != CSS::TextOverflow::Ellipsis)
        return;
    if (block_values.overflow_x() == CSS::Overflow::Visible)
        return;

    // Render an ellipsis character (U+2026) to represent clipped inline content.
    constexpr u32 ellipsis_codepoint = 0x2026;

    for (auto& line_box : line_boxes) {
        // NB: Use the line box's original available width rather than the IFC's available space, since the line's
        //     usable width may be reduced by float intrusions.
        if (!line_box.original_available_width().is_definite())
            continue;
        auto available_width = line_box.original_available_width().to_px_or_zero();
        if (line_box.inline_length() <= available_width)
            continue;

        auto& fragments = line_box.fragments();
        if (fragments.is_empty())
            continue;

        // For the ellipsis and string values, implementations must hide characters and atomic inline-level elements at
        // the applicable edge(s) of the line as necessary to fit the ellipsis/string, and place the ellipsis/string
        // immediately adjacent to the applicable edge(s) of the remaining inline content.
        bool line_has_visible_content = false;
        for (size_t i = 0; i < fragments.size(); i++) {
            auto& fragment = fragments[i];
            auto fragment_start = fragment.inline_offset();
            auto fragment_end = fragment_start + fragment.inline_length();

            if (fragment_end <= available_width) {
                line_has_visible_content = true;
                continue;
            }

            // NB: We skip non-text fragments (atomic inlines) for now. Hiding them entirely to make room for the
            //     ellipsis is not yet implemented.
            if (!fragment.glyph_run())
                continue;

            auto& font = fragment.glyph_run()->font();
            auto ellipsis_width = font.glyph_width(ellipsis_codepoint);
            auto available_in_fragment = (available_width - fragment_start).to_float();
            auto max_text_width = available_in_fragment - ellipsis_width;

            auto& glyphs = fragment.glyph_run()->glyphs();
            size_t keep_count = 0;
            float last_kept_end = 0.f;
            float y_position = 0.f;

            // https://drafts.csswg.org/css-overflow-4/#auto-ellipsis
            // The first character or atomic inline-level element on a line must be clipped rather than ellipsed.
            for (auto const& glyph : glyphs) {
                auto glyph_end = glyph.position.x() + glyph.glyph_width;
                if (glyph_end > max_text_width && (keep_count > 0 || line_has_visible_content))
                    break;
                keep_count++;
                last_kept_end = glyph_end;
                y_position = glyph.position.y();
            }

            if (keep_count < glyphs.size())
                glyphs.remove(keep_count, glyphs.size() - keep_count);

            glyphs.append(Gfx::DrawGlyph {
                .position = { last_kept_end, y_position },
                .length_in_code_units = AK::UnicodeUtils::code_unit_length_for_code_point(ellipsis_codepoint),
                .glyph_width = ellipsis_width,
                .glyph_id = font.glyph_id_for_code_point(ellipsis_codepoint),
            });

            fragment.set_inline_length(CSSPixels::nearest_value_for(last_kept_end + ellipsis_width));

            for (size_t j = i + 1; j < fragments.size(); ++j)
                fragments[j].set_fully_truncated(true);

            line_box.m_inline_length = available_width;
            line_box.clamp_static_position_markers_to_inline_length();
            break;
        }
    }
}

void InlineFormattingContext::generate_line_boxes()
{
    auto& line_boxes = m_containing_block_used_values.line_boxes;
    line_boxes.clear_with_capacity();

    auto direction = m_context_box.computed_values().direction();
    auto writing_mode = m_context_box.computed_values().writing_mode();

    InlineLevelIterator iterator(*this, m_state, containing_block(), m_containing_block_used_values, m_layout_mode);
    LineBuilder line_builder(*this, m_state, m_containing_block_used_values, direction, writing_mode);

    // NOTE: When we ignore collapsible whitespace chunks at the start of a line,
    //       we have to remember how much start margin, border and padding that chunk had
    //       in the inline axis, so that we can add it to the first non-whitespace chunk.
    CSSPixels leading_margin_from_collapsible_whitespace = 0;
    CSSPixels leading_border_from_collapsible_whitespace = 0;
    CSSPixels leading_padding_from_collapsible_whitespace = 0;

    Vector<Box const*> absolute_boxes;

    for (;;) {
        auto item_opt = iterator.next();
        if (!item_opt.has_value())
            break;
        auto& item = item_opt.value();

        // Ignore collapsible whitespace chunks at the start of line, and if the last fragment already ends in whitespace.
        if (item.is_collapsible_whitespace && (line_boxes.is_empty() || line_boxes.last().is_empty_or_ends_in_whitespace())) {
            if (item.node->computed_values().text_wrap_mode() == CSS::TextWrapMode::Wrap) {
                auto next_width = iterator.next_non_whitespace_sequence_width();
                if (next_width > 0)
                    line_builder.break_if_needed(next_width);
            }
            leading_margin_from_collapsible_whitespace += item.margin_start;
            leading_border_from_collapsible_whitespace += item.border_start;
            leading_padding_from_collapsible_whitespace += item.padding_start;
            continue;
        }

        item.margin_start += leading_margin_from_collapsible_whitespace;
        leading_margin_from_collapsible_whitespace = 0;
        item.border_start += leading_border_from_collapsible_whitespace;
        leading_border_from_collapsible_whitespace = 0;
        item.padding_start += leading_padding_from_collapsible_whitespace;
        leading_padding_from_collapsible_whitespace = 0;

        switch (item.type) {
        case InlineLevelIterator::Item::Type::ForcedBreak: {
            line_builder.break_line(LineBuilder::ForcedBreak::Yes);
            if (item.node) {
                auto introduce_clearance = parent().clear_floating_boxes(*item.node, *this);
                if (introduce_clearance == BlockFormattingContext::DidIntroduceClearance::Yes) {
                    line_builder.did_introduce_clearance(vertical_float_clearance());
                    parent().reset_margin_state();
                }
            }
            break;
        }
        case InlineLevelIterator::Item::Type::Element: {
            auto& box = as<Layout::Box>(*item.node);
            compute_inset(box, content_box_rect(m_containing_block_used_values).size());
            if (containing_block().computed_values().text_wrap_mode() == CSS::TextWrapMode::Wrap) {
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
            if (auto const* box = as_if<Box>(*item.node)) {
                line_builder.append_static_position_marker(*box);
                absolute_boxes.append(box);
            }
            break;

        case InlineLevelIterator::Item::Type::FloatingElement:
            if (auto* box = as_if<Box>(*item.node)) {
                (void)parent().clear_floating_boxes(*item.node, *this);
                // Even if this introduces clearance, we do NOT reset the margin state, because that is clearance
                // between floats and does not contribute to the height of the Inline Formatting Context.
                parent().layout_floating_box(*box, containing_block(), *m_available_space, 0, &line_builder);
            }
            break;

        case InlineLevelIterator::Item::Type::Text: {
            auto& text_node = as<Layout::TextNode>(*item.node);

            if (text_node.computed_values().text_wrap_mode() == CSS::TextWrapMode::Wrap) {
                bool is_whitespace = false;
                CSSPixels next_width = 0;
                // If we're in a whitespace-collapsing context, we can simply check the flag.
                if (item.is_collapsible_whitespace) {
                    is_whitespace = true;
                    next_width = iterator.next_non_whitespace_sequence_width();
                } else {
                    // In whitespace-preserving contexts (white-space: pre*), we have to check manually.
                    auto view = text_node.text_for_rendering().substring_view(item.offset_in_node, item.length_in_node);
                    is_whitespace = view.is_ascii_whitespace();
                    if (is_whitespace)
                        next_width = iterator.next_non_whitespace_sequence_width();
                }

                // If whitespace caused us to break, don't put it on the next line.
                if (is_whitespace && next_width > 0 && line_builder.break_if_needed(item.border_box_width() + next_width)) {
                    // Record that the previous line has trailing whitespace for text selection.
                    line_builder.set_trailing_whitespace_on_previous_line();
                    break;
                }

                // https://drafts.csswg.org/css2/#floats
                // If a shortened line box is too small to contain any content, then the line box is shifted downward
                // (and its width recomputed) until either some content fits or there are no more floats present.
                if (!is_whitespace && (item.can_break_before || line_boxes.last().is_empty()))
                    line_builder.break_if_needed(item.border_box_width());
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

    apply_text_overflow_ellipsis(line_boxes);

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

    line_builder.update_last_line();

    if (m_layout_mode == LayoutMode::Normal) {
        for (auto const* box : absolute_boxes) {
            StaticPositionRect static_position_rect;
            bool found_static_position_marker = false;
            for (auto const& line_box : line_boxes) {
                for (auto const& marker : line_box.static_position_markers()) {
                    if (marker.box != box)
                        continue;

                    if (box->display_before_box_type_transformation().is_block_outside()) {
                        auto block_position = line_box.fragments().is_empty() ? marker.offset().y() : line_box.bottom();
                        static_position_rect.rect = { { 0, block_position }, { 0, 0 } };
                    } else {
                        static_position_rect.rect = { marker.offset(), { 0, 0 } };
                    }
                    found_static_position_marker = true;
                    break;
                }
                if (found_static_position_marker)
                    break;
            }
            auto& box_state = m_state.get_mutable(*box);
            box_state.set_static_position_rect(static_position_rect);
        }
    }

    line_builder.remove_last_line_if_empty();
    m_containing_block_used_values.set_inline_end_static_position_rect(calculate_inline_end_static_position_rect());
}

bool InlineFormattingContext::any_floats_intrude_in_block_range(CSSPixels block_start, CSSPixels block_end) const
{
    // FIXME: Respect inline direction.
    auto intrusions = parent().intrusion_by_floats_into_box(m_containing_block_used_values, block_start, block_end);
    return intrusions.left > 0 || intrusions.right > 0;
}

bool InlineFormattingContext::can_fit_new_line_at_block_offset(CSSPixels block_offset, CSSPixels line_height) const
{
    // FIXME: Respect inline direction.

    if (!m_available_space->width.is_definite())
        return true;
    return available_space_for_line(block_offset, line_height).to_px_or_zero() > 0;
}

Optional<CSSPixels> InlineFormattingContext::next_float_band_block_start_after(CSSPixels block_offset) const
{
    auto box_in_root_rect = content_box_rect_in_ancestor_coordinate_space(m_containing_block_used_values, parent().root());
    auto next_band_start = parent().next_float_band_block_start_after(box_in_root_rect.y() + block_offset);
    if (!next_band_start.has_value())
        return {};
    return next_band_start.value() - box_in_root_rect.y();
}

CSSPixels InlineFormattingContext::vertical_float_clearance() const
{
    return m_vertical_float_clearance;
}

void InlineFormattingContext::set_vertical_float_clearance(CSSPixels vertical_float_clearance)
{
    m_vertical_float_clearance = vertical_float_clearance;
}

StaticPositionRect InlineFormattingContext::calculate_inline_end_static_position_rect() const
{
    CSSPixels logical_inline_position = 0;
    CSSPixels logical_block_position = 0;

    auto to_physical_position = [](CSS::WritingMode writing_mode, CSSPixels logical_inline_position, CSSPixels logical_block_position) {
        if (writing_mode != CSS::WritingMode::HorizontalTb)
            return CSSPixelPoint { logical_block_position, logical_inline_position };
        return CSSPixelPoint { logical_inline_position, logical_block_position };
    };
    auto writing_mode = containing_block().computed_values().writing_mode();

    if (m_containing_block_used_values.line_boxes.is_empty())
        return { .rect = { to_physical_position(writing_mode, logical_inline_position, logical_block_position), { 0, 0 } } };

    CSSPixels line_boxes_bottom = 0;
    for (auto const& line_box : m_containing_block_used_values.line_boxes)
        line_boxes_bottom = max(line_boxes_bottom, line_box.bottom());

    auto const& last_line_box = m_containing_block_used_values.line_boxes.last();
    if (last_line_box.has_forced_break()) {
        logical_block_position = line_boxes_bottom;
        return { .rect = { to_physical_position(writing_mode, logical_inline_position, logical_block_position), { 0, 0 } } };
    }

    if (last_line_box.fragments().is_empty()) {
        logical_block_position = line_boxes_bottom;
        return { .rect = { to_physical_position(writing_mode, logical_inline_position, logical_block_position), { 0, 0 } } };
    }

    auto const& last_fragment = last_line_box.fragments().last();
    auto direction = containing_block().computed_values().direction();
    if (containing_block().is_anonymous() && containing_block().parent())
        direction = containing_block().parent()->computed_values().direction();

    if (direction == CSS::Direction::Rtl) {
        logical_inline_position = last_fragment.inline_offset();
    } else {
        auto last_fragment_visual_inline_end = last_fragment.inline_offset() + last_fragment.inline_length();
        logical_inline_position = max(last_fragment_visual_inline_end, last_line_box.inline_length());
    }
    logical_block_position = last_fragment.block_offset();

    return { .rect = { to_physical_position(last_fragment.writing_mode(), logical_inline_position, logical_block_position), { 0, 0 } } };
}

}
