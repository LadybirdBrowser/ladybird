/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/TemporaryChange.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Dump.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/FieldSetBox.h>
#include <LibWeb/Layout/InlineFormattingContext.h>
#include <LibWeb/Layout/LegendBox.h>
#include <LibWeb/Layout/LineBuilder.h>
#include <LibWeb/Layout/ListItemBox.h>
#include <LibWeb/Layout/ListItemMarkerBox.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/Layout/TableFormattingContext.h>
#include <LibWeb/Layout/TableWrapper.h>
#include <LibWeb/Layout/Viewport.h>

namespace Web::Layout {

BlockFormattingContext::BlockFormattingContext(LayoutState& state, LayoutMode layout_mode, BlockContainer const& root, FormattingContext* parent)
    : FormattingContext(Type::Block, layout_mode, state, root, parent)
{
    m_bands.append({});
}

BlockFormattingContext::~BlockFormattingContext()
{
    if (!m_was_notified_after_parent_dimensioned_my_root_box) {
        // HACK: The parent formatting context never notified us after assigning dimensions to our root box.
        //       Pretend that it did anyway, to make sure absolutely positioned children get laid out.
        // FIXME: Get rid of this hack once parent contexts behave properly.
        parent_context_did_dimension_child_root_box();
    }
}

CSSPixels BlockFormattingContext::automatic_content_width() const
{
    if (root().children_are_inline())
        return m_state.get(root()).content_width();
    if (is<TableWrapper>(root())) {
        Optional<Box const&> table_box;
        root().for_each_in_subtree_of_type<Box>([&](Box const& child_box) {
            if (child_box.display().is_table_inside()) {
                table_box = child_box;
                return TraversalDecision::Break;
            }
            return TraversalDecision::Continue;
        });
        return m_state.get(*table_box).border_box_width();
    }
    return greatest_child_width(root());
}

CSSPixels BlockFormattingContext::automatic_content_height() const
{
    return compute_auto_height_for_block_formatting_context_root(root());
}

static bool margins_collapse_through(Box const& box, LayoutState& state)
{
    // https://drafts.csswg.org/css2/#adjoining-margins
    // Two margins are adjoining if and only if:
    // - both belong to in-flow block-level boxes that participate in the same block formatting context
    //   NB: Yes, we're dealing with one and the same box here.

    // - no line boxes, no clearance, no padding and no border separate them (Note that certain zero-height line boxes
    //   (see 9.4.2) are ignored for this purpose.)
    // NB: Border and padding are handled further down.
    if (box.computed_values().clear() != CSS::Clear::None)
        return false;

    // - both belong to vertically-adjacent box edges, i.e. form one of the following pairs:
    //   - top and bottom margins of a box that does not establish a new block formatting context and that has zero
    //     computed 'min-height', zero or 'auto' computed 'height', and no in-flow children
    if (FormattingContext::creates_block_formatting_context(box))
        return false;

    // https://drafts.csswg.org/css-display-3/#independent-formatting-context
    // NOTE: [..] margins do not collapse across formatting context boundaries.
    if (FormattingContext::formatting_context_type_created_by_box(box).has_value())
        return false;

    // NB: This should take care of the height and min-height constraints.
    //     ( also see https://github.com/w3c/csswg-drafts/pull/13699#issuecomment-4103045370 for spec ambiguity )
    if (state.get(box).border_box_height() != 0)
        return false;

    // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-margin-collapse
    // FIXME: For the purpose of margin collapsing (CSS 2 §8.3.1 Collapsing margins), if the block axis is the
    //        ratio-dependent axis, it is not considered to have a computed block-size of auto.

    // AD-HOC: The "and no in-flow children" above is wrong. (see https://github.com/w3c/csswg-drafts/pull/13699 )
    for (auto const* child = box.first_child_of_type<Box>(); child; child = child->next_sibling_of_type<Box>()) {
        if (child->is_out_of_flow())
            continue;
        if (!margins_collapse_through(*child, state))
            return false;
    }

    return true;
}

void BlockFormattingContext::run(LayoutInput const& layout_input)
{
    auto const& available_space = layout_input.available_space;
    FORMATTING_CONTEXT_TRACE();
    // https://drafts.csswg.org/css-multicol-2/#the-multi-column-model
    auto const& root_state = m_state.get(root());
    auto column_count = determine_used_value_for_column_count(root_state.content_width());
    if (column_count.has_value()) {
        auto column_width = determine_used_value_for_column_width(root_state.content_width(), column_count.value());
        // FIXME: Do multi-column layout.
        (void)column_width;
    }

    auto root_layout_input = layout_input.with_content_box_position_in_bfc_root(CSSPixelPoint {});

    if (auto const* fieldset_box = as_if<FieldSetBox>(root()); fieldset_box && fieldset_box->rendered_legend()) {
        layout_fieldset_with_rendered_legend(*fieldset_box, root_layout_input);
        return;
    }
    if (root().children_are_inline())
        layout_inline_children(root(), root_layout_input, available_space);
    else
        layout_block_level_children(root(), root_layout_input, available_space);

    // Fieldsets without a rendered legend skip collapsed margin assignment.
    if (is<FieldSetBox>(root()))
        return;

    // Assign collapsed margin left after children layout of formatting context to the last child box
    if (m_margin_state.current_collapsed_margin() != 0) {
        for (auto* child_box = root().last_child_of_type<Box>(); child_box; child_box = child_box->previous_sibling_of_type<Box>()) {
            if (child_box->is_absolutely_positioned() || child_box->is_floating())
                continue;
            if (margins_collapse_through(*child_box, m_state))
                continue;
            m_state.get_mutable(*child_box).margin_bottom = m_margin_state.current_collapsed_margin();
            break;
        }

        // The margin reassignment above changed a child's margin box, which the root's baselines may
        // have been derived from (a scroll container child exports its bottom margin edge), so re-derive them.
        compute_and_store_baselines(m_state.get_mutable(root()));
    }
}

void BlockFormattingContext::parent_context_did_dimension_child_root_box()
{
    m_was_notified_after_parent_dimensioned_my_root_box = true;

    for (auto& floating_box : m_floats) {
        auto content_y = floating_box->top_margin_edge + floating_box->used_values.margin_box_top();
        if (floating_box->side == FloatSide::Left) {
            // Left-side floats: offset_from_edge is from left edge (0) to left content edge of floating_box.
            place_child(floating_box->box, { floating_box->offset_from_edge, content_y });
        } else {
            // Right-side floats: offset_from_edge is from right edge (float_containing_block_width) to the left content edge of floating_box.
            auto float_containing_block_width = [&] {
                switch (floating_box->used_values.width_constraint) {
                case SizeConstraint::MinContent:
                    return CSSPixels(0);
                case SizeConstraint::MaxContent:
                    return CSSPixels::max();
                case SizeConstraint::None:
                    return floating_box->percentage_basis_width.value_or(0);
                }
                VERIFY_NOT_REACHED();
            }();
            place_child(floating_box->box, { float_containing_block_width - floating_box->offset_from_edge, content_y });
        }
    }

    layout_absolutely_positioned_children();
}

bool BlockFormattingContext::box_should_avoid_floats_because_it_establishes_fc(Box const& box) const
{
    // https://drafts.csswg.org/css2/#floats
    // The border box of a table, a block-level replaced element, or an element in the normal flow that establishes
    // a new block formatting context (such as an element with 'overflow' other than 'visible') must not overlap the
    // margin box of any floats in the same block formatting context as the element itself. If necessary,
    // implementations should clear the said element by placing it below any preceding floats, but may place it
    // adjacent to such floats if there is sufficient space. They may even make the border box of said element
    // narrower than defined by section 10.3.3. CSS2 does not define when a UA may put said element next to the
    // float or by how much said element may become narrower.

    // https://drafts.csswg.org/css-flexbox/#flex-containers
    // A flex container establishes a new flex formatting context for its contents. This is the same as establishing
    // a block formatting context, except that flex layout is used instead of block layout. For example, floats do
    // not intrude into the flex container, and the flex container’s margins do not collapse with the margins of its
    // contents.

    // https://drafts.csswg.org/css-grid/#grid-containers
    // A grid container that is not a subgrid establishes an independent grid formatting context for its contents.
    // This is the same as establishing an independent block formatting context, except that grid layout is used
    // instead of block layout: floats do not intrude into the grid container, and the grid container’s margins do
    // not collapse with the margins of its contents.

    auto formatting_context_type = formatting_context_type_created_by_box(box);
    return formatting_context_type.has_value()
        && first_is_one_of(formatting_context_type.value(), Type::Block, Type::Flex, Type::Grid);
}

void BlockFormattingContext::compute_width(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints, CSSPixelPoint content_position_in_root)
{
    auto remaining_available_space = available_space;

    // Certain formatting contexts do not allow float intrusions, so reduce the available space for them.
    if (available_space.width.is_definite() && box_should_avoid_floats_because_it_establishes_fc(box)) {
        auto available_width = available_space.width.to_px_or_zero();
        auto box_in_root_rect = CSSPixelRect { content_position_in_root, m_state.get(box).content_size() };
        box_in_root_rect.set_width(available_width);
        auto intrusion = intrusions_for_band_into_rect(band_at(box_in_root_rect.y()), box_in_root_rect);
        auto remaining_width = available_width - intrusion.left - intrusion.right;
        if (intrusion.left > 0 || intrusion.right > 0) {
            // Negative margins do not create additional space next to a float. Reduce the space available for
            // resolving an automatic width by any negative margins, so that the resulting border box is no wider
            // than the space next to the float.
            auto margin_left = box.computed_values().margin().left().resolved_or_auto(available_width).to_px_or_zero();
            auto margin_right = box.computed_values().margin().right().resolved_or_auto(available_width).to_px_or_zero();
            auto negative_margin_sum = min(margin_left, CSSPixels(0)) + min(margin_right, CSSPixels(0));
            remaining_width = max(remaining_width + negative_margin_sum, CSSPixels(0));
        }
        remaining_available_space.width = AvailableSize::make_definite(remaining_width);
    }

    if (box_is_sized_as_replaced_element(box, available_space, containing_block_constraints)) {
        compute_width_for_block_level_replaced_element_in_normal_flow(box, available_space, containing_block_constraints);
        if (box.is_floating()) {
            // 10.3.6 Floating, replaced elements:
            // https://www.w3.org/TR/CSS22/visudet.html#float-replaced-width
            return;
        }
    }

    if (box.is_floating()) {
        // 10.3.5 Floating, non-replaced elements:
        // https://www.w3.org/TR/CSS22/visudet.html#float-width
        compute_width_for_floating_box(box, available_space, containing_block_constraints);
        return;
    }

    auto const& computed_values = box.computed_values();
    auto available_width_px = available_space.width.to_px_or_zero();
    auto margin_left = computed_values.margin().left().resolved_or_auto(available_width_px);
    auto margin_right = computed_values.margin().right().resolved_or_auto(available_width_px);
    auto const padding_left = computed_values.padding().left().resolved_or_auto(available_width_px);
    auto const padding_right = computed_values.padding().right().resolved_or_auto(available_width_px);

    auto& box_state = m_state.get_mutable(box);
    box_state.margin_left = margin_left.to_px_or_zero();
    box_state.margin_right = margin_right.to_px_or_zero();
    box_state.border_left = computed_values.border_left().width;
    box_state.border_right = computed_values.border_right().width;
    box_state.padding_left = padding_left.to_px_or_zero();
    box_state.padding_right = padding_right.to_px_or_zero();

    // NOTE: If we are calculating the min-content or max-content width of this box,
    //       and the width should be treated as auto, then we can simply return here,
    //       as the preferred width and min/max constraints are irrelevant for intrinsic sizing.
    if (box_state.width_constraint != SizeConstraint::None)
        return;

    auto const remaining_width_px = remaining_available_space.width.to_px_or_zero();
    auto const zero_value = CSS::Length::make_px(0);

    auto try_compute_width = [&](CSS::LengthOrAuto const& a_width) {
        auto width = a_width;
        margin_left = computed_values.margin().left().resolved_or_auto(available_space.width.to_px_or_zero());
        margin_right = computed_values.margin().right().resolved_or_auto(available_space.width.to_px_or_zero());
        CSSPixels total_px = computed_values.border_left().width + computed_values.border_right().width;
        for (auto& value : { CSS::LengthOrAuto(margin_left), CSS::LengthOrAuto(padding_left), width, CSS::LengthOrAuto(padding_right), CSS::LengthOrAuto(margin_right) })
            total_px += value.to_px_or_zero();

        if (!box.is_inline()) {
            // 10.3.3 Block-level, non-replaced elements in normal flow
            // If 'width' is not 'auto' and 'border-left-width' + 'padding-left' + 'width' + 'padding-right' +
            // 'border-right-width' (plus any of 'margin-left' or 'margin-right' that are not 'auto') is larger than the
            // width of the containing block, then any 'auto' values for 'margin-left' or 'margin-right' are, for the
            // following rules, treated as zero.
            if (!width.is_auto() && total_px > remaining_width_px) {
                if (margin_left.is_auto())
                    margin_left = zero_value;
                if (margin_right.is_auto())
                    margin_right = zero_value;
            }

            // 10.3.3 cont'd.
            auto underflow_px = remaining_width_px - total_px;
            if (available_space.width.is_intrinsic_sizing_constraint())
                underflow_px = 0;

            if (width.is_auto()) {
                if (margin_left.is_auto())
                    margin_left = zero_value;
                if (margin_right.is_auto())
                    margin_right = zero_value;

                if (available_space.width.is_definite()) {
                    if (underflow_px >= 0) {
                        width = CSS::Length::make_px(underflow_px);
                    } else {
                        width = zero_value;
                    }
                } else if (available_space.width.is_min_content()) {
                    if (formatting_context_type_created_by_box(box).has_value())
                        width = CSS::Length::make_px(calculate_min_content_width(box, containing_block_constraints));
                } else if (available_space.width.is_max_content()) {
                    if (formatting_context_type_created_by_box(box).has_value())
                        width = CSS::Length::make_px(calculate_max_content_width(box, containing_block_constraints));
                } else {
                    VERIFY_NOT_REACHED();
                }
            } else {
                if (!margin_left.is_auto() && !margin_right.is_auto()) {
                    margin_right = CSS::Length::make_px(margin_right.to_px_or_zero() + underflow_px);
                } else if (!margin_left.is_auto() && margin_right.is_auto()) {
                    margin_right = CSS::Length::make_px(underflow_px);
                } else if (margin_left.is_auto() && !margin_right.is_auto()) {
                    margin_left = CSS::Length::make_px(underflow_px);
                } else { // margin_left.is_auto() && margin_right.is_auto()
                    auto half_of_the_underflow = CSS::Length::make_px(underflow_px / 2);
                    margin_left = half_of_the_underflow;
                    margin_right = half_of_the_underflow;
                }
            }
        }

        return width;
    };

    auto input_width = [&] -> CSS::LengthOrAuto {
        if (box_is_sized_as_replaced_element(box, available_space, containing_block_constraints)) {
            // NOTE: Replaced elements had their width calculated independently above.
            //       We use that width as the input here to ensure that margins get resolved.
            return CSS::Length::make_px(box_state.content_width());
        }
        if (is<TableWrapper>(box))
            return CSS::Length::make_px(compute_table_box_width_inside_table_wrapper(box, remaining_available_space, containing_block_constraints));

        // https://html.spec.whatwg.org/multipage/rendering.html#button-layout
        // If the computed value of 'inline-size' is 'auto', then the used value is the fit-content inline size.
        if (auto const* html_element = as_if<HTML::HTMLElement>(box.dom_node()); html_element
            && html_element->uses_button_layout() && computed_values.width().is_auto()) {
            return CSS::Length::make_px(calculate_fit_content_width(box, available_space, containing_block_constraints));
        }

        if (should_treat_width_as_auto(box, available_space))
            return CSS::LengthOrAuto::make_auto();
        return CSS::Length::make_px(calculate_inner_width(box, available_space.width, computed_values.width(), containing_block_constraints));
    }();

    // 1. The tentative used width is calculated (without 'min-width' and 'max-width')
    auto used_width = try_compute_width(input_width);

    // 2. The tentative used width is greater than 'max-width', the rules above are applied again,
    //    but this time using the computed value of 'max-width' as the computed value for 'width'.
    if (!should_treat_max_width_as_none(box, available_space.width, containing_block_constraints)) {
        auto max_width = calculate_inner_width(box, available_space.width, computed_values.max_width(), containing_block_constraints);
        if (used_width.to_px_or_zero() > max_width)
            used_width = try_compute_width(CSS::Length::make_px(max_width));
    }

    // 3. If the resulting width is smaller than 'min-width', the rules above are applied again,
    //    but this time using the value of 'min-width' as the computed value for 'width'.
    if (!computed_values.min_width().is_auto() && !used_width.is_auto()) {
        auto min_width = calculate_inner_width(box, available_space.width, computed_values.min_width(), containing_block_constraints);
        if (used_width.to_px_or_zero() < min_width)
            used_width = try_compute_width(CSS::Length::make_px(min_width));
    }

    if (!box_is_sized_as_replaced_element(box, available_space, containing_block_constraints) && !used_width.is_auto())
        box_state.set_content_width(used_width.to_px_or_zero());

    box_state.margin_left = margin_left.to_px_or_zero();
    box_state.margin_right = margin_right.to_px_or_zero();
}

size_t BlockFormattingContext::band_index_at(CSSPixels y) const
{
    VERIFY(!m_bands.is_empty());

    size_t index = 0;
    for (size_t i = 1; i < m_bands.size(); ++i) {
        if (m_bands[i].block_start > y)
            break;
        index = i;
    }
    return index;
}

BlockFormattingContext::FloatBand const& BlockFormattingContext::band_at(CSSPixels y) const
{
    return m_bands[band_index_at(y)];
}

Optional<CSSPixels> BlockFormattingContext::next_float_band_block_start_after(CSSPixels y_in_root) const
{
    for (auto const& band : m_bands) {
        if (band.block_start > y_in_root)
            return band.block_start;
    }
    return {};
}

FormattingContext::SpaceUsedByFloats BlockFormattingContext::available_inline_space(CSSPixels block_start_in_root, CSSPixels block_end_in_root) const
{
    VERIFY(!m_bands.is_empty());

    SpaceUsedByFloats intrusions;
    if (block_end_in_root <= block_start_in_root) {
        auto const& band = band_at(block_start_in_root);
        intrusions.left = band.left_intrusion;
        intrusions.right = band.right_intrusion;
        return intrusions;
    }

    for (size_t i = band_index_at(block_start_in_root); i < m_bands.size(); ++i) {
        auto const& band = m_bands[i];
        if (band.block_start >= block_end_in_root)
            break;
        intrusions.left = max(intrusions.left, band.left_intrusion);
        intrusions.right = max(intrusions.right, band.right_intrusion);
    }

    return intrusions;
}

FormattingContext::SpaceUsedByFloats BlockFormattingContext::intrusions_for_band_into_rect(FloatBand const& band, CSSPixelRect const& rect_in_root) const
{
    auto root_content_width = m_state.get(root()).content_width();
    return {
        .left = band.left_intrusion == 0 ? CSSPixels(0) : max(CSSPixels(0), band.left_intrusion - rect_in_root.x()),
        .right = band.right_intrusion == 0 ? CSSPixels(0) : max(CSSPixels(0), band.right_intrusion - (root_content_width - rect_in_root.right())),
    };
}

FormattingContext::SpaceUsedByFloats BlockFormattingContext::intrusion_by_floats_into_rect(CSSPixelRect const& box_in_root_rect, CSSPixels block_start_in_box, CSSPixels block_end_in_box) const
{
    auto intrusions = available_inline_space(box_in_root_rect.y() + block_start_in_box, box_in_root_rect.y() + block_end_in_box);
    auto root_content_width = m_state.get(root()).content_width();
    return {
        .left = intrusions.left == 0 ? CSSPixels(0) : max(CSSPixels(0), intrusions.left - box_in_root_rect.x()),
        .right = intrusions.right == 0 ? CSSPixels(0) : max(CSSPixels(0), intrusions.right - (root_content_width - box_in_root_rect.right())),
    };
}

BlockFormattingContext::FloatPlacement BlockFormattingContext::place_float(FloatSide side, LayoutState::UsedValues const& box_state, AvailableSpace const& available_space, CSSPixelRect const& containing_block_rect_in_root, CSSPixels ceiling_in_root) const
{
    auto const margin_box_width = box_state.margin_box_width();
    auto candidate_block_start = ceiling_in_root;

    for (;;) {
        auto const& band = band_at(candidate_block_start);
        auto intrusions = intrusions_for_band_into_rect(band, containing_block_rect_in_root);
        auto available_width = available_space.width.to_px_or_zero() - intrusions.left - intrusions.right;
        auto has_floats_present = band.left_intrusion > 0 || band.right_intrusion > 0;

        if (available_space.width.is_max_content() || available_space.width.is_indefinite() || margin_box_width <= available_width || !has_floats_present) {
            auto offset_from_edge = side == FloatSide::Left
                ? intrusions.left + box_state.margin_box_left()
                : intrusions.right + box_state.content_width() + box_state.margin_box_right();
            return {
                .block_start = candidate_block_start,
                .offset_from_edge = offset_from_edge,
            };
        }

        auto next_band_start = next_float_band_block_start_after(candidate_block_start);
        if (!next_band_start.has_value())
            return {
                .block_start = candidate_block_start,
                .offset_from_edge = side == FloatSide::Left
                    ? intrusions.left + box_state.margin_box_left()
                    : intrusions.right + box_state.content_width() + box_state.margin_box_right(),
            };

        candidate_block_start = next_band_start.value();
    }
}

void BlockFormattingContext::ensure_band_boundary(CSSPixels block_start)
{
    VERIFY(!m_bands.is_empty());

    for (size_t i = 0; i < m_bands.size(); ++i) {
        if (m_bands[i].block_start == block_start)
            return;
        if (m_bands[i].block_start > block_start) {
            auto new_band = i == 0 ? FloatBand {} : m_bands[i - 1];
            new_band.block_start = block_start;
            m_bands.insert(i, new_band);
            return;
        }
    }

    auto new_band = m_bands.last();
    new_band.block_start = block_start;
    m_bands.append(new_band);
}

void BlockFormattingContext::add_float_to_bands(FloatingBox const& floating_box, CSSPixelRect containing_block_rect_in_root)
{
    auto const pending_y_adjustment = y_adjustment_from_pending_ancestor_top_margins(floating_box.box);
    containing_block_rect_in_root.translate_by(0, pending_y_adjustment);

    auto const& box_state = floating_box.used_values;
    auto const root_content_width = m_state.get(root()).content_width();
    auto const margin_box_rect_in_root = floating_box.margin_box_rect_in_root_coordinate_space.translated(0, pending_y_adjustment);
    auto const block_start = margin_box_rect_in_root.top();
    auto const block_end = margin_box_rect_in_root.bottom();

    if (floating_box.side == FloatSide::Left)
        m_lowest_left_margin_edge = max(m_lowest_left_margin_edge, block_end);
    else
        m_lowest_right_margin_edge = max(m_lowest_right_margin_edge, block_end);

    if (block_end <= block_start)
        return;

    ensure_band_boundary(block_start);
    ensure_band_boundary(block_end);

    auto intrusion = floating_box.side == FloatSide::Left
        ? containing_block_rect_in_root.x() + floating_box.offset_from_edge + box_state.content_width() + box_state.margin_box_right()
        : (root_content_width - containing_block_rect_in_root.right()) + floating_box.offset_from_edge + box_state.margin_box_left();

    for (auto& band : m_bands) {
        if (band.block_start < block_start)
            continue;
        if (band.block_start >= block_end)
            break;
        if (floating_box.side == FloatSide::Left)
            band.left_intrusion = max(band.left_intrusion, intrusion);
        else
            band.right_intrusion = max(band.right_intrusion, intrusion);
    }
}

void BlockFormattingContext::rebuild_float_bands()
{
    m_bands.clear();
    m_bands.append({});
    m_lowest_left_margin_edge = 0;
    m_lowest_right_margin_edge = 0;

    for (auto& floating_box : m_floats)
        add_float_to_bands(*floating_box, floating_box->containing_block_rect_in_root_coordinate_space);
}

void BlockFormattingContext::update_lowest_floating_descendant_bottom_margin_edge()
{
    Optional<CSSPixels> lowest;
    for (auto const& floating_box : m_floats) {
        auto bottom_margin_edge = floating_box->margin_box_rect_in_root_coordinate_space.bottom();
        if (!lowest.has_value() || bottom_margin_edge > *lowest)
            lowest = bottom_margin_edge;
    }
    m_state.get_mutable(root()).set_lowest_floating_descendant_bottom_margin_edge(lowest);
}

void BlockFormattingContext::translate_floats_in_subtree(Box const& ancestor, CSSPixelPoint delta)
{
    if (delta.is_zero() || m_floats.is_empty())
        return;
    bool any_float_moved = false;
    for (auto& floating_box : m_floats) {
        if (!ancestor.is_ancestor_of(floating_box->box))
            continue;
        floating_box->margin_box_rect_in_root_coordinate_space.translate_by(delta);
        floating_box->containing_block_rect_in_root_coordinate_space.translate_by(delta);
        any_float_moved = true;
    }
    if (!any_float_moved)
        return;
    update_lowest_floating_descendant_bottom_margin_edge();
    rebuild_float_bands();
}

CSSPixels BlockFormattingContext::margin_box_left_of_float_in_root(FloatingBox const& floating_box, CSSPixelRect const& containing_block_rect_in_root) const
{
    if (floating_box.side == FloatSide::Left)
        return containing_block_rect_in_root.x() + floating_box.offset_from_edge - floating_box.used_values.margin_box_left();
    return containing_block_rect_in_root.right() - floating_box.offset_from_edge - floating_box.used_values.margin_box_left();
}

CSSPixels BlockFormattingContext::border_box_left_of_box_avoiding_floats(Box const& box, LayoutState::UsedValues const& box_state, SpaceUsedByFloats const& space_used_by_floats) const
{
    if (box.computed_values().margin().left().is_auto())
        return space_used_by_floats.left + box_state.margin_left;
    if (box_state.margin_left >= 0)
        return max(space_used_by_floats.left, box_state.margin_left);
    if (space_used_by_floats.left > 0 || space_used_by_floats.right > 0)
        return space_used_by_floats.left;
    return space_used_by_floats.left + box_state.margin_left;
}

CSSPixels BlockFormattingContext::avoid_float_intrusions(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints, CSSPixels content_y, CSSPixelRect const& containing_block_rect_in_root)
{
    if (!available_space.width.is_definite())
        return content_y;
    if (!box_should_avoid_floats_because_it_establishes_fc(box))
        return content_y;

    // https://drafts.csswg.org/css2/#floats
    // If necessary, implementations should clear the said element by placing it below any preceding floats, but may
    // place it adjacent to such floats if there is sufficient space.
    auto const& box_state = m_state.get(box);
    while (true) {
        auto border_box_y_in_root = containing_block_rect_in_root.y() + content_y - box_state.border_box_top();
        auto band_containing_block_rect = containing_block_rect_in_root;
        band_containing_block_rect.set_y(border_box_y_in_root);
        band_containing_block_rect.set_height(box_state.border_box_height());
        auto const& band = band_at(border_box_y_in_root);
        auto space_used_by_floats = intrusions_for_band_into_rect(band, band_containing_block_rect);
        bool const constrained_by_floats = space_used_by_floats.left > 0 || space_used_by_floats.right > 0;
        auto border_box_left_in_containing_block = border_box_left_of_box_avoiding_floats(box, box_state, space_used_by_floats);

        bool must_clear_below_current_band = constrained_by_floats
            && border_box_left_in_containing_block + box_state.border_box_width() > available_space.width.to_px_or_zero() - space_used_by_floats.right;

        if (!must_clear_below_current_band) {
            CSSPixelRect border_box_rect_in_root {
                band_containing_block_rect.x() + border_box_left_in_containing_block,
                border_box_y_in_root,
                box_state.border_box_width(),
                box_state.border_box_height(),
            };
            must_clear_below_current_band = any_of(m_floats, [&](auto const& floating_box) {
                auto margin_box_rect_in_root = floating_box->margin_box_rect_in_root_coordinate_space.translated(
                    0, y_adjustment_from_pending_ancestor_top_margins(floating_box->box));
                return !margin_box_rect_in_root.intersected(border_box_rect_in_root).is_empty();
            });
        }
        if (!must_clear_below_current_band)
            break;

        auto next_band_start = next_float_band_block_start_after(border_box_y_in_root);
        if (!next_band_start.has_value())
            break;

        content_y += next_band_start.value() - border_box_y_in_root;
        auto content_position_in_root = containing_block_rect_in_root.location().translated(0, content_y);
        compute_width(box, available_space, containing_block_constraints, content_position_in_root);
    }
    return content_y;
}

void BlockFormattingContext::compute_width_for_floating_box(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints)
{
    // 10.3.5 Floating, non-replaced elements
    auto& computed_values = box.computed_values();

    auto width_of_containing_block = available_space.width.to_px_or_zero();

    // If 'margin-left', or 'margin-right' are computed as 'auto', their used value is '0'.
    auto margin_left = computed_values.margin().left().to_px_or_zero(width_of_containing_block);
    auto margin_right = computed_values.margin().right().to_px_or_zero(width_of_containing_block);

    auto& box_state = m_state.get_mutable(box);
    box_state.padding_left = computed_values.padding().left().to_px_or_zero(width_of_containing_block);
    box_state.padding_right = computed_values.padding().right().to_px_or_zero(width_of_containing_block);
    box_state.margin_left = margin_left;
    box_state.margin_right = margin_right;
    box_state.border_left = computed_values.border_left().width;
    box_state.border_right = computed_values.border_right().width;

    auto compute_width = [&](CSS::LengthOrAuto width) {
        // If 'width' is computed as 'auto', the used value is the "shrink-to-fit" width.
        if (width.is_auto()) {
            if (available_space.width.is_definite()) {
                // Find the available width: in this case, this is the width of the containing
                // block minus the used values of 'margin-left', 'border-left-width', 'padding-left',
                // 'padding-right', 'border-right-width', 'margin-right', and the widths of any relevant scroll bars.
                auto available_width = available_space.width.to_px_or_zero()
                    - margin_left - computed_values.border_left().width - box_state.padding_left
                    - box_state.padding_right - computed_values.border_right().width - margin_right;
                // Then the shrink-to-fit width is: min(max(preferred minimum width, available width), preferred width).
                auto preferred_width = calculate_max_content_width(box, containing_block_constraints);
                if (preferred_width <= available_width) {
                    width = CSS::Length::make_px(preferred_width);
                } else {
                    auto preferred_minimum_width = calculate_min_content_width(box, containing_block_constraints);
                    width = CSS::Length::make_px(min(max(preferred_minimum_width, available_width), preferred_width));
                }
            } else if (available_space.width.is_indefinite() || available_space.width.is_max_content()) {
                // Fold the formula for shrink-to-fit width for indefinite and max-content available width.
                width = CSS::Length::make_px(calculate_max_content_width(box, containing_block_constraints));
            } else {
                // Fold the formula for shrink-to-fit width for min-content available width.
                width = CSS::Length::make_px(calculate_min_content_width(box, containing_block_constraints));
            }
        }

        return width;
    };

    auto input_width = [&] -> CSS::LengthOrAuto {
        if (should_treat_width_as_auto(box, available_space))
            return CSS::LengthOrAuto::make_auto();
        return CSS::Length::make_px(calculate_inner_width(box, available_space.width, computed_values.width(), containing_block_constraints));
    }();

    // 1. The tentative used width is calculated (without 'min-width' and 'max-width')
    auto width = compute_width(input_width);

    // 2. The tentative used width is greater than 'max-width', the rules above are applied again,
    //    but this time using the computed value of 'max-width' as the computed value for 'width'.
    if (!should_treat_max_width_as_none(box, available_space.width, containing_block_constraints)) {
        auto max_width = calculate_inner_width(box, available_space.width, computed_values.max_width(), containing_block_constraints);
        if (width.to_px_or_zero() > max_width)
            width = compute_width(CSS::Length::make_px(max_width));
    }

    // 3. If the resulting width is smaller than 'min-width', the rules above are applied again,
    //    but this time using the value of 'min-width' as the computed value for 'width'.
    if (!computed_values.min_width().is_auto()) {
        auto min_width = calculate_inner_width(box, available_space.width, computed_values.min_width(), containing_block_constraints);
        if (width.to_px_or_zero() < min_width)
            width = compute_width(CSS::Length::make_px(min_width));
    }

    box_state.set_content_width(width.to_px_or_zero());
}

void BlockFormattingContext::compute_width_for_block_level_replaced_element_in_normal_flow(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints)
{
    // 10.3.6 Floating, replaced elements
    auto& computed_values = box.computed_values();

    auto width_of_containing_block = available_space.width.to_px_or_zero();

    // 10.3.4 Block-level, replaced elements in normal flow
    // The used value of 'width' is determined as for inline replaced elements. Then the rules for
    // non-replaced block-level elements are applied to determine the margins.
    // If 'margin-left', or 'margin-right' are computed as 'auto', their used value is '0'.
    auto margin_left = computed_values.margin().left().to_px_or_zero(width_of_containing_block);
    auto margin_right = computed_values.margin().right().to_px_or_zero(width_of_containing_block);
    auto const padding_left = computed_values.padding().left().to_px_or_zero(width_of_containing_block);
    auto const padding_right = computed_values.padding().right().to_px_or_zero(width_of_containing_block);

    auto& box_state = m_state.get_mutable(box);
    box_state.margin_left = margin_left;
    box_state.margin_right = margin_right;
    box_state.border_left = computed_values.border_left().width;
    box_state.border_right = computed_values.border_right().width;
    box_state.padding_left = padding_left;
    box_state.padding_right = padding_right;

    auto width = compute_width_for_replaced_element(box, available_space, containing_block_constraints);
    box_state.set_content_width(width);
}

void BlockFormattingContext::resolve_used_height_if_not_treated_as_auto(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints)
{
    if (should_treat_height_as_auto(box, available_space, containing_block_constraints)) {
        return;
    }

    auto const& computed_values = box.computed_values();
    auto& box_state = m_state.get_mutable(box);

    auto height = calculate_inner_height(box, available_space, box.computed_values().height(), containing_block_constraints);

    if (!should_treat_max_height_as_none(box, available_space.height, containing_block_constraints)) {
        if (!computed_values.max_height().is_auto()) {
            auto max_height = calculate_inner_height(box, available_space, computed_values.max_height(), containing_block_constraints);
            height = min(height, max_height);
        }
    }
    if (!computed_values.min_height().is_auto()) {
        height = max(height, calculate_inner_height(box, available_space, computed_values.min_height(), containing_block_constraints));
    }

    box_state.set_content_height(height);
    if (computed_height_establishes_definite_containing_block_height(computed_values.height()))
        box_state.set_has_definite_height(true);
}

void BlockFormattingContext::resolve_used_height_if_treated_as_auto(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints, FormattingContext const* box_formatting_context)
{
    if (!should_treat_height_as_auto(box, available_space, containing_block_constraints)) {
        return;
    }

    auto const& computed_values = box.computed_values();
    auto& box_state = m_state.get_mutable(box);

    CSSPixels height = 0;
    if (box_is_sized_as_replaced_element(box, available_space, containing_block_constraints)) {
        height = compute_height_for_replaced_element(box, available_space, containing_block_constraints);
    } else {
        if (box_formatting_context) {
            height = box_formatting_context->automatic_content_height();
        } else {
            height = compute_auto_height_for_block_level_element(box, m_state.get(box).available_inner_space_or_constraints_from(available_space), containing_block_constraints);
        }
    }

    if (!should_treat_max_height_as_none(box, available_space.height, containing_block_constraints)) {
        if (!computed_values.max_height().is_auto()) {
            auto max_height = calculate_inner_height(box, available_space, computed_values.max_height(), containing_block_constraints);
            height = min(height, max_height);
        }
    }
    if (!computed_values.min_height().is_auto()) {
        height = max(height, calculate_inner_height(box, available_space, computed_values.min_height(), containing_block_constraints));
    }

    if (box.document().in_quirks_mode()
        && box.dom_node()
        && box.dom_node()->is_html_html_element()
        && box.computed_values().height().is_auto()) {
        // 3.6. The html element fills the viewport quirk
        // https://quirks.spec.whatwg.org/#the-html-element-fills-the-viewport-quirk
        // FIXME: Handle vertical writing mode.

        // 1. Let margins be sum of the used values of the margin-left and margin-right properties of element
        //    if element has a vertical writing mode, otherwise let margins be the sum of the used values of
        //    the margin-top and margin-bottom properties of element.
        auto margins = box_state.margin_top + box_state.margin_bottom;

        // 2. Let size be the size of the initial containing block in the block flow direction minus margins.
        auto size = containing_block_constraints.percentage_basis_height.value_or(0) - margins;

        // 3. Return the bigger value of size and the normal border box size the element would have
        //    according to the CSS specification.
        height = max(size, height);

        // NOTE: The height of the root element when affected by this quirk is considered to be definite.
        box_state.set_has_definite_height(true);
    }

    if (box.document().in_quirks_mode()
        && box.dom_node()
        && box.dom_node()->is_html_body_element()
        && box.computed_values().height().is_auto()) {
        // 3.7. The body element fills the html element quirk
        // https://quirks.spec.whatwg.org/#the-body-element-fills-the-html-element-quirk
        // FIXME: Handle vertical writing mode.

        // The element body must additionally meet the following conditions:
        // - The computed value of the 'position' property of element is neither 'absolute' nor 'fixed'.
        // - The computed value of the 'float' property of element is 'none'.
        // - Element is not an inline-level element.
        // - Element is not a multi-column spanning element.
        // NON-STANDARD: We don't check column-span since no browser actually excludes it.
        if (!box.is_absolutely_positioned() && !box.is_floating() && !box.is_inline()) {
            // 1. Let margins be sum of the used values of the margin-left and margin-right properties of element
            //    if element has a vertical writing mode, otherwise let margins be the sum of the used values of
            //    the margin-top and margin-bottom properties of element.
            auto margins = box_state.margin_top + box_state.margin_bottom;

            // 2. Let size be the size of element's parent element's content box in the block flow direction minus margins.
            auto size = containing_block_constraints.percentage_basis_height.value_or(0) - margins;

            // 3. Return the bigger value of size and the normal border box size the element would have
            //    according to the CSS specification.
            height = max(size, height);
        }
    }

    box_state.set_content_height(height);
}

void BlockFormattingContext::layout_inline_children(BlockContainer const& block_container, LayoutInput const& layout_input, AvailableSpace const& available_space_for_children)
{
    auto const& available_space = layout_input.available_space;
    VERIFY(block_container.children_are_inline());

    auto& block_container_state = m_state.get_mutable(block_container);

    InlineFormattingContext context(m_state, m_layout_mode, block_container, block_container_state, *this);
    context.run(layout_input_for_child_context(block_container_state, layout_input, available_space_for_children));

    if (!block_container_state.has_definite_width()) {
        // NOTE: min-width or max-width for boxes with inline children can only be applied after inside layout
        //       is done and width of box content is known
        auto used_width_px = context.automatic_content_width();
        // NOTE: Min and max constraints are not applied to a box that is being sized under an intrinsic
        //       sizing constraint: per css-sizing-3, min/max-width affect a box's intrinsic size
        //       *contributions*, and the callers of calculate_{min,max}_content_width() apply them.
        //       Applying them here would bake the box's own min/max-width into its measured intrinsic
        //       size, and the border-box adjustment would consume border/padding that measurement
        //       state does not have.
        if (block_container_state.width_constraint == SizeConstraint::None) {
            // https://www.w3.org/TR/css-sizing-3/#sizing-values
            // Percentages are resolved against the width/height, as appropriate, of the box’s containing block.
            auto containing_block_width = layout_input.containing_block_constraints.percentage_basis_width.value_or(0);
            auto available_width = AvailableSize::make_definite(containing_block_width);
            if (!should_treat_max_width_as_none(block_container, available_space.width, layout_input.containing_block_constraints)) {
                auto max_width_px = calculate_inner_width(block_container, available_width, block_container.computed_values().max_width(), layout_input.containing_block_constraints);
                if (used_width_px > max_width_px)
                    used_width_px = max_width_px;
            }

            auto should_treat_min_width_as_auto = [&] {
                auto const& available_width = available_space.width;
                auto const& min_width = block_container.computed_values().min_width();
                if (min_width.is_auto())
                    return true;
                if (min_width.is_fit_content() && available_width.is_intrinsic_sizing_constraint())
                    return true;
                if (min_width.is_max_content() && available_width.is_max_content())
                    return true;
                if (min_width.is_min_content() && available_width.is_min_content())
                    return true;
                return false;
            }();
            if (!should_treat_min_width_as_auto) {
                auto min_width_px = calculate_inner_width(block_container, available_width, block_container.computed_values().min_width(), layout_input.containing_block_constraints);
                if (used_width_px < min_width_px)
                    used_width_px = min_width_px;
            }
        }
        block_container_state.set_content_width(used_width_px);
        block_container_state.set_content_height(context.automatic_content_height());
    }
}

CSSPixels BlockFormattingContext::compute_auto_height_for_block_level_element(Box const& box, AvailableSpace const& available_space, ContainingBlockConstraints const& containing_block_constraints)
{
    if (creates_block_formatting_context(box)) {
        return compute_auto_height_for_block_formatting_context_root(box);
    }

    auto const& box_state = m_state.get(box);

    auto display = box.display();
    if (display.is_flex_inside()) {
        // https://drafts.csswg.org/css-flexbox-1/#algo-main-container
        // NOTE: The automatic block size of a block-level flex container is its max-content size.
        return calculate_max_content_height(box, available_space.width.to_px_or_zero(), containing_block_constraints);
    }
    if (display.is_grid_inside()) {
        // https://www.w3.org/TR/css-grid-2/#intrinsic-sizes
        // In both inline and block formatting contexts, the grid container’s auto block size is its
        // max-content size.
        return calculate_max_content_height(box, available_space.width.to_px_or_zero(), containing_block_constraints);
    }
    if (display.is_table_inside()) {
        return calculate_max_content_height(box, available_space.width.to_px_or_zero(), containing_block_constraints);
    }

    // https://www.w3.org/TR/CSS22/visudet.html#normal-block
    // 10.6.3 Block-level non-replaced elements in normal flow when 'overflow' computes to 'visible'

    // The element's height is the distance from its top content edge to the first applicable of the following:

    // 1. the bottom edge of the last line box, if the box establishes a inline formatting context with one or more lines
    if (box.children_are_inline() && !box_state.line_boxes.is_empty()) {
        auto height = box_state.line_boxes.last().bottom();
        if (box_state.line_boxes.last().has_block_level_box()) {
            auto margin_bottom = m_margin_state.current_collapsed_margin();
            if (box_state.padding_bottom == 0 && box_state.border_bottom == 0) {
                m_margin_state.set_box_last_in_flow_child_margin_bottom_collapsed(true);
                margin_bottom = 0;
            }
            height = max(CSSPixels(0), height + margin_bottom);
        }
        return height;
    }

    // 2. the bottom edge of the bottom (possibly collapsed) margin of its last in-flow child, if the child's bottom margin does not collapse with the element's bottom margin
    // 3. the bottom border edge of the last in-flow child whose top margin doesn't collapse with the element's bottom margin
    if (!box.children_are_inline()) {
        CSSPixels marker_line_height = 0;
        for (auto* child_box = box.last_child_of_type<Box>(); child_box; child_box = child_box->previous_sibling_of_type<Box>()) {
            if (child_box->is_absolutely_positioned() || child_box->is_floating())
                continue;

            // NOTE: Markers are not in-flow, but for list items that contain only floats (or are otherwise empty),
            //       the marker's line-height determines the list item's height. This ensures proper vertical stacking
            //       of list items and alignment with their floated content.
            if (child_box->is_list_item_marker_box()) {
                marker_line_height = child_box->computed_values().line_height();
                continue;
            }

            auto const& child_box_state = m_state.get(*child_box);

            if (margins_collapse_through(*child_box, m_state))
                continue;

            auto margin_bottom = m_margin_state.current_collapsed_margin();
            if (box_state.padding_bottom == 0 && box_state.border_bottom == 0) {
                m_margin_state.set_box_last_in_flow_child_margin_bottom_collapsed(true);
                margin_bottom = 0;
            }

            return max(CSSPixels(0), child_box_state.content_offset().y() + child_box_state.content_height() + child_box_state.border_box_bottom() + margin_bottom);
        }

        // If no in-flow children were found but there's a marker, use the marker's line-height.
        if (marker_line_height > 0)
            return marker_line_height;
    }

    // AD-HOC: Contenteditable elements must have a minimum height (line-height) when empty, to remain clickable and
    //         usable for text input, even though this is not specified.
    //         See: https://github.com/w3c/editing/issues/70.
    if (auto const* element = as_if<DOM::Element>(box.dom_node()); element && element->is_editing_host())
        return box.computed_values().line_height();

    // 4. zero, otherwise
    return 0;
}

void BlockFormattingContext::layout_interrupting_block_inside_inline_context(Box const& box, BlockContainer const& containing_block, LayoutInput const& layout_input, LineBuilder& line_builder)
{
    CSSPixels dummy_bottom_of_lowest_margin_box = 0;
    CSSPixels block_bottom;
    {
        TemporaryChange<Optional<CSSPixels>> change { m_y_offset_of_current_block_container, line_builder.current_block_offset() };
        layout_block_level_box(box, containing_block, dummy_bottom_of_lowest_margin_box, layout_input);
        block_bottom = m_y_offset_of_current_block_container.value_or(line_builder.current_block_offset());
    }
    line_builder.append_block_level_box(box, block_bottom, m_margin_state.current_collapsed_margin());
}

CSSPixels BlockFormattingContext::commit_pending_margin_before_inline_content()
{
    auto has_open_top_margin_group = m_margin_state.has_open_top_margin_group();
    auto collapsed_margin = m_margin_state.current_collapsed_margin();

    m_margin_state.update_open_top_margin_group();
    m_margin_state.reset();

    return has_open_top_margin_group ? CSSPixels(0) : collapsed_margin;
}

void BlockFormattingContext::layout_block_level_box(Box const& box, BlockContainer const& block_container, CSSPixels& bottom_of_lowest_margin_box, LayoutInput const& layout_input)
{
    auto const& available_space = layout_input.available_space;

    if (box.is_absolutely_positioned()) {
        if (m_layout_mode == LayoutMode::Normal) {
            // NB: An originally-inline absolutely positioned box never reaches this path; the tree
            //     builder keeps out-of-flow boxes in inline context, where static position markers
            //     pin them at their exact flow position.
            StaticPositionRect static_position;
            static_position.rect = { { 0, m_y_offset_of_current_block_container.value() }, { 0, 0 } };
            register_contained_abspos_child(box, static_position);
        }
        return;
    }

    // NOTE: ListItemMarkerBoxes are placed by their corresponding ListItemBox, and their used
    //       values were created together with its own.
    if (is<ListItemMarkerBox>(box))
        return;

    // NOTE: It is possible to encounter SVGMaskBox and SVGClipBox nodes while doing layout of the
    //       formatting context established by a <foreignObject> that references them. Skip them
    //       before creating any used values; SVGFormattingContext lays them out on behalf of the
    //       referencing element.
    if (box.is_svg_mask_box() || box.is_svg_clip_box())
        return;

    auto const& block_container_state = m_state.get(block_container);
    auto& box_state = [&]() -> LayoutState::UsedValues& {
        auto const* document_element = box.document().document_element();
        if (!m_state.has_subtree_root()
            && document_element && document_element->unsafe_layout_node()
            && box.is_inclusive_ancestor_of(*document_element->unsafe_layout_node())) {
            if (auto* existing_state = m_state.try_get_mutable(box))
                return *existing_state;
        }
        return m_state.create(box, layout_input.containing_block_constraints.percentage_basis_width, layout_input.containing_block_constraints.percentage_basis_height);
    }();

    resolve_vertical_box_model_metrics(box, block_container_state.content_width());

    VERIFY(box.containing_block() == &block_container);
    auto containing_block_position_in_root = layout_input.content_box_position_in_bfc_root.value();

    if (box.is_floating()) {
        auto const y = m_y_offset_of_current_block_container.value();
        auto margin_top = m_margin_state.has_open_top_margin_group() ? 0 : m_margin_state.current_collapsed_margin();
        layout_floating_box(box, block_container, layout_input, margin_top + y);
        bottom_of_lowest_margin_box = max(bottom_of_lowest_margin_box, m_floats.last()->bottom_margin_edge);
        return;
    }

    m_margin_state.add_margin(box_state.margin_top);
    auto introduce_clearance = clear_floating_boxes(box, {}, containing_block_position_in_root);
    if (introduce_clearance == DidIntroduceClearance::Yes)
        m_margin_state.reset();
    m_margin_state.update_open_top_margin_group();

    auto const y = m_y_offset_of_current_block_container.value();

    auto box_is_html_element_in_quirks_mode = box.document().in_quirks_mode()
        && box.dom_node()
        && box.dom_node()->is_html_html_element()
        && box.computed_values().height().is_auto();

    // NOTE: In quirks mode, the html element's height matches the viewport so it can be treated as definite
    if (box_state.has_definite_height() || box_is_html_element_in_quirks_mode)
        resolve_used_height_if_treated_as_auto(box, available_space, layout_input.containing_block_constraints);

    auto independent_formatting_context = create_independent_formatting_context_if_needed(m_state, m_layout_mode, box, this);

    if (!independent_formatting_context && !is<BlockContainer>(box)) {
        dbgln("FIXME: Block-level box is not BlockContainer but does not create formatting context: {}", box.debug_description());
        return;
    }

    CSSPixels margin_top = m_margin_state.current_collapsed_margin();

    if (m_margin_state.has_open_top_margin_group()) {
        // If first child margin top will collapse with margin-top of containing block then margin-top of child is 0
        margin_top = 0;
    }

    auto box_opens_top_margin_group = !independent_formatting_context
        && box_state.border_top == 0 && box_state.padding_top == 0
        && !m_margin_state.has_open_top_margin_group();

    auto const* fieldset_box = as_if<FieldSetBox>(block_container);
    auto box_is_positioned_by_fieldset_layout = fieldset_box && fieldset_box->rendered_legend() == &box;

    // Earlier sibling placement may have invalidated cached float bands.
    rebuild_float_bands();

    auto content_y = y + margin_top + box_state.border_box_top();

    auto const containing_block_rect_in_root = CSSPixelRect { containing_block_position_in_root, block_container_state.content_size() };

    auto const containing_block_rect_in_root_now = containing_block_rect_in_root.translated(0, y_adjustment_from_pending_ancestor_top_margins(box));
    auto content_position_in_root_now = [&](CSSPixels content_y) {
        return containing_block_rect_in_root_now.location().translated(0, content_y);
    };

    compute_width(box, available_space, layout_input.containing_block_constraints, content_position_in_root_now(content_y));
    content_y = avoid_float_intrusions(box, available_space, layout_input.containing_block_constraints, content_y, containing_block_rect_in_root_now);

    auto content_x = compute_normal_flow_x(box, available_space, content_position_in_root_now(content_y));

    // FIXME: We currently so not support ListItemBox-es generated by pseudo-elements. We will need to, eventually.
    auto const* list_item_box = as_if<ListItemBox>(box);
    auto is_list_item_box_without_css_content = list_item_box != nullptr;
    if (auto const* dom_node = as_if<DOM::Element>(box.dom_node()); list_item_box && dom_node) {
        if (auto const computed_values = dom_node->computed_values(CSS::PseudoElement::Marker))
            is_list_item_box_without_css_content = !computed_values->content().has_value();
    }

    if (is_list_item_box_without_css_content && list_item_box->marker()) {
        dimension_list_item_marker(*list_item_box->marker());

        auto const& marker = *list_item_box->marker();
        if (marker.list_style_position() == CSS::ListStylePosition::Inside
            && box.computed_values().direction() == CSS::Direction::Ltr) {
            content_x += m_state.get(marker).content_width() + distance_between_marker_and_list_item(marker);
        }
    }

    auto* table_formatting_context = independent_formatting_context && independent_formatting_context->type() == Type::Table
        ? static_cast<TableFormattingContext*>(independent_formatting_context.ptr())
        : nullptr;

    Optional<CSSPixelPoint> pending_position;

    if (box_is_positioned_by_fieldset_layout) {
        m_pending_legend_flow_position = CSSPixelPoint { content_x, content_y };
    } else if (table_formatting_context) {
        table_formatting_context->set_pending_table_box_content_offset_in_wrapper({ content_x, content_y });
    } else if (!box_opens_top_margin_group) {
        pending_position = CSSPixelPoint { content_x, content_y };
    }

    AvailableSpace available_space_for_height_resolution = available_space;
    auto is_table_box = box.display().is_table_row() || box.display().is_table_row_group() || box.display().is_table_header_group() || box.display().is_table_footer_group() || box.display().is_table_cell() || box.display().is_table_caption();
    // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
    auto shadow_root = box.dom_node() ? box.dom_node()->containing_shadow_root() : nullptr;
    bool is_in_ua_internal_shadow_tree = shadow_root && shadow_root->is_user_agent_internal();
    if (box.document().in_quirks_mode() && box.computed_values().height().is_percentage() && !is_table_box && !is_in_ua_internal_shadow_tree) {
        available_space_for_height_resolution.height = AvailableSize::make_definite(layout_input.containing_block_constraints.quirks_mode_percentage_basis_height.value_or(0));
    }

    resolve_used_height_if_not_treated_as_auto(box, available_space_for_height_resolution, layout_input.containing_block_constraints);

    // NOTE: Flex containers with `auto` height are treated as `max-content`, so we can compute their height early.
    if (box.has_auto_content_box_size() || box.display().is_flex_inside()) {
        resolve_used_height_if_treated_as_auto(box, available_space_for_height_resolution, layout_input.containing_block_constraints);
    }

    // Before we insert the children of a list item we need to know the location of the marker.
    // If we do not do this then left-floating elements inside the list item will push the marker to the right,
    // in some cases even causing it to overlap with the non-floating content of the list.
    SpaceUsedByFloats inline_space_used_before_children_formatted;
    if (is_list_item_box_without_css_content && list_item_box->marker()) {
        auto const& list_item_state = m_state.get(*list_item_box);
        auto const& marker_state = m_state.get(*list_item_box->marker());

        auto offset_y = max(CSSPixels(0), (list_item_box->marker()->computed_values().line_height() - marker_state.content_height()) / 2);
        inline_space_used_before_children_formatted = intrusion_by_floats_into_rect({ content_position_in_root_now(content_y).translated(content_x, 0), list_item_state.content_size() }, offset_y, offset_y);
    }

    if (independent_formatting_context) {
        // Margins of elements that establish new formatting contexts do not collapse with their in-flow children
        m_margin_state.reset();

        // This box establishes a new formatting context. Pass control to it.
        auto inner_available_space = box_state.available_inner_space_or_constraints_from(available_space);

        // For boxes with auto height but non-auto min-height, we need to determine if the content height is less than
        // min-height. If so, we run layout with min-height as the available height.
        Optional<CSSPixels> measured_content_height;
        if (should_treat_height_as_auto(box, available_space, layout_input.containing_block_constraints) && !box.computed_values().min_height().is_auto()) {
            auto content_height = measure_automatic_content_height(box, inner_available_space, layout_input.containing_block_constraints);
            measured_content_height = content_height;
            auto min_height = calculate_inner_height(box, available_space, box.computed_values().min_height(), layout_input.containing_block_constraints);
            if (content_height < min_height) {
                inner_available_space.height = AvailableSize::make_definite(min_height);
            }
        }

        make_button_content_box_definite(box, available_space, layout_input.containing_block_constraints, measured_content_height);

        auto inside_layout_input = [&] {
            auto input = layout_input.for_child_formatting_context(inner_available_space);
            if (table_formatting_context && layout_input.table_grid_min_border_box_height.has_value())
                return input.with_table_grid_min_border_box_height(*layout_input.table_grid_min_border_box_height);
            return input;
        }();
        independent_formatting_context->run(inside_layout_input);
        if (table_formatting_context)
            pending_position = table_formatting_context->pending_table_box_content_offset_in_wrapper();
        if (is<TableWrapper>(block_container) && box.display().is_table_inside()) {
            box_state.margin_left = max(box_state.margin_left, 0);
            box_state.margin_right = max(box_state.margin_right, 0);
        }
        if (is<TableWrapper>(box) && !box.is_grid_item())
            box_state.set_content_width(independent_formatting_context->automatic_content_width());
    } else {
        // This box participates in the current block container's flow.
        auto space_available_for_children = box.is_anonymous() ? available_space : box_state.available_inner_space_or_constraints_from(available_space);
        if (box_state.border_top > 0 || box_state.padding_top > 0) {
            // margin-top of block container can't collapse with its children if it has non-zero border or padding.
            m_margin_state.reset();
        } else if (box_opens_top_margin_group) {
            m_margin_state.open_top_margin_group(box, introduce_clearance == DidIntroduceClearance::Yes);
        }

        auto inside_layout_input = layout_input.with_content_box_position_in_bfc_root(
            containing_block_position_in_root.translated(content_x, content_y));

        if (box.children_are_inline())
            layout_inline_children(as<BlockContainer>(box), inside_layout_input, space_available_for_children);
        else
            layout_block_level_children(as<BlockContainer>(box), inside_layout_input, space_available_for_children);

        if (box_opens_top_margin_group) {
            auto resolved_margin_top = m_margin_state.take_pending_top_margin();
            auto final_content_y = introduce_clearance == DidIntroduceClearance::No
                ? y + resolved_margin_top + box_state.border_box_top()
                : content_y;
            if (box_is_positioned_by_fieldset_layout) {
                m_pending_legend_flow_position = CSSPixelPoint { content_x, final_content_y };
            } else {
                pending_position = CSSPixelPoint { content_x, final_content_y };
            }
            translate_floats_in_subtree(box, { 0, final_content_y - content_y });
        }
    }

    // Tables already set their height during the independent formatting context run. When multi-line text cells are involved, using different
    // available space here than during the independent formatting context run can result in different line breaks and thus a different height.
    if (!box.display().is_table_inside()) {
        resolve_used_height_if_treated_as_auto(box, available_space_for_height_resolution, layout_input.containing_block_constraints, independent_formatting_context);
    }

    // Now that our children are formatted we place the ListItemBox with the left space we remembered.
    if (is_list_item_box_without_css_content)
        // The marker pseudo-element will be created from a ListItemMarkerBox
        layout_list_item_marker(*list_item_box, inline_space_used_before_children_formatted);
    // Otherwise, it will be a dealt with as a generic pseudo-element with the content of the ::marker pseudo-element.

    if (pending_position.has_value())
        place_child(box, *pending_position);

    if (independent_formatting_context || !margins_collapse_through(box, m_state)) {
        if (!m_margin_state.box_last_in_flow_child_margin_bottom_collapsed()) {
            m_margin_state.reset();
        }
        m_y_offset_of_current_block_container = box_state.content_offset().y() + box_state.content_height() + box_state.border_box_bottom();
    }
    m_margin_state.set_box_last_in_flow_child_margin_bottom_collapsed(false);

    m_margin_state.add_margin(box_state.margin_bottom);
    m_margin_state.update_open_top_margin_group();

    compute_inset(box, content_box_rect(block_container_state).size());

    bottom_of_lowest_margin_box = max(bottom_of_lowest_margin_box, box_state.content_offset().y() + box_state.content_height() + box_state.margin_box_bottom());

    if (independent_formatting_context)
        independent_formatting_context->parent_context_did_dimension_child_root_box();
}

void BlockFormattingContext::layout_block_level_children(BlockContainer const& block_container, LayoutInput const& layout_input, AvailableSpace const& available_space_for_children)
{
    VERIFY(!block_container.children_are_inline());

    auto const& available_space = available_space_for_children;
    // The table wrapper is invisible to percentage resolution: percentages on the table root
    // resolve against the wrapper's containing block, so the wrapper's own constraints pass
    // through to the table box unchanged.
    auto child_layout_input = [&]() -> LayoutInput {
        if (is<TableWrapper>(block_container))
            return LayoutInput { available_space_for_children, layout_input.containing_block_constraints, layout_input.content_box_position_in_bfc_root, layout_input.table_grid_min_border_box_height };
        else
            return layout_input_for_child_context(m_state.get(block_container), layout_input, available_space_for_children);
    }();

    CSSPixels bottom_of_lowest_margin_box = 0;

    TemporaryChange<Optional<CSSPixels>> change { m_y_offset_of_current_block_container, CSSPixels(0) };
    block_container.for_each_child_of_type<Box>([&](Box& box) {
        layout_block_level_box(box, block_container, bottom_of_lowest_margin_box, child_layout_input);
        return IterationDecision::Continue;
    });

    if (m_layout_mode == LayoutMode::IntrinsicSizing) {
        auto& block_container_state = m_state.get_mutable(block_container);
        if (!block_container_state.has_definite_width()) {
            auto width = greatest_child_width_in_rect(block_container, { layout_input.content_box_position_in_bfc_root->translated(0, y_adjustment_from_pending_ancestor_top_margins(block_container)), block_container_state.content_size() });
            auto const& computed_values = block_container.computed_values();
            // NOTE: Min and max constraints are not applied to a box that is being sized as intrinsic because
            //       according to css-sizing-3 spec:
            //       The min-content size of a box in each axis is the size it would have if it was a float given an
            //       auto size in that axis (and no minimum or maximum size in that axis) and if its containing block
            //       was zero-sized in that axis.
            if (block_container_state.width_constraint == SizeConstraint::None) {
                if (!should_treat_max_width_as_none(block_container, available_space.width, layout_input.containing_block_constraints)) {
                    auto max_width = calculate_inner_width(block_container, available_space.width,
                        computed_values.max_width(), layout_input.containing_block_constraints);
                    width = min(width, max_width);
                }
                if (!computed_values.min_width().is_auto()) {
                    auto min_width = calculate_inner_width(block_container, available_space.width,
                        computed_values.min_width(), layout_input.containing_block_constraints);
                    width = max(width, min_width);
                }
            }
            block_container_state.set_content_width(width);
            block_container_state.set_content_height(bottom_of_lowest_margin_box);
        }
    }

    compute_and_store_baselines(m_state.get_mutable(block_container));
}

// https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
void BlockFormattingContext::layout_fieldset_with_rendered_legend(FieldSetBox const& fieldset_box, LayoutInput const& layout_input)
{
    auto const& available_space = layout_input.available_space;
    auto child_layout_input = layout_input_for_child_context(m_state.get(fieldset_box), layout_input, available_space);

    auto& fieldset_state = m_state.get_mutable(fieldset_box);

    auto legend = fieldset_box.rendered_legend();
    VERIFY(legend);

    // Lay out the legend to determine its dimensions.
    {
        TemporaryChange<Optional<CSSPixels>> change { m_y_offset_of_current_block_container, CSSPixels(0) };
        CSSPixels dummy_bottom = 0;
        layout_block_level_box(*legend, fieldset_box, dummy_bottom, child_layout_input);
    }

    // If the computed value of 'inline-size' is 'auto', then the used value is the fit-content inline size.
    auto& legend_state = m_state.get_mutable(*legend);
    if (legend->computed_values().width().is_auto()) {
        auto width = calculate_fit_content_width(*legend, available_space, child_layout_input.containing_block_constraints);
        legend_state.set_content_width(width);
    }

    // The space allocated for the element's border on the block-start side is expected to be the element's
    // 'border-block-start-width' or the rendered legend's margin box size in the fieldset's block-flow direction,
    // whichever is greater.
    auto effective_border = max(fieldset_state.border_top, legend_state.margin_box_height());
    auto extra_top = effective_border - fieldset_state.border_top;

    // Lay out non-legend children below the legend accommodation.
    m_margin_state.reset();

    CSSPixels bottom_of_lowest_margin_box = 0;
    {
        TemporaryChange<Optional<CSSPixels>> change { m_y_offset_of_current_block_container, extra_top };
        fieldset_box.for_each_child_of_type<Box>([&](Box& child) {
            if (&child == legend)
                return IterationDecision::Continue;
            layout_block_level_box(child, fieldset_box, bottom_of_lowest_margin_box, child_layout_input);
            return IterationDecision::Continue;
        });
    }

    if (m_layout_mode == LayoutMode::IntrinsicSizing && !fieldset_state.has_definite_width()) {
        auto width = greatest_child_width(fieldset_box);
        auto const& computed_values = fieldset_box.computed_values();
        if (fieldset_state.width_constraint == SizeConstraint::None) {
            if (!should_treat_max_width_as_none(fieldset_box, available_space.width, layout_input.containing_block_constraints)) {
                auto max_width = calculate_inner_width(fieldset_box, available_space.width, computed_values.max_width(), layout_input.containing_block_constraints);
                width = min(width, max_width);
            }
            if (!computed_values.min_width().is_auto()) {
                auto min_width = calculate_inner_width(fieldset_box, available_space.width, computed_values.min_width(), layout_input.containing_block_constraints);
                width = max(width, min_width);
            }
        }
        fieldset_state.set_content_width(width);
        fieldset_state.set_content_height(bottom_of_lowest_margin_box);
    }

    // The element is expected to be positioned in the block-flow direction such that its border box is centered over
    // the border on the block-start side of the fieldset element.
    // FIXME: Take writing modes into consideration.
    auto legend_border_box_centering_offset = (effective_border - legend_state.border_box_height()) / 2;
    auto fieldset_border_box_top_in_content = -(fieldset_state.border_top + fieldset_state.padding_top);
    auto legend_content_y = fieldset_border_box_top_in_content + legend_border_box_centering_offset + legend_state.border_box_top();
    if (auto legend_flow_position = m_pending_legend_flow_position; legend_flow_position.has_value()) {
        m_pending_legend_flow_position = {};
        place_child(*legend, { legend_flow_position->x(), legend_content_y });
        translate_floats_in_subtree(*legend, { 0, legend_content_y - legend_flow_position->y() });
    }

    compute_and_store_baselines(fieldset_state);
}

void BlockFormattingContext::resolve_vertical_box_model_metrics(Box const& box, CSSPixels width_of_containing_block)
{
    auto& box_state = m_state.get_mutable(box);
    auto const& computed_values = box.computed_values();

    box_state.margin_top = computed_values.margin().top().to_px_or_zero(width_of_containing_block);
    box_state.margin_bottom = computed_values.margin().bottom().to_px_or_zero(width_of_containing_block);
    box_state.border_top = computed_values.border_top().width;
    box_state.border_bottom = computed_values.border_bottom().width;
    box_state.padding_top = computed_values.padding().top().to_px_or_zero(width_of_containing_block);
    box_state.padding_bottom = computed_values.padding().bottom().to_px_or_zero(width_of_containing_block);
}

void BlockFormattingContext::resolve_horizontal_box_model_metrics(Box const& box, CSSPixels width_of_containing_block)
{
    auto& box_state = m_state.get_mutable(box);
    auto const& computed_values = box.computed_values();

    box_state.margin_left = computed_values.margin().left().to_px_or_zero(width_of_containing_block);
    box_state.margin_right = computed_values.margin().right().to_px_or_zero(width_of_containing_block);
    box_state.border_left = computed_values.border_left().width;
    box_state.border_right = computed_values.border_right().width;
    box_state.padding_left = computed_values.padding().left().to_px_or_zero(width_of_containing_block);
    box_state.padding_right = computed_values.padding().right().to_px_or_zero(width_of_containing_block);
}

BlockFormattingContext::DidIntroduceClearance BlockFormattingContext::clear_floating_boxes(NodeWithStyle const& child_box, Optional<InlineFormattingContext&> inline_formatting_context, CSSPixelPoint containing_block_position_in_root)
{
    auto const& computed_values = child_box.computed_values();
    auto result = DidIntroduceClearance::No;

    auto clear_floating_boxes = [&](CSSPixels clearance_y_in_root) {
        if (clearance_y_in_root == 0)
            return;

        // NOTE: Floating boxes are globally relevant within this BFC, *but* their offset coordinates
        //       are relative to their containing block.
        //       This means that we have to first convert to a root-space Y coordinate before clearing,
        //       and then convert back to a local Y coordinate when assigning the cleared offset to
        //       the `child_box` layout state.
        CSSPixels clearance_y_in_containing_block = clearance_y_in_root
            - containing_block_position_in_root.y() - y_adjustment_from_pending_ancestor_top_margins(child_box);

        if (inline_formatting_context.has_value()) {
            if (clearance_y_in_containing_block > inline_formatting_context->vertical_float_clearance()) {
                result = DidIntroduceClearance::Yes;
                inline_formatting_context->set_vertical_float_clearance(clearance_y_in_containing_block);
            }
        } else if (clearance_y_in_containing_block > m_y_offset_of_current_block_container.value()) {
            result = DidIntroduceClearance::Yes;
            m_y_offset_of_current_block_container = clearance_y_in_containing_block;
        }
    };

    // FIXME: Honor writing-mode, direction and text-orientation.
    if (first_is_one_of(computed_values.clear(), CSS::Clear::Left, CSS::Clear::Both, CSS::Clear::InlineStart))
        clear_floating_boxes(m_lowest_left_margin_edge);
    if (first_is_one_of(computed_values.clear(), CSS::Clear::Right, CSS::Clear::Both, CSS::Clear::InlineEnd))
        clear_floating_boxes(m_lowest_right_margin_edge);

    return result;
}

CSSPixels BlockFormattingContext::compute_normal_flow_x(Box const& child_box, AvailableSpace const& available_space, CSSPixelPoint content_position_in_root) const
{
    auto const& box_state = m_state.get(child_box);

    CSSPixels x = 0;
    CSSPixels available_width_within_containing_block = available_space.width.to_px_or_zero();

    if (box_should_avoid_floats_because_it_establishes_fc(child_box)) {
        auto space_used_by_floats = intrusion_by_floats_into_rect({ content_position_in_root, box_state.content_size() }, 0, 0);
        available_width_within_containing_block -= space_used_by_floats.left + space_used_by_floats.right;

        // Subtracting the left margin here because it is applied again when the margin box offset is added below.
        x = border_box_left_of_box_avoiding_floats(child_box, box_state, space_used_by_floats) - box_state.margin_left;
    }

    if (child_box.containing_block()->computed_values().text_align() == CSS::TextAlign::LibwebCenter) {
        x += (available_width_within_containing_block / 2) - box_state.content_width() / 2;
    } else if (child_box.containing_block()->computed_values().text_align() == CSS::TextAlign::LibwebRight) {
        // Subtracting the left margin here because left and right margins need to be swapped when aligning to the right
        x += available_width_within_containing_block - box_state.content_width() - box_state.margin_box_left();
    } else {
        x += box_state.margin_box_left();
    }

    return x;
}

void BlockFormattingContext::layout_floating_box(Box const& box, BlockContainer const& block_container, LayoutInput const& layout_input, CSSPixels y, LineBuilder* line_builder)
{
    auto const& available_space = layout_input.available_space;
    VERIFY(box.is_floating());

    auto& box_state = m_state.get_mutable(box);
    auto const& computed_values = box.computed_values();

    auto const& block_container_state = m_state.get(block_container);
    resolve_vertical_box_model_metrics(box, block_container_state.content_width());

    auto const containing_block_rect_in_root = CSSPixelRect { layout_input.content_box_position_in_bfc_root.value(), block_container_state.content_size() };
    auto const containing_block_rect_in_root_now = containing_block_rect_in_root.translated(0, y_adjustment_from_pending_ancestor_top_margins(block_container));

    compute_width(box, available_space, layout_input.containing_block_constraints, containing_block_rect_in_root_now.location());

    resolve_used_height_if_not_treated_as_auto(box, available_space, layout_input.containing_block_constraints);

    // NOTE: Flex containers with `auto` height are treated as `max-content`, so we can compute their height early.
    if (box.has_auto_content_box_size() || box.display().is_flex_inside()) {
        resolve_used_height_if_treated_as_auto(box, available_space, layout_input.containing_block_constraints);
    }

    auto independent_formatting_context = layout_inside(box, m_layout_mode, layout_input.for_child_formatting_context(box_state.available_inner_space_or_constraints_from(available_space)));
    // A floating table wrapper shrink-to-fits from cached intrinsic sizes, which may not match
    // the width table layout just produced; the wrapper is exactly as wide as the table grid box.
    if (is<TableWrapper>(box) && independent_formatting_context)
        box_state.set_content_width(independent_formatting_context->automatic_content_width());
    resolve_used_height_if_treated_as_auto(box, available_space, layout_input.containing_block_constraints, independent_formatting_context);

    // Next, float to the left and/or right
    // FIXME: Honor writing-mode, direction and text-orientation.
    Optional<FloatSide> side;
    if (box.computed_values().float_() == CSS::Float::Left || box.computed_values().float_() == CSS::Float::InlineStart) {
        side = FloatSide::Left;
    } else if (box.computed_values().float_() == CSS::Float::Right || box.computed_values().float_() == CSS::Float::InlineEnd) {
        side = FloatSide::Right;
    }

    if (!side.has_value())
        return;

    auto margin_box_ceiling = line_builder ? line_builder->ceiling_for_float_to_be_inserted_here(box) : y;
    auto clearance = computed_values.clear();
    if (side.value() == FloatSide::Left && first_is_one_of(clearance, CSS::Clear::Left, CSS::Clear::Both, CSS::Clear::InlineStart))
        margin_box_ceiling = max(margin_box_ceiling, m_lowest_left_margin_edge - containing_block_rect_in_root_now.y());
    if (side.value() == FloatSide::Right && first_is_one_of(clearance, CSS::Clear::Right, CSS::Clear::Both, CSS::Clear::InlineEnd))
        margin_box_ceiling = max(margin_box_ceiling, m_lowest_right_margin_edge - containing_block_rect_in_root_now.y());

    auto ceiling_in_root = containing_block_rect_in_root_now.y() + margin_box_ceiling;
    if (!m_floats.is_empty())
        ceiling_in_root = max(ceiling_in_root, m_floats.last()->margin_box_rect_in_root_coordinate_space.top() + y_adjustment_from_pending_ancestor_top_margins(m_floats.last()->box));

    auto placement = place_float(side.value(), box_state, available_space, containing_block_rect_in_root_now, ceiling_in_root);
    auto content_y = placement.block_start - containing_block_rect_in_root_now.y() + box_state.margin_box_top();

    auto margin_box_rect_in_root = margin_box_rect(box_state)
                                       .translated(0, content_y)
                                       .translated(containing_block_rect_in_root.location());
    m_floats.append(adopt_own(*new FloatingBox {
        .box = box,
        .used_values = box_state,
        .side = side.value(),
        .offset_from_edge = placement.offset_from_edge,
        .top_margin_edge = content_y - box_state.margin_box_top(),
        .bottom_margin_edge = content_y + box_state.content_height() + box_state.margin_box_bottom(),
        .margin_box_rect_in_root_coordinate_space = margin_box_rect_in_root,
        .containing_block_rect_in_root_coordinate_space = containing_block_rect_in_root,
        .percentage_basis_width = layout_input.containing_block_constraints.percentage_basis_width,
    }));
    auto& floating_box = *m_floats.last();
    floating_box.margin_box_rect_in_root_coordinate_space.set_x(margin_box_left_of_float_in_root(floating_box, containing_block_rect_in_root));
    add_float_to_bands(floating_box, containing_block_rect_in_root);

    auto& root_state = m_state.get_mutable(root());
    auto bottom_margin_edge = m_floats.last()->margin_box_rect_in_root_coordinate_space.bottom();
    auto lowest = root_state.lowest_floating_descendant_bottom_margin_edge();
    if (!lowest.has_value() || bottom_margin_edge > *lowest)
        root_state.set_lowest_floating_descendant_bottom_margin_edge(bottom_margin_edge);

    if (line_builder)
        line_builder->recalculate_available_space();

    compute_inset(box, content_box_rect(block_container_state).size());

    if (independent_formatting_context)
        independent_formatting_context->parent_context_did_dimension_child_root_box();
}

void BlockFormattingContext::layout_list_item_marker(ListItemBox const& list_item_box, SpaceUsedByFloats const& inline_space_used_before_list_item_elements_formatted)
{
    if (!list_item_box.marker())
        return;

    auto& marker = *list_item_box.marker();
    auto& marker_state = m_state.get_mutable(marker);
    auto& list_item_state = m_state.get_mutable(list_item_box);

    auto marker_distance = distance_between_marker_and_list_item(marker);

    auto marker_height = marker_state.content_height();
    auto marker_width = marker_state.content_width();

    auto list_item_direction = list_item_box.computed_values().direction();
    auto marker_offset_x = list_item_direction == CSS::Direction::Ltr
        ? inline_space_used_before_list_item_elements_formatted.left - marker_distance - marker_width
        : list_item_state.content_width() - (inline_space_used_before_list_item_elements_formatted.right - marker_distance);
    auto marker_offset_y = max(CSSPixels(0), (marker.computed_values().line_height() - marker_height) / 2);

    if (marker.list_style_position() == CSS::ListStylePosition::Inside) {
        // FIXME: Just adjusting the content width for an inside marker is wrong, as it will still position
        //        the marker outside of the box, instead of treating it more like an inline child on the first line.
        list_item_state.set_content_width(list_item_state.content_width() - marker_width - marker_distance);
    }

    // Animations can make `float` or `position` apply to ::marker.
    if (!marker.is_floating() && !marker.is_absolutely_positioned())
        place_child(marker, { round(marker_offset_x), round(marker_offset_y) });

    if (marker.computed_values().line_height() > list_item_state.content_height())
        list_item_state.set_content_height(marker.computed_values().line_height());
}

CSSPixels BlockFormattingContext::greatest_child_width(Box const& box) const
{
    auto box_in_root_rect = content_box_rect_in_ancestor_coordinate_space(m_state.get(box), root());
    return greatest_child_width_in_rect(box, box_in_root_rect);
}

CSSPixels BlockFormattingContext::greatest_child_width_in_rect(Box const& box, CSSPixelRect const& box_in_root_rect) const
{
    // Similar to FormattingContext::greatest_child_width()
    // but this one takes floats into account!
    CSSPixels max_width = 0;
    for (auto const& band : m_bands) {
        auto intrusions = intrusions_for_band_into_rect(band, box_in_root_rect);
        max_width = max(max_width, intrusions.left + intrusions.right);
    }

    if (box.children_are_inline()) {
        for (auto const& line_box : m_state.get(as<BlockContainer>(box)).line_boxes) {
            auto width_here = line_box_physical_width(box, line_box);
            auto line_top = line_box.bottom() - line_box.height();
            auto line_bottom = line_box.bottom();
            CSSPixels extra_width_from_left_floats = 0;
            for (auto& left_float : m_floats) {
                if (left_float->side != FloatSide::Left)
                    continue;
                // NOTE: Floats directly affect the automatic size of their containing block, but only indirectly anything above in the tree.
                if (left_float->box.containing_block() != &box)
                    continue;
                if (line_top < left_float->bottom_margin_edge && line_bottom > left_float->top_margin_edge) {
                    extra_width_from_left_floats = max(extra_width_from_left_floats, left_float->offset_from_edge + left_float->used_values.content_width() + left_float->used_values.margin_box_right());
                }
            }
            CSSPixels extra_width_from_right_floats = 0;
            for (auto& right_float : m_floats) {
                if (right_float->side != FloatSide::Right)
                    continue;
                // NOTE: Floats directly affect the automatic size of their containing block, but only indirectly anything above in the tree.
                if (right_float->box.containing_block() != &box)
                    continue;
                if (line_top < right_float->bottom_margin_edge && line_bottom > right_float->top_margin_edge) {
                    extra_width_from_right_floats = max(extra_width_from_right_floats, right_float->offset_from_edge + right_float->used_values.margin_box_left());
                }
            }
            width_here += extra_width_from_left_floats + extra_width_from_right_floats;
            max_width = max(max_width, width_here);
        }
    } else {
        box.for_each_child_of_type<Box>([&](Box const& child) {
            if (child.is_absolutely_positioned())
                return IterationDecision::Continue;
            if (auto const* child_state = m_state.try_get(child))
                max_width = max(max_width, child_state->margin_box_width());
            return IterationDecision::Continue;
        });
    }
    return max_width;
}

// https://drafts.csswg.org/css-multicol/#pseudo-algorithm
// The pseudo-algorithm below determines the used values for column-count (N) and column-width (W). There is
// one other variable in the pseudo-algorithm: U is the used width of the multi-column container.
Optional<int> BlockFormattingContext::determine_used_value_for_column_count(CSSPixels const& U) const
{
    auto const& computed_values = root().computed_values();
    // (01)  if ((column-width = auto) and (column-count = auto)) then
    if (computed_values.column_width().is_auto() && computed_values.column_count().is_auto()) {
        // (02)      exit; /* not a multicol container */
        return {};
    }

    // (03)  if column-width = auto then
    if (computed_values.column_width().is_auto()) {
        // (04)      N := column-count
        return computed_values.column_count().value();
    }

    auto column_gap = get_column_gap_used_value_for_multicol(U);
    auto column_width = get_column_width_used_value_for_multicol(U);

    // (05)  else if column-count = auto then
    if (computed_values.column_count().is_auto()) {
        // (06)      N := max(1,
        // (07)        floor((U + column-gap)/(column-width + column-gap)))
        return max(1, ((U + column_gap) / (column_width + column_gap)).to_int());
    }

    // (08)  else
    // (09)      N := min(column-count, max(1,
    // (10)        floor((U + column-gap)/(column-width + column-gap))))
    return min(computed_values.column_count().value(), max(1, ((U + column_gap) / (column_width + column_gap)).to_int()));
}
CSSPixels BlockFormattingContext::determine_used_value_for_column_width(CSSPixels const& U, int N) const
{
    auto column_gap = get_column_gap_used_value_for_multicol(U);
    // (11)  W := max(0, (U + column-gap)/N - column-gap)
    return max(CSSPixels(0), (U + column_gap) / N - column_gap);
}

// https://drafts.csswg.org/css-multicol-2/#cw
CSSPixels BlockFormattingContext::get_column_width_used_value_for_multicol(CSSPixels const& U) const
{
    // Used values will be clamped to a minimum of '1px'.
    return max(root().computed_values().column_width().to_px(U), 1);
}

// https://www.w3.org/TR/css-align-3/#column-row-gap
CSSPixels BlockFormattingContext::get_column_gap_used_value_for_multicol(CSSPixels const& U) const
{
    // The 'normal' represents a used value of '1em' on multi-column containers
    return root().computed_values().column_gap().visit(
        [&](CSS::NormalGap) { return CSS::Length(1, CSS::LengthUnit::Em).to_px(root()); },
        [&](auto const& gap) { return gap.to_px(U); });
}

}
