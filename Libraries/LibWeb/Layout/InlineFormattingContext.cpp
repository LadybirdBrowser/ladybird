/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/QuickSort.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/InlineFormattingContext.h>
#include <LibWeb/Layout/InlineLevelIterator.h>
#include <LibWeb/Layout/LineBuilder.h>
#include <LibWeb/Layout/ListItemMarkerBox.h>

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

FormattingContext::SpaceUsedByFloats InlineFormattingContext::intrusion_by_floats_into_containing_block(CSSPixels block_start, CSSPixels block_end) const
{
    auto containing_block_position_in_root_now = m_layout_input->content_box_position_in_bfc_root->translated(0, parent().y_adjustment_from_pending_ancestor_top_margins(containing_block()));
    return parent().intrusion_by_floats_into_rect({ containing_block_position_in_root_now, m_containing_block_used_values.content_size() }, block_start, block_end);
}

CSSPixels InlineFormattingContext::leftmost_inline_offset_at(CSSPixels block_offset, CSSPixels line_height) const
{
    auto intrusions = intrusion_by_floats_into_containing_block(block_offset, block_offset + line_height);
    return intrusions.left;
}

AvailableSize InlineFormattingContext::available_space_for_line(CSSPixels block_offset, CSSPixels line_height) const
{
    if (!m_available_space->width.is_definite())
        return m_available_space->width;

    auto intrusions = intrusion_by_floats_into_containing_block(block_offset, block_offset + line_height);
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
    m_layout_input.emplace(layout_input);
    generate_line_boxes();
    compute_inline_box_pieces();

    auto const& line_boxes = m_containing_block_used_values.line_boxes;
    CSSPixels content_height = 0;
    if (any_of(line_boxes, [](auto& line_box) { return line_box.has_block_level_box(); })) {
        content_height = line_boxes.last().bottom();
    } else {
        for (auto& line_box : line_boxes)
            content_height += line_box.height();
    }

    // NOTE: We ask the parent BFC to calculate the automatic content width of this IFC.
    //       This ensures that any floated boxes are taken into account.
    auto provisional_containing_block_position_in_root = m_layout_input->content_box_position_in_bfc_root->translated(
        0, parent().y_adjustment_from_pending_ancestor_top_margins(containing_block()));
    m_automatic_content_width = parent().greatest_child_width_in_rect(
        containing_block(), { provisional_containing_block_position_in_root, m_containing_block_used_values.content_size() });
    m_automatic_content_height = content_height;

    compute_and_store_baselines(m_containing_block_used_values);
}

// Must run after all fragment-mutating post-passes (alignment, whitespace trimming, ellipsis,
// justification), since piece rects are derived from final fragment geometry.
void InlineFormattingContext::compute_inline_box_pieces()
{
    if (m_layout_mode != LayoutMode::Normal || m_state.is_for_measurement())
        return;

    auto const& line_boxes = m_containing_block_used_values.line_boxes;
    auto& pieces = m_containing_block_used_values.inline_box_pieces;
    pieces.clear_with_capacity();

    bool inline_axis_is_horizontal = containing_block().computed_values().writing_mode() == CSS::WritingMode::HorizontalTb;

    struct PerLine {
        explicit PerLine(size_t a_line_index)
            : line_index(a_line_index)
        {
        }

        size_t line_index { 0 };
        bool has_contributions { false };
        CSSPixels contributions_inline_start { 0 };
        CSSPixels contributions_inline_end { 0 };
        Optional<CSSPixels> first_direct_fragment_block_start;
        CSSPixels max_direct_fragment_block_length { 0 };
        Optional<CSSPixels> fallback_block_start_from_contributions;
        Optional<CSSPixelPoint> interrupting_block_position;
        Optional<u32> first_fragment_index;
        u32 fragment_count { 0 };
    };
    struct PerNode {
        PerNode(NodeWithStyleAndBoxModelMetrics const& a_node, u32 a_depth)
            : node(&a_node)
            , depth(a_depth)
        {
        }

        NodeWithStyleAndBoxModelMetrics const* node { nullptr };
        Optional<size_t> parent_index;
        u32 depth { 0 };
        Vector<PerLine, 2> lines;
        Optional<size_t> first_line_with_content;
        Optional<size_t> last_line_with_content;
    };

    Vector<PerNode> per_node_data;
    HashMap<NodeWithStyleAndBoxModelMetrics const*, size_t> node_to_index;

    auto fragmented_inline_nesting_depth = [](Node const& node) -> u32 {
        u32 depth = 1;
        for (auto const* ancestor = node.nearest_fragmented_inline_ancestor(); ancestor; ancestor = ancestor->nearest_fragmented_inline_ancestor())
            ++depth;
        return depth;
    };

    auto ensure_node = [&](NodeWithStyleAndBoxModelMetrics const& node) -> size_t {
        return node_to_index.ensure(&node, [&] {
            per_node_data.empend(node, fragmented_inline_nesting_depth(node));
            return per_node_data.size() - 1;
        });
    };

    auto ensure_line = [&](PerNode& per_node, size_t line_index) -> PerLine& {
        // Lines are visited mostly in ascending order; search from the back.
        size_t insertion_index = per_node.lines.size();
        while (insertion_index > 0 && per_node.lines[insertion_index - 1].line_index > line_index)
            --insertion_index;
        if (insertion_index > 0 && per_node.lines[insertion_index - 1].line_index == line_index)
            return per_node.lines[insertion_index - 1];
        per_node.lines.insert(insertion_index, PerLine { line_index });
        return per_node.lines[insertion_index];
    };

    auto note_contribution = [](PerLine& line, CSSPixels inline_start, CSSPixels inline_end, CSSPixels block_start) {
        if (!line.has_contributions || inline_start < line.contributions_inline_start)
            line.fallback_block_start_from_contributions = block_start;
        if (!line.has_contributions) {
            line.has_contributions = true;
            line.contributions_inline_start = inline_start;
            line.contributions_inline_end = inline_end;
        } else {
            line.contributions_inline_start = min(line.contributions_inline_start, inline_start);
            line.contributions_inline_end = max(line.contributions_inline_end, inline_end);
        }
    };

    // Fragment indexes must count exactly the fragments LayoutState::commit() will store on the
    // containing block's paintable, which skips the fully truncated ones.
    u32 committed_fragment_index = 0;
    for (size_t line_index = 0; line_index < line_boxes.size(); ++line_index) {
        auto const& line_box = line_boxes[line_index];
        for (auto const& fragment : line_box.fragments()) {
            if (fragment.is_fully_truncated())
                continue;
            auto fragment_index = committed_fragment_index++;
            auto const& fragment_node = fragment.layout_node();
            bool is_interrupting_block_fragment = fragment.style_source().display().is_block_outside();

            auto fragment_position = fragment.offset();
            auto fragment_size = fragment.size();
            CSSPixels inline_start = inline_axis_is_horizontal ? fragment_position.x() : fragment_position.y();
            CSSPixels inline_end = inline_start + (inline_axis_is_horizontal ? fragment_size.width() : fragment_size.height());
            CSSPixels block_start = inline_axis_is_horizontal ? fragment_position.y() : fragment_position.x();
            CSSPixels block_length = inline_axis_is_horizontal ? fragment_size.height() : fragment_size.width();

            // An atomic inline's fragment rect covers only its content box; what contributes to
            // ancestor extents on the inline axis is its margin box.
            if (!is_interrupting_block_fragment && fragment.is_atomic_inline()) {
                if (auto const* atomic_used_values = m_state.try_get(fragment_node)) {
                    inline_start -= inline_axis_is_horizontal ? atomic_used_values->margin_box_left() : atomic_used_values->margin_box_top();
                    inline_end += inline_axis_is_horizontal ? atomic_used_values->margin_box_right() : atomic_used_values->margin_box_bottom();
                }
            }

            bool is_direct_ancestor = true;
            Optional<size_t> previous_node_index;
            for (auto const* ancestor = fragment_node.nearest_fragmented_inline_ancestor(); ancestor; ancestor = ancestor->nearest_fragmented_inline_ancestor()) {
                auto node_index = ensure_node(*ancestor);
                if (previous_node_index.has_value())
                    per_node_data[*previous_node_index].parent_index = node_index;
                previous_node_index = node_index;

                auto& per_node = per_node_data[node_index];
                auto& line = ensure_line(per_node, line_index);

                // Fragments of one box are contiguous within a line, so a range suffices.
                if (!line.first_fragment_index.has_value())
                    line.first_fragment_index = fragment_index;
                line.fragment_count = fragment_index + 1 - *line.first_fragment_index;

                if (is_interrupting_block_fragment) {
                    if (!line.interrupting_block_position.has_value())
                        line.interrupting_block_position = fragment_position;
                    continue;
                }

                if (is_direct_ancestor) {
                    note_contribution(line, inline_start, inline_end, block_start);
                    if (!line.first_direct_fragment_block_start.has_value())
                        line.first_direct_fragment_block_start = block_start;
                    line.max_direct_fragment_block_length = max(line.max_direct_fragment_block_length, block_length);
                } else if (!line.fallback_block_start_from_contributions.has_value()) {
                    line.fallback_block_start_from_contributions = block_start;
                }
                if (!per_node.first_line_with_content.has_value())
                    per_node.first_line_with_content = line_index;
                per_node.last_line_with_content = line_index;
                is_direct_ancestor = false;
            }
        }
    }

    // Fragmented inlines that produced no fragments at all (e.g. spans with no in-flow content)
    // still get a placeholder piece, so DOM geometry queries have something to answer from.
    Vector<NodeWithStyleAndBoxModelMetrics const*> fragmented_inlines_without_fragments;
    for (auto const* fragmented_inline : m_fragmented_inlines_in_pre_order) {
        if (fragmented_inline->dom_node() && !node_to_index.contains(fragmented_inline))
            fragmented_inlines_without_fragments.append(fragmented_inline);
    }

    struct StagedPiece {
        Painting::InlineBoxPiece piece;
        u32 line_index { 0 };
        u32 depth { 0 };
        size_t discovery_index { 0 };
    };
    Vector<StagedPiece> staged_pieces;

    // `low`/`high` below mean the physically lesser/greater-coordinate side of the inline axis:
    // left/right for a horizontal inline axis, top/bottom for a vertical one.
    using Edge = Painting::InlineBoxPiece::Edge;
    u8 const block_axis_edges_present_on_every_piece = inline_axis_is_horizontal
        ? to_underlying(Edge::Top) | to_underlying(Edge::Bottom)
        : to_underlying(Edge::Left) | to_underlying(Edge::Right);

    auto edge_bits = [&](bool has_low_edge, bool has_high_edge) -> u8 {
        u8 edges = block_axis_edges_present_on_every_piece;
        if (has_low_edge)
            edges |= to_underlying(inline_axis_is_horizontal ? Edge::Left : Edge::Top);
        if (has_high_edge)
            edges |= to_underlying(inline_axis_is_horizontal ? Edge::Right : Edge::Bottom);
        return edges;
    };

    // Nested boxes' pieces expand their ancestors' extents, so pieces are emitted deepest-first.
    Vector<size_t> deepest_first_emission_order;
    deepest_first_emission_order.ensure_capacity(per_node_data.size());
    for (size_t i = 0; i < per_node_data.size(); ++i)
        deepest_first_emission_order.append(i);
    quick_sort(deepest_first_emission_order, [&](size_t a, size_t b) {
        if (per_node_data[a].depth != per_node_data[b].depth)
            return per_node_data[a].depth > per_node_data[b].depth;
        return a < b;
    });

    for (auto node_index : deepest_first_emission_order) {
        auto& per_node = per_node_data[node_index];
        auto const& node = *per_node.node;
        auto const* used_values = m_state.try_get(node);

        bool node_inline_axis_is_reversed = node.computed_values().inline_axis_is_reverse();

        CSSPixels border_padding_low = 0;
        CSSPixels border_padding_high = 0;
        CSSPixels border_padding_block_low = 0;
        CSSPixels border_padding_block_high = 0;
        CSSPixels margin_low = 0;
        CSSPixels margin_high = 0;
        if (used_values) {
            if (inline_axis_is_horizontal) {
                border_padding_low = used_values->border_box_left();
                border_padding_high = used_values->border_box_right();
                border_padding_block_low = used_values->border_box_top();
                border_padding_block_high = used_values->border_box_bottom();
                margin_low = used_values->margin_left;
                margin_high = used_values->margin_right;
            } else {
                border_padding_low = used_values->border_box_top();
                border_padding_high = used_values->border_box_bottom();
                border_padding_block_low = used_values->border_box_left();
                border_padding_block_high = used_values->border_box_right();
                margin_low = used_values->margin_top;
                margin_high = used_values->margin_bottom;
            }
        }

        for (auto& line : per_node.lines) {
            if (!line.has_contributions) {
                if (line.interrupting_block_position.has_value()) {
                    staged_pieces.append({ .piece = {
                                               .node = node,
                                               .first_fragment_index = line.first_fragment_index.value_or(0),
                                               .fragment_count = line.fragment_count,
                                               .border_box_rect = { *line.interrupting_block_position, {} },
                                               .present_edges = edge_bits(true, true),
                                               .is_geometry_only_placeholder = true,
                                           },
                        .line_index = static_cast<u32>(line.line_index),
                        .depth = per_node.depth,
                        .discovery_index = node_index });
                }
                continue;
            }

            bool is_first_content_line = per_node.first_line_with_content == line.line_index;
            bool is_last_content_line = per_node.last_line_with_content == line.line_index;

            bool has_low_edge = node_inline_axis_is_reversed ? is_last_content_line : is_first_content_line;
            bool has_high_edge = node_inline_axis_is_reversed ? is_first_content_line : is_last_content_line;

            CSSPixels content_block_start = line.first_direct_fragment_block_start.value_or(line.fallback_block_start_from_contributions.value_or(0));
            CSSPixels content_block_length = line.first_direct_fragment_block_start.has_value() ? line.max_direct_fragment_block_length : node.computed_values().line_height();

            CSSPixels border_box_inline_start = line.contributions_inline_start - (has_low_edge ? border_padding_low : 0);
            CSSPixels border_box_inline_end = line.contributions_inline_end + (has_high_edge ? border_padding_high : 0);
            CSSPixels border_box_block_start = content_block_start - border_padding_block_low;
            CSSPixels border_box_block_length = content_block_length + border_padding_block_low + border_padding_block_high;

            auto border_box_rect = inline_axis_is_horizontal
                ? CSSPixelRect { border_box_inline_start, border_box_block_start, border_box_inline_end - border_box_inline_start, border_box_block_length }
                : CSSPixelRect { border_box_block_start, border_box_inline_start, border_box_block_length, border_box_inline_end - border_box_inline_start };

            staged_pieces.append({ .piece = {
                                       .node = node,
                                       .first_fragment_index = line.first_fragment_index.value_or(0),
                                       .fragment_count = line.fragment_count,
                                       .border_box_rect = border_box_rect,
                                       .present_edges = edge_bits(has_low_edge, has_high_edge),
                                   },
                .line_index = static_cast<u32>(line.line_index),
                .depth = per_node.depth,
                .discovery_index = node_index });

            if (per_node.parent_index.has_value()) {
                auto& parent = per_node_data[*per_node.parent_index];
                auto& parent_line = ensure_line(parent, line.line_index);
                auto margin_box_inline_start = border_box_inline_start - (has_low_edge ? margin_low : 0);
                auto margin_box_inline_end = border_box_inline_end + (has_high_edge ? margin_high : 0);
                note_contribution(parent_line, margin_box_inline_start, margin_box_inline_end, content_block_start);
            }
        }
    }

    for (size_t i = 0; i < fragmented_inlines_without_fragments.size(); ++i) {
        auto const& node = *fragmented_inlines_without_fragments[i];
        if (!m_state.try_get(node))
            continue;
        auto placeholder_size = inline_axis_is_horizontal
            ? CSSPixelSize { 0, node.computed_values().line_height() }
            : CSSPixelSize { node.computed_values().line_height(), 0 };
        staged_pieces.append({ .piece = {
                                   .node = node,
                                   .border_box_rect = { {}, placeholder_size },
                                   .present_edges = edge_bits(true, true),
                                   .is_geometry_only_placeholder = true,
                               },
            .line_index = 0,
            .depth = fragmented_inline_nesting_depth(node),
            .discovery_index = per_node_data.size() + i });
    }

    quick_sort(staged_pieces, [](auto const& a, auto const& b) {
        if (a.line_index != b.line_index)
            return a.line_index < b.line_index;
        if (a.depth != b.depth)
            return a.depth < b.depth;
        return a.discovery_index < b.discovery_index;
    });

    pieces.ensure_capacity(staged_pieces.size());
    for (auto& staged_piece : staged_pieces)
        pieces.append(move(staged_piece.piece));
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

    if (auto const* marker = as_if<ListItemMarkerBox>(box)) {
        dimension_list_item_marker(*marker);
        auto marker_distance = distance_between_marker_and_list_item(*marker);
        if (computed_values.direction() == CSS::Direction::Ltr)
            box_state.margin_right += marker_distance;
        else
            box_state.margin_left += marker_distance;
        return;
    }

    auto const& box_constraints = m_layout_input->containing_block_constraints;
    if (box_is_sized_as_replaced_element(box, *m_available_space, box_constraints)) {
        box_state.set_content_width(compute_width_for_replaced_element(box, *m_available_space, box_constraints));
        box_state.set_content_height(compute_height_for_replaced_element(box, *m_available_space, box_constraints));

        // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-automatic
        // The axis in which the preferred size calculation depends on this aspect ratio is called the ratio-dependent
        // axis, and the resulting size is definite if its input sizes are also definite
        auto const height_is_automatic = computed_values.height().is_auto() || should_treat_height_as_auto(box, *m_available_space, box_constraints);
        auto const height_resolved_from_aspect_ratio = box_state.has_definite_width() && box.has_preferred_aspect_ratio() && height_is_automatic;

        if (height_resolved_from_aspect_ratio)
            box_state.set_has_definite_height(true);

        auto child_layout_input = m_layout_input->for_child_formatting_context(box_state.available_inner_space_or_constraints_from(*m_available_space));
        auto independent_formatting_context = layout_inside(box, layout_mode, child_layout_input);
        if (independent_formatting_context)
            independent_formatting_context->parent_context_did_dimension_child_root_box();
        return;
    }

    // Any fragmented inline box should have generated line box fragments already.
    if (box.is_fragmented_inline()) {
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

            auto preferred_width = calculate_max_content_width(box, box_constraints);
            if (preferred_width <= available_width) {
                unconstrained_width = preferred_width;
            } else {
                auto preferred_minimum_width = calculate_min_content_width(box, box_constraints);
                unconstrained_width = min(max(preferred_minimum_width, available_width), preferred_width);
            }
        } else if (m_available_space->width.is_min_content()) {
            unconstrained_width = calculate_min_content_width(box, box_constraints);
        } else {
            unconstrained_width = calculate_max_content_width(box, box_constraints);
        }
    } else {
        if (width_value.contains_percentage() && !m_available_space->width.is_definite()) {
            // NOTE: We can't resolve percentages yet. We'll have to wait until after inner layout.
        } else {
            auto inner_width = calculate_inner_width(box, m_available_space->width, width_value, box_constraints);
            unconstrained_width = inner_width;
        }
    }

    CSSPixels width = unconstrained_width;
    if (!should_treat_max_width_as_none(box, m_available_space->width, box_constraints)) {
        auto max_width = calculate_inner_width(box, m_available_space->width, box.computed_values().max_width(), box_constraints);
        width = min(width, max_width);
    }

    auto computed_min_width = box.computed_values().min_width();
    if (!computed_min_width.is_auto()) {
        auto min_width = calculate_inner_width(box, m_available_space->width, computed_min_width, box_constraints);
        width = max(width, min_width);
    }

    box_state.set_content_width(width);

    parent().resolve_used_height_if_not_treated_as_auto(box, AvailableSpace(AvailableSize::make_definite(width), AvailableSize::make_indefinite()), box_constraints);

    // NOTE: Flex containers with `auto` height are treated as `max-content`, so we can compute their height early.
    if (box.display().is_flex_inside())
        parent().resolve_used_height_if_treated_as_auto(box, AvailableSpace(AvailableSize::make_definite(width), AvailableSize::make_indefinite()), box_constraints);

    make_button_content_box_definite(box, *m_available_space, box_constraints);

    auto child_layout_input = m_layout_input->for_child_formatting_context(box_state.available_inner_space_or_constraints_from(*m_available_space));
    auto independent_formatting_context = layout_inside(box, layout_mode, child_layout_input);

    if (should_treat_height_as_auto(box, *m_available_space, box_constraints)) {
        // FIXME: (10.6.6) If 'height' is 'auto', the height depends on the element's descendants per 10.6.7.
        parent().resolve_used_height_if_treated_as_auto(box, *m_available_space, box_constraints);
    } else {
        parent().resolve_used_height_if_not_treated_as_auto(box, *m_available_space, box_constraints);
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

    InlineLevelIterator iterator(*this, m_state, containing_block(), m_containing_block_used_values, *m_layout_input, m_layout_mode);
    m_fragmented_inlines_in_pre_order = iterator.take_visited_fragmented_inlines();
    auto containing_block_width = m_layout_input->containing_block_constraints.percentage_basis_width.value_or(0);
    LineBuilder line_builder(*this, m_state, m_containing_block_used_values, containing_block_width, direction, writing_mode);

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
        if (item.is_collapsible_whitespace && (line_boxes.is_empty() || line_boxes.last().is_empty_or_ends_in_whitespace() || line_boxes.last().has_block_level_box())) {
            if (item.style_source().computed_values().text_wrap_mode() == CSS::TextWrapMode::Wrap) {
                auto next_width = iterator.next_non_whitespace_sequence_width();
                if (next_width > 0) {
                    line_builder.prepare_to_append_inline_content();
                    line_builder.break_if_needed(next_width);
                }
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
                auto introduce_clearance = parent().clear_floating_boxes(as<NodeWithStyle>(*item.node), *this, m_layout_input->content_box_position_in_bfc_root.value());
                if (introduce_clearance == BlockFormattingContext::DidIntroduceClearance::Yes) {
                    line_builder.did_introduce_clearance(vertical_float_clearance());
                    parent().reset_margin_state();
                }
            }
            break;
        }
        case InlineLevelIterator::Item::Type::Element: {
            auto& box = as<Layout::Box>(*item.node);
            line_builder.prepare_to_append_inline_content();
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
        case InlineLevelIterator::Item::Type::BlockLevelBox: {
            auto& box = as<Layout::Box>(*item.node);
            // The interrupting block cannot carry inline start-edge PBM; stash it back so it attaches to the next
            // inline item instead of being dropped.
            // FIXME: InlineLevelIterator's pending leading inline PBM still attaches to content after an interrupting
            // block, even though the inline start edge should render on the line before the block.
            leading_margin_from_collapsible_whitespace += item.margin_start;
            leading_border_from_collapsible_whitespace += item.border_start;
            leading_padding_from_collapsible_whitespace += item.padding_start;
            line_builder.finish_current_line_before_block_level_box();
            parent().layout_interrupting_block_inside_inline_context(box, containing_block(), *m_layout_input, line_builder);
            break;
        }
        case InlineLevelIterator::Item::Type::AbsolutelyPositionedElement:
            if (auto const* box = as_if<Box>(*item.node)) {
                // Enclosing inline boxes' start edges arrive either still unattached in the iterator
                // or restashed onto this item from a skipped collapsible whitespace chunk.
                auto preceded_by_inline_box_start_edges = item.preceded_by_unattached_inline_start_edges
                    || item.margin_start != 0 || item.border_start != 0 || item.padding_start != 0;
                line_builder.append_static_position_marker(*box, preceded_by_inline_box_start_edges);
                absolute_boxes.append(box);
            }
            break;

        case InlineLevelIterator::Item::Type::FloatingElement:
            if (auto* box = as_if<Box>(*item.node)) {
                line_builder.commit_pending_margin_before_float();
                if (!is<ListItemMarkerBox>(*box))
                    m_state.create(*box, m_layout_input->containing_block_constraints.percentage_basis_width, m_layout_input->containing_block_constraints.percentage_basis_height);
                (void)parent().clear_floating_boxes(as<NodeWithStyle>(*item.node), *this, m_layout_input->content_box_position_in_bfc_root.value());
                // Even if this introduces clearance, we do NOT reset the margin state, because that is clearance
                // between floats and does not contribute to the height of the Inline Formatting Context.
                line_builder.set_unbreakable_run_width_interrupted_by_float(iterator.next_non_whitespace_sequence_width());
                parent().layout_floating_box(*box, containing_block(), *m_layout_input, 0, &line_builder);
            }
            break;

        case InlineLevelIterator::Item::Type::Text: {
            auto& text_node = as<Layout::TextNode>(*item.node);
            line_builder.prepare_to_append_inline_content();

            if (text_node.parent()->computed_values().text_wrap_mode() == CSS::TextWrapMode::Wrap) {
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
                text_node.parent()->computed_values().line_height(),
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

    line_builder.update_last_line();

    // NB: This must happen after update_last_line() so that last-line alignment
    //     adjustments are reflected in the used values of atomic inlines.
    for (auto& line_box : line_boxes) {
        if (line_box.has_block_level_box())
            continue;
        for (auto& fragment : line_box.fragments()) {
            if (fragment.layout_node().is_atomic_inline()) {
                auto& box = as<Box>(fragment.layout_node());
                place_child(box, fragment.offset());
            }
        }
    }

    if (m_layout_mode == LayoutMode::Normal) {
        for (auto const* box : absolute_boxes) {
            StaticPositionRect static_position_rect;
            bool found_static_position_marker = false;
            for (auto const& line_box : line_boxes) {
                for (auto const& marker : line_box.static_position_markers()) {
                    if (marker.box != box)
                        continue;

                    if (box->display_before_box_type_transformation().is_block_outside()) {
                        auto block_position = marker.preceded_by_in_flow_content ? line_box.bottom() : marker.offset().y();
                        auto containing_block_width = m_layout_input->containing_block_constraints.percentage_basis_width.value_or(0);
                        static_position_rect.rect = { { 0, block_position }, { containing_block_width, 0 } };
                    } else {
                        static_position_rect.rect = { marker.offset(), { 0, 0 } };
                    }
                    if (direction == CSS::Direction::Rtl)
                        static_position_rect.horizontal_alignment = StaticPositionRect::Alignment::End;
                    found_static_position_marker = true;
                    break;
                }
                if (found_static_position_marker)
                    break;
            }
            register_contained_abspos_child(*box, static_position_rect);
        }
    }

    line_builder.remove_last_line_if_empty();
}

bool InlineFormattingContext::any_floats_intrude_in_block_range(CSSPixels block_start, CSSPixels block_end) const
{
    // FIXME: Respect inline direction.
    auto intrusions = intrusion_by_floats_into_containing_block(block_start, block_end);
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
    auto containing_block_y_in_root_now = m_layout_input->content_box_position_in_bfc_root->y() + parent().y_adjustment_from_pending_ancestor_top_margins(containing_block());
    auto next_band_start = parent().next_float_band_block_start_after(containing_block_y_in_root_now + block_offset);
    if (!next_band_start.has_value())
        return {};
    return next_band_start.value() - containing_block_y_in_root_now;
}

CSSPixels InlineFormattingContext::vertical_float_clearance() const
{
    return m_vertical_float_clearance;
}

void InlineFormattingContext::set_vertical_float_clearance(CSSPixels vertical_float_clearance)
{
    m_vertical_float_clearance = vertical_float_clearance;
}

}
