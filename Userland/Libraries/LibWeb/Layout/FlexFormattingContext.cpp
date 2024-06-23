/*
 * Copyright (c) 2021-2024, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "InlineFormattingContext.h"
#include <AK/Function.h>
#include <AK/QuickSort.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/BlockFormattingContext.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/FlexFormattingContext.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>

namespace Web::Layout {

// NOTE: We use a custom clamping function here instead of AK::clamp(), since the AK version
//       will VERIFY(max >= min) and CSS explicitly allows that (see css-values-4.)
template<typename T>
[[nodiscard]] constexpr T css_clamp(T const& value, T const& min, T const& max)
{
    return ::max(min, ::min(value, max));
}

CSSPixels FlexFormattingContext::get_pixel_width(Box const& box, CSS::Size const& size) const
{
    return calculate_inner_width(box, containing_block_width_as_available_size(box), size);
}

CSSPixels FlexFormattingContext::get_pixel_height(Box const& box, CSS::Size const& size) const
{
    return calculate_inner_height(box, containing_block_height_as_available_size(box), size);
}

FlexFormattingContext::FlexFormattingContext(LayoutState& state, Box const& flex_container, FormattingContext* parent)
    : FormattingContext(Type::Flex, state, flex_container, parent)
    , m_flex_container_state(m_state.get_mutable(flex_container))
    , m_flex_direction(flex_container.computed_values().flex_direction())
{
}

FlexFormattingContext::~FlexFormattingContext() = default;

CSSPixels FlexFormattingContext::automatic_content_width() const
{
    return m_flex_container_state.content_width();
}

CSSPixels FlexFormattingContext::automatic_content_height() const
{
    return m_flex_container_state.content_height();
}

void FlexFormattingContext::run(Box const& run_box, LayoutMode, AvailableSpace const& available_space)
{
    VERIFY(&run_box == &flex_container());

    // This implements https://www.w3.org/TR/css-flexbox-1/#layout-algorithm

    // 1. Generate anonymous flex items
    generate_anonymous_flex_items();

    // 2. Determine the available main and cross space for the flex items
    determine_available_space_for_items(available_space);

    {
        // https://drafts.csswg.org/css-flexbox-1/#definite-sizes
        // 3. If a single-line flex container has a definite cross size,
        //    the automatic preferred outer cross size of any stretched flex items is the flex container’s inner cross size
        //    (clamped to the flex item’s min and max cross size) and is considered definite.
        if (is_single_line() && has_definite_cross_size(m_flex_container_state)) {
            auto flex_container_inner_cross_size = inner_cross_size(m_flex_container_state);
            for (auto& item : m_flex_items) {
                if (!flex_item_is_stretched(item))
                    continue;
                auto item_min_cross_size = has_cross_min_size(item.box) ? specified_cross_min_size(item.box) : 0;
                auto item_max_cross_size = has_cross_max_size(item.box) ? specified_cross_max_size(item.box) : CSSPixels::max();
                auto item_preferred_outer_cross_size = css_clamp(flex_container_inner_cross_size, item_min_cross_size, item_max_cross_size);
                auto item_inner_cross_size = item_preferred_outer_cross_size - item.margins.cross_before - item.margins.cross_after - item.padding.cross_before - item.padding.cross_after - item.borders.cross_before - item.borders.cross_after;
                set_cross_size(item.box, item_inner_cross_size);
                set_has_definite_cross_size(item);
            }
        }
    }

    // 3. Determine the flex base size and hypothetical main size of each item
    for (auto& item : m_flex_items) {
        if (item.box->is_replaced_box()) {
            // FIXME: Get rid of prepare_for_replaced_layout() and make replaced elements figure out their intrinsic size lazily.
            static_cast<ReplacedBox&>(*item.box).prepare_for_replaced_layout();
        }
        determine_flex_base_size_and_hypothetical_main_size(item);
    }

    if (available_space.width.is_intrinsic_sizing_constraint() || available_space.height.is_intrinsic_sizing_constraint()) {
        // We're computing intrinsic size for the flex container. This happens at the end of run().
    } else {
        // 4. Determine the main size of the flex container
        // Determine the main size of the flex container using the rules of the formatting context in which it participates.
        // NOTE: The automatic block size of a block-level flex container is its max-content size.

        // NOTE: We've already handled this in the parent formatting context.
        //       Specifically, all formatting contexts will have assigned width & height to the flex container
        //       before this formatting context runs.
    }

    // 5. Collect flex items into flex lines:
    // After this step no additional items are to be added to flex_lines or any of its items!
    collect_flex_items_into_flex_lines();

    // 6. Resolve the flexible lengths
    resolve_flexible_lengths();

    // Cross Size Determination
    // 7. Determine the hypothetical cross size of each item
    for (auto& item : m_flex_items) {
        determine_hypothetical_cross_size_of_item(item, false);
    }

    // 8. Calculate the cross size of each flex line.
    calculate_cross_size_of_each_flex_line();

    // 9. Handle 'align-content: stretch'.
    handle_align_content_stretch();

    // 10. Collapse visibility:collapse items.
    // FIXME: This

    // 11. Determine the used cross size of each flex item.
    determine_used_cross_size_of_each_flex_item();

    // 12. Distribute any remaining free space.
    distribute_any_remaining_free_space();

    // 13. Resolve cross-axis auto margins.
    resolve_cross_axis_auto_margins();

    // 14. Align all flex items along the cross-axis
    align_all_flex_items_along_the_cross_axis();

    // 15. Determine the flex container’s used cross size
    // NOTE: This is handled by the parent formatting context.

    {
        // https://drafts.csswg.org/css-flexbox-1/#definite-sizes
        // 4. Once the cross size of a flex line has been determined,
        //    the cross sizes of items in auto-sized flex containers are also considered definite for the purpose of layout.
        auto const& flex_container_computed_cross_size = is_row_layout() ? flex_container().computed_values().height() : flex_container().computed_values().width();
        if (flex_container_computed_cross_size.is_auto()) {
            for (auto& item : m_flex_items) {
                set_cross_size(item.box, item.cross_size.value());
                set_has_definite_cross_size(item);
            }
        }
    }

    {
        // NOTE: We re-resolve cross sizes here, now that we can resolve percentages.

        // 7. Determine the hypothetical cross size of each item
        for (auto& item : m_flex_items) {
            determine_hypothetical_cross_size_of_item(item, true);
        }

        // 11. Determine the used cross size of each flex item.
        determine_used_cross_size_of_each_flex_item();
    }

    // 16. Align all flex lines (per align-content)
    align_all_flex_lines();

    if (available_space.width.is_intrinsic_sizing_constraint() || available_space.height.is_intrinsic_sizing_constraint()) {
        // We're computing intrinsic size for the flex container.
        determine_intrinsic_size_of_flex_container();
    } else {
        // This is a normal layout (not intrinsic sizing).
        // AD-HOC: Finally, layout the inside of all flex items.
        copy_dimensions_from_flex_items_to_boxes();
        for (auto& item : m_flex_items) {
            if (auto independent_formatting_context = layout_inside(item.box, LayoutMode::Normal, item.used_values.available_inner_space_or_constraints_from(m_available_space_for_items->space)))
                independent_formatting_context->parent_context_did_dimension_child_root_box();

            compute_inset(item.box);
        }
    }
}

void FlexFormattingContext::parent_context_did_dimension_child_root_box()
{
    flex_container().for_each_child_of_type<Box>([&](Layout::Box& box) {
        if (box.is_absolutely_positioned()) {
            auto& cb_state = m_state.get(*box.containing_block());
            auto available_width = AvailableSize::make_definite(cb_state.content_width() + cb_state.padding_left + cb_state.padding_right);
            auto available_height = AvailableSize::make_definite(cb_state.content_height() + cb_state.padding_top + cb_state.padding_bottom);
            layout_absolutely_positioned_element(box, AvailableSpace(available_width, available_height));
        }
        return IterationDecision::Continue;
    });
}

void FlexFormattingContext::populate_specified_margins(FlexItem& item, CSS::FlexDirection flex_direction) const
{
    auto width_of_containing_block = m_flex_container_state.content_width();
    // FIXME: This should also take reverse-ness into account
    if (flex_direction == CSS::FlexDirection::Row || flex_direction == CSS::FlexDirection::RowReverse) {
        item.borders.main_before = item.box->computed_values().border_left().width;
        item.borders.main_after = item.box->computed_values().border_right().width;
        item.borders.cross_before = item.box->computed_values().border_top().width;
        item.borders.cross_after = item.box->computed_values().border_bottom().width;

        item.padding.main_before = item.box->computed_values().padding().left().to_px(item.box, width_of_containing_block);
        item.padding.main_after = item.box->computed_values().padding().right().to_px(item.box, width_of_containing_block);
        item.padding.cross_before = item.box->computed_values().padding().top().to_px(item.box, width_of_containing_block);
        item.padding.cross_after = item.box->computed_values().padding().bottom().to_px(item.box, width_of_containing_block);

        item.margins.main_before = item.box->computed_values().margin().left().to_px(item.box, width_of_containing_block);
        item.margins.main_after = item.box->computed_values().margin().right().to_px(item.box, width_of_containing_block);
        item.margins.cross_before = item.box->computed_values().margin().top().to_px(item.box, width_of_containing_block);
        item.margins.cross_after = item.box->computed_values().margin().bottom().to_px(item.box, width_of_containing_block);

        item.margins.main_before_is_auto = item.box->computed_values().margin().left().is_auto();
        item.margins.main_after_is_auto = item.box->computed_values().margin().right().is_auto();
        item.margins.cross_before_is_auto = item.box->computed_values().margin().top().is_auto();
        item.margins.cross_after_is_auto = item.box->computed_values().margin().bottom().is_auto();
    } else {
        item.borders.main_before = item.box->computed_values().border_top().width;
        item.borders.main_after = item.box->computed_values().border_bottom().width;
        item.borders.cross_before = item.box->computed_values().border_left().width;
        item.borders.cross_after = item.box->computed_values().border_right().width;

        item.padding.main_before = item.box->computed_values().padding().top().to_px(item.box, width_of_containing_block);
        item.padding.main_after = item.box->computed_values().padding().bottom().to_px(item.box, width_of_containing_block);
        item.padding.cross_before = item.box->computed_values().padding().left().to_px(item.box, width_of_containing_block);
        item.padding.cross_after = item.box->computed_values().padding().right().to_px(item.box, width_of_containing_block);

        item.margins.main_before = item.box->computed_values().margin().top().to_px(item.box, width_of_containing_block);
        item.margins.main_after = item.box->computed_values().margin().bottom().to_px(item.box, width_of_containing_block);
        item.margins.cross_before = item.box->computed_values().margin().left().to_px(item.box, width_of_containing_block);
        item.margins.cross_after = item.box->computed_values().margin().right().to_px(item.box, width_of_containing_block);

        item.margins.main_before_is_auto = item.box->computed_values().margin().top().is_auto();
        item.margins.main_after_is_auto = item.box->computed_values().margin().bottom().is_auto();
        item.margins.cross_before_is_auto = item.box->computed_values().margin().left().is_auto();
        item.margins.cross_after_is_auto = item.box->computed_values().margin().right().is_auto();
    }
}

// https://www.w3.org/TR/css-flexbox-1/#flex-items
void FlexFormattingContext::generate_anonymous_flex_items()
{
    // More like, sift through the already generated items.
    // After this step no items are to be added or removed from flex_items!
    // It holds every item we need to consider and there should be nothing in the following
    // calculations that could change that.
    // This is particularly important since we take references to the items stored in flex_items
    // later, whose addresses won't be stable if we added or removed any items.
    HashMap<int, Vector<FlexItem>> order_item_bucket;

    flex_container().for_each_child_of_type<Box>([&](Box& child_box) {
        if (can_skip_is_anonymous_text_run(child_box))
            return IterationDecision::Continue;

        // Skip any "out-of-flow" children
        if (child_box.is_out_of_flow(*this))
            return IterationDecision::Continue;

        child_box.set_flex_item(true);
        FlexItem item = { child_box, m_state.get_mutable(child_box) };
        populate_specified_margins(item, m_flex_direction);

        auto& order_bucket = order_item_bucket.ensure(child_box.computed_values().order());
        order_bucket.append(move(item));

        return IterationDecision::Continue;
    });

    auto keys = order_item_bucket.keys();

    if (is_direction_reverse()) {
        quick_sort(keys, [](auto& a, auto& b) { return a > b; });
    } else {
        quick_sort(keys, [](auto& a, auto& b) { return a < b; });
    }

    for (auto key : keys) {
        auto order_bucket = order_item_bucket.get(key);
        if (order_bucket.has_value()) {
            auto& items = order_bucket.value();
            if (is_direction_reverse()) {
                for (auto item : items.in_reverse()) {
                    m_flex_items.append(move(item));
                }
            } else {
                for (auto item : items) {
                    m_flex_items.append(move(item));
                }
            }
        }
    }
}

bool FlexFormattingContext::has_definite_main_size(LayoutState::UsedValues const& used_values) const
{
    return is_row_layout() ? used_values.has_definite_width() : used_values.has_definite_height();
}

CSSPixels FlexFormattingContext::inner_main_size(LayoutState::UsedValues const& used_values) const
{
    return is_row_layout() ? used_values.content_width() : used_values.content_height();
}

CSSPixels FlexFormattingContext::inner_cross_size(LayoutState::UsedValues const& used_values) const
{
    return is_row_layout() ? used_values.content_height() : used_values.content_width();
}

bool FlexFormattingContext::has_main_min_size(Box const& box) const
{
    auto const& value = is_row_layout() ? box.computed_values().min_width() : box.computed_values().min_height();
    return !value.is_auto();
}

bool FlexFormattingContext::has_cross_min_size(Box const& box) const
{
    auto const& value = is_row_layout() ? box.computed_values().min_height() : box.computed_values().min_width();
    return !value.is_auto();
}

bool FlexFormattingContext::has_definite_cross_size(LayoutState::UsedValues const& used_values) const
{
    return is_row_layout() ? used_values.has_definite_height() : used_values.has_definite_width();
}

CSSPixels FlexFormattingContext::specified_main_min_size(Box const& box) const
{
    return is_row_layout()
        ? get_pixel_width(box, box.computed_values().min_width())
        : get_pixel_height(box, box.computed_values().min_height());
}

CSSPixels FlexFormattingContext::specified_cross_min_size(Box const& box) const
{
    return is_row_layout()
        ? get_pixel_height(box, box.computed_values().min_height())
        : get_pixel_width(box, box.computed_values().min_width());
}

bool FlexFormattingContext::has_main_max_size(Box const& box) const
{
    return !should_treat_main_max_size_as_none(box);
}

bool FlexFormattingContext::has_cross_max_size(Box const& box) const
{
    return !should_treat_cross_max_size_as_none(box);
}

CSSPixels FlexFormattingContext::specified_main_max_size(Box const& box) const
{
    return is_row_layout()
        ? get_pixel_width(box, box.computed_values().max_width())
        : get_pixel_height(box, box.computed_values().max_height());
}

CSSPixels FlexFormattingContext::specified_cross_max_size(Box const& box) const
{
    return is_row_layout()
        ? get_pixel_height(box, box.computed_values().max_height())
        : get_pixel_width(box, box.computed_values().max_width());
}

bool FlexFormattingContext::is_cross_auto(Box const& box) const
{
    auto& cross_length = is_row_layout() ? box.computed_values().height() : box.computed_values().width();
    return cross_length.is_auto();
}

void FlexFormattingContext::set_has_definite_main_size(FlexItem& item)
{
    if (is_row_layout())
        item.used_values.set_has_definite_width(true);
    else
        item.used_values.set_has_definite_height(true);
}

void FlexFormattingContext::set_has_definite_cross_size(FlexItem& item)
{
    if (is_row_layout())
        item.used_values.set_has_definite_height(true);
    else
        item.used_values.set_has_definite_width(true);
}

void FlexFormattingContext::set_main_size(Box const& box, CSSPixels size)
{
    if (is_row_layout())
        m_state.get_mutable(box).set_content_width(size);
    else
        m_state.get_mutable(box).set_content_height(size);
}

void FlexFormattingContext::set_cross_size(Box const& box, CSSPixels size)
{
    if (is_row_layout())
        m_state.get_mutable(box).set_content_height(size);
    else
        m_state.get_mutable(box).set_content_width(size);
}

void FlexFormattingContext::set_offset(Box const& box, CSSPixels main_offset, CSSPixels cross_offset)
{
    if (is_row_layout())
        m_state.get_mutable(box).offset = CSSPixelPoint { main_offset, cross_offset };
    else
        m_state.get_mutable(box).offset = CSSPixelPoint { cross_offset, main_offset };
}

void FlexFormattingContext::set_main_axis_first_margin(FlexItem& item, CSSPixels margin)
{
    item.margins.main_before = margin;
    if (is_row_layout())
        m_state.get_mutable(item.box).margin_left = margin;
    else
        m_state.get_mutable(item.box).margin_top = margin;
}

void FlexFormattingContext::set_main_axis_second_margin(FlexItem& item, CSSPixels margin)
{
    item.margins.main_after = margin;
    if (is_row_layout())
        m_state.get_mutable(item.box).margin_right = margin;
    else
        m_state.get_mutable(item.box).margin_bottom = margin;
}

// https://drafts.csswg.org/css-flexbox-1/#algo-available
void FlexFormattingContext::determine_available_space_for_items(AvailableSpace const& available_space)
{
    if (is_row_layout()) {
        m_available_space_for_items = AxisAgnosticAvailableSpace {
            .main = available_space.width,
            .cross = available_space.height,
            .space = { available_space.width, available_space.height },
        };
    } else {
        m_available_space_for_items = AxisAgnosticAvailableSpace {
            .main = available_space.height,
            .cross = available_space.width,
            .space = { available_space.width, available_space.height },
        };
    }
}

// https://drafts.csswg.org/css-flexbox-1/#propdef-flex-basis
CSS::FlexBasis FlexFormattingContext::used_flex_basis_for_item(FlexItem const& item) const
{
    auto flex_basis = item.box->computed_values().flex_basis();

    if (flex_basis.has<CSS::Size>() && flex_basis.get<CSS::Size>().is_auto()) {
        // https://drafts.csswg.org/css-flexbox-1/#valdef-flex-basis-auto
        // When specified on a flex item, the auto keyword retrieves the value of the main size property as the used flex-basis.
        // If that value is itself auto, then the used value is content.
        auto const& main_size = is_row_layout() ? item.box->computed_values().width() : item.box->computed_values().height();

        if (main_size.is_auto()) {
            flex_basis = CSS::FlexBasisContent {};
        } else {
            flex_basis = main_size;
        }
    }

    // For example, percentage values of flex-basis are resolved against the flex item’s containing block
    // (i.e. its flex container); and if that containing block’s size is indefinite,
    // the used value for flex-basis is content.
    if (flex_basis.has<CSS::Size>()
        && flex_basis.get<CSS::Size>().is_percentage()
        && !has_definite_main_size(m_flex_container_state)) {
        flex_basis = CSS::FlexBasisContent {};
    }

    return flex_basis;
}

CSSPixels FlexFormattingContext::calculate_main_size_from_cross_size_and_aspect_ratio(CSSPixels cross_size, CSSPixelFraction aspect_ratio) const
{
    if (is_row_layout())
        return cross_size * aspect_ratio;
    return cross_size / aspect_ratio;
}

CSSPixels FlexFormattingContext::calculate_cross_size_from_main_size_and_aspect_ratio(CSSPixels main_size, CSSPixelFraction aspect_ratio) const
{
    if (is_row_layout())
        return main_size / aspect_ratio;
    return main_size * aspect_ratio;
}

// This function takes a size in the main axis and adjusts it according to the aspect ratio of the box
// if the min/max constraints in the cross axis forces us to come up with a new main axis size.
CSSPixels FlexFormattingContext::adjust_main_size_through_aspect_ratio_for_cross_size_min_max_constraints(Box const& box, CSSPixels main_size, CSS::Size const& min_cross_size, CSS::Size const& max_cross_size) const
{
    if (!should_treat_cross_max_size_as_none(box)) {
        auto max_cross_size_px = max_cross_size.to_px(box, !is_row_layout() ? m_flex_container_state.content_width() : m_flex_container_state.content_height());
        main_size = min(main_size, calculate_main_size_from_cross_size_and_aspect_ratio(max_cross_size_px, box.preferred_aspect_ratio().value()));
    }

    if (!min_cross_size.is_auto()) {
        auto min_cross_size_px = min_cross_size.to_px(box, !is_row_layout() ? m_flex_container_state.content_width() : m_flex_container_state.content_height());
        main_size = max(main_size, calculate_main_size_from_cross_size_and_aspect_ratio(min_cross_size_px, box.preferred_aspect_ratio().value()));
    }

    return main_size;
}

// https://www.w3.org/TR/css-flexbox-1/#algo-main-item
void FlexFormattingContext::determine_flex_base_size_and_hypothetical_main_size(FlexItem& item)
{
    auto& child_box = item.box;

    item.flex_base_size = [&] {
        item.used_flex_basis = used_flex_basis_for_item(item);

        item.used_flex_basis_is_definite = [&](CSS::FlexBasis const& flex_basis) -> bool {
            if (!flex_basis.has<CSS::Size>())
                return false;
            auto const& size = flex_basis.get<CSS::Size>();
            if (size.is_auto() || size.is_min_content() || size.is_max_content() || size.is_fit_content())
                return false;
            if (size.is_length())
                return true;

            bool can_resolve_percentages = is_row_layout()
                ? m_flex_container_state.has_definite_width()
                : m_flex_container_state.has_definite_height();

            if (size.is_calculated()) {
                auto const& calc_value = size.calculated();
                if (calc_value.resolves_to_percentage())
                    return can_resolve_percentages;
                if (calc_value.resolves_to_length()) {
                    if (calc_value.contains_percentage())
                        return can_resolve_percentages;
                    return true;
                }
                return false;
            }
            VERIFY(size.is_percentage());
            return can_resolve_percentages;
        }(*item.used_flex_basis);

        // A. If the item has a definite used flex basis, that’s the flex base size.
        if (item.used_flex_basis_is_definite) {
            auto const& size = item.used_flex_basis->get<CSS::Size>();
            if (is_row_layout())
                return get_pixel_width(child_box, size);
            return get_pixel_height(child_box, size);
        }

        // B. If the flex item has ...
        //    - an intrinsic aspect ratio,
        //    - a used flex basis of content, and
        //    - a definite cross size,
        if (item.box->has_preferred_aspect_ratio()
            && item.used_flex_basis->has<CSS::FlexBasisContent>()
            && has_definite_cross_size(item)) {
            // flex_base_size is calculated from definite cross size and intrinsic aspect ratio
            return adjust_main_size_through_aspect_ratio_for_cross_size_min_max_constraints(
                item.box,
                calculate_main_size_from_cross_size_and_aspect_ratio(inner_cross_size(item), item.box->preferred_aspect_ratio().value()),
                computed_cross_min_size(item.box),
                computed_cross_max_size(item.box));
        }

        // C. If the used flex basis is content or depends on its available space,
        //    and the flex container is being sized under a min-content or max-content constraint
        //    (e.g. when performing automatic table layout [CSS21]), size the item under that constraint.
        //    The flex base size is the item’s resulting main size.
        if (item.used_flex_basis->has<CSS::FlexBasisContent>() && m_available_space_for_items->main.is_intrinsic_sizing_constraint()) {
            if (m_available_space_for_items->main.is_min_content())
                return calculate_min_content_main_size(item);
            return calculate_max_content_main_size(item);
        }

        // D. Otherwise, if the used flex basis is content or depends on its available space,
        //    the available main size is infinite, and the flex item’s inline axis is parallel to the main axis,
        //    lay the item out using the rules for a box in an orthogonal flow [CSS3-WRITING-MODES].
        //    The flex base size is the item’s max-content main size.
        if (item.used_flex_basis->has<CSS::FlexBasisContent>()
            // FIXME: && main_size is infinite && inline axis is parallel to the main axis
            && false && false) {
            TODO();
            // Use rules for a flex_container in orthogonal flow
        }

        // E. Otherwise, size the item into the available space using its used flex basis in place of its main size,
        //    treating a value of content as max-content. If a cross size is needed to determine the main size
        //    (e.g. when the flex item’s main size is in its block axis) and the flex item’s cross size is auto and not definite,
        //    in this calculation use fit-content as the flex item’s cross size.
        //    The flex base size is the item’s resulting main size.

        if (auto* size = item.used_flex_basis->get_pointer<CSS::Size>()) {
            if (size->is_fit_content())
                return calculate_fit_content_main_size(item);
            if (size->is_max_content())
                return calculate_max_content_main_size(item);
            if (size->is_min_content())
                return calculate_min_content_main_size(item);
        }

        // NOTE: If the flex item has a definite main size, just use that as the flex base size.
        if (has_definite_main_size(item))
            return inner_main_size(item);

        // NOTE: There's a fundamental problem with many CSS specifications in that they neglect to mention
        //       which width to provide when calculating the intrinsic height of a box in various situations.
        //       Spec bug: https://github.com/w3c/csswg-drafts/issues/2890

        // NOTE: This is one of many situations where that causes trouble: if this is a flex column layout,
        //       we may need to calculate the intrinsic height of a flex item. This requires a width, but a
        //       width won't be determined until later on in the flex layout algorithm.
        //       In the specific case above (E), the spec mentions using `fit-content` in place of `auto`
        //       if "a cross size is needed to determine the main size", so that's exactly what we do.

        // NOTE: Finding a suitable width for intrinsic height determination actually happens elsewhere,
        //       in the various helpers that calculate the intrinsic sizes of a flex item,
        //       e.g. calculate_min_content_main_size().

        if (item.used_flex_basis->has<CSS::FlexBasisContent>()) {
            return calculate_max_content_main_size(item);
        }

        return calculate_fit_content_main_size(item);
    }();

    // AD-HOC: This is not mentioned in the spec, but if the item has an aspect ratio,
    //         we may need to adjust the main size in these ways:
    //         - using stretch-fit main size if the flex basis is indefinite and there is no cross size to resolve the ratio against.
    //         - in response to cross size min/max constraints.
    if (item.box->has_natural_aspect_ratio()) {
        if (!item.used_flex_basis_is_definite && !has_definite_cross_size(item)) {
            item.flex_base_size = inner_main_size(m_flex_container_state);
        }
        item.flex_base_size = adjust_main_size_through_aspect_ratio_for_cross_size_min_max_constraints(child_box, item.flex_base_size, computed_cross_min_size(child_box), computed_cross_max_size(child_box));
    }

    // The hypothetical main size is the item’s flex base size clamped according to its used min and max main sizes (and flooring the content box size at zero).
    auto clamp_min = has_main_min_size(child_box) ? specified_main_min_size(child_box) : automatic_minimum_size(item);
    auto clamp_max = has_main_max_size(child_box) ? specified_main_max_size(child_box) : CSSPixels::max();
    item.hypothetical_main_size = max(CSSPixels(0), css_clamp(item.flex_base_size, clamp_min, clamp_max));

    // NOTE: At this point, we set the hypothetical main size as the flex item's *temporary* main size.
    //       The size may change again when we resolve flexible lengths, but this is necessary in order for
    //       descendants of this flex item to resolve percentage sizes against something.
    //
    //       The spec just barely hand-waves about this, but it seems to *roughly* match what other engines do.
    //       See "Note" section here: https://drafts.csswg.org/css-flexbox-1/#definite-sizes
    if (is_row_layout())
        item.used_values.set_temporary_content_width(item.hypothetical_main_size);
    else
        item.used_values.set_temporary_content_height(item.hypothetical_main_size);
}

// https://drafts.csswg.org/css-flexbox-1/#min-size-auto
CSSPixels FlexFormattingContext::automatic_minimum_size(FlexItem const& item) const
{
    // To provide a more reasonable default minimum size for flex items,
    // the used value of a main axis automatic minimum size on a flex item that is not a scroll container is its content-based minimum size;
    // for scroll containers the automatic minimum size is zero, as usual.
    if (!item.box->is_scroll_container())
        return content_based_minimum_size(item);
    return 0;
}

// https://drafts.csswg.org/css-flexbox-1/#specified-size-suggestion
Optional<CSSPixels> FlexFormattingContext::specified_size_suggestion(FlexItem const& item) const
{
    // If the item’s preferred main size is definite and not automatic,
    // then the specified size suggestion is that size. It is otherwise undefined.
    if (has_definite_main_size(item) && !should_treat_main_size_as_auto(item.box)) {
        // NOTE: We use get_pixel_{width,height} to ensure that CSS box-sizing is respected.
        return is_row_layout() ? get_pixel_width(item.box, computed_main_size(item.box)) : get_pixel_height(item.box, computed_main_size(item.box));
    }
    return {};
}

// https://drafts.csswg.org/css-flexbox-1/#content-size-suggestion
CSSPixels FlexFormattingContext::content_size_suggestion(FlexItem const& item) const
{
    auto suggestion = calculate_min_content_main_size(item);

    if (item.box->has_preferred_aspect_ratio()) {
        suggestion = adjust_main_size_through_aspect_ratio_for_cross_size_min_max_constraints(item.box, suggestion, computed_cross_min_size(item.box), computed_cross_max_size(item.box));
    }

    return suggestion;
}

// https://drafts.csswg.org/css-flexbox-1/#transferred-size-suggestion
Optional<CSSPixels> FlexFormattingContext::transferred_size_suggestion(FlexItem const& item) const
{
    // If the item has a preferred aspect ratio and its preferred cross size is definite,
    // then the transferred size suggestion is that size
    // (clamped by its minimum and maximum cross sizes if they are definite), converted through the aspect ratio.
    if (item.box->has_preferred_aspect_ratio() && has_definite_cross_size(item)) {
        auto aspect_ratio = item.box->preferred_aspect_ratio().value();
        return adjust_main_size_through_aspect_ratio_for_cross_size_min_max_constraints(
            item.box,
            calculate_main_size_from_cross_size_and_aspect_ratio(inner_cross_size(item), aspect_ratio),
            computed_cross_min_size(item.box),
            computed_cross_max_size(item.box));
    }

    // It is otherwise undefined.
    return {};
}

// https://drafts.csswg.org/css-flexbox-1/#content-based-minimum-size
CSSPixels FlexFormattingContext::content_based_minimum_size(FlexItem const& item) const
{
    auto unclamped_size = [&] {
        // The content-based minimum size of a flex item is the smaller of its specified size suggestion
        // and its content size suggestion if its specified size suggestion exists;
        if (auto specified_size_suggestion = this->specified_size_suggestion(item); specified_size_suggestion.has_value()) {
            return min(specified_size_suggestion.value(), content_size_suggestion(item));
        }

        // otherwise, the smaller of its transferred size suggestion and its content size suggestion
        // if the element is replaced and its transferred size suggestion exists;
        if (item.box->is_replaced_box()) {
            if (auto transferred_size_suggestion = this->transferred_size_suggestion(item); transferred_size_suggestion.has_value()) {
                return min(transferred_size_suggestion.value(), content_size_suggestion(item));
            }
        }

        // otherwise its content size suggestion.
        return content_size_suggestion(item);
    }();

    // In all cases, the size is clamped by the maximum main size if it’s definite.
    if (has_main_max_size(item.box)) {
        return min(unclamped_size, specified_main_max_size(item.box));
    }
    return unclamped_size;
}

// https://www.w3.org/TR/css-flexbox-1/#algo-line-break
void FlexFormattingContext::collect_flex_items_into_flex_lines()
{
    // FIXME: Also support wrap-reverse

    // If the flex container is single-line, collect all the flex items into a single flex line.
    if (is_single_line()) {
        FlexLine line;
        for (auto& item : m_flex_items) {
            line.items.append(item);
        }
        m_flex_lines.append(move(line));
        return;
    }

    // Otherwise, starting from the first uncollected item, collect consecutive items one by one
    // until the first time that the next collected item would not fit into the flex container’s inner main size
    // (or until a forced break is encountered, see §10 Fragmenting Flex Layout).
    // If the very first uncollected item wouldn't fit, collect just it into the line.

    // For this step, the size of a flex item is its outer hypothetical main size. (Note: This can be negative.)

    // Repeat until all flex items have been collected into flex lines.

    FlexLine line;
    CSSPixels line_main_size = 0;
    for (auto& item : m_flex_items) {
        auto const outer_hypothetical_main_size = item.outer_hypothetical_main_size();
        if (!line.items.is_empty() && (line_main_size + outer_hypothetical_main_size) > m_available_space_for_items->main) {
            m_flex_lines.append(move(line));
            line = {};
            line_main_size = 0;
        }
        line.items.append(item);
        line_main_size += outer_hypothetical_main_size;
        // CSS-FLEXBOX-2: Account for gap between flex items.
        line_main_size += main_gap();
    }
    m_flex_lines.append(move(line));
}

// https://drafts.csswg.org/css-flexbox-1/#resolve-flexible-lengths
void FlexFormattingContext::resolve_flexible_lengths_for_line(FlexLine& line)
{
    // AD-HOC: The spec tells us to use the "flex container’s inner main size" in this algorithm,
    //         but that doesn't work when we're sizing under a max-content constraint.
    //         In that case, there is effectively infinite size available in the main axis,
    //         but the inner main size has not been assigned yet.
    //         We solve this by calculating our own "available main size" here, which is essentially
    //         infinity under max-content, 0 under min-content, and the inner main size otherwise.
    AvailableSize available_main_size { AvailableSize::make_indefinite() };
    if (m_available_space_for_items->main.is_intrinsic_sizing_constraint())
        available_main_size = m_available_space_for_items->main;
    else
        available_main_size = AvailableSize::make_definite(inner_main_size(m_flex_container_state));

    // 1. Determine the used flex factor.

    // Sum the outer hypothetical main sizes of all items on the line.
    // If the sum is less than the flex container’s inner main size,
    // use the flex grow factor for the rest of this algorithm; otherwise, use the flex shrink factor
    enum FlexFactor {
        FlexGrowFactor,
        FlexShrinkFactor
    };
    auto used_flex_factor = [&]() -> FlexFactor {
        CSSPixels sum = 0;
        for (auto const& item : line.items) {
            sum += item.outer_hypothetical_main_size();
        }
        // CSS-FLEXBOX-2: Account for gap between flex items.
        sum += main_gap() * (line.items.size() - 1);
        // AD-HOC: Note that we're using our own "available main size" explained above
        //         instead of the flex container’s inner main size.
        if (sum < available_main_size)
            return FlexFactor::FlexGrowFactor;
        return FlexFactor::FlexShrinkFactor;
    }();

    // 2. Each item in the flex line has a target main size, initially set to its flex base size.
    //    Each item is initially unfrozen and may become frozen.
    for (auto& item : line.items) {
        item.target_main_size = item.flex_base_size;
        item.frozen = false;
    }

    // 3. Size inflexible items.

    for (FlexItem& item : line.items) {
        if (used_flex_factor == FlexFactor::FlexGrowFactor) {
            item.flex_factor = item.box->computed_values().flex_grow();
        } else if (used_flex_factor == FlexFactor::FlexShrinkFactor) {
            item.flex_factor = item.box->computed_values().flex_shrink();
        }
        // Freeze, setting its target main size to its hypothetical main size…
        // - any item that has a flex factor of zero
        // - if using the flex grow factor: any item that has a flex base size greater than its hypothetical main size
        // - if using the flex shrink factor: any item that has a flex base size smaller than its hypothetical main size
        if (item.flex_factor.value() == 0
            || (used_flex_factor == FlexFactor::FlexGrowFactor && item.flex_base_size > item.hypothetical_main_size)
            || (used_flex_factor == FlexFactor::FlexShrinkFactor && item.flex_base_size < item.hypothetical_main_size)) {
            item.frozen = true;
            item.target_main_size = item.hypothetical_main_size;
        }
    }

    // 4. Calculate initial free space

    // Sum the outer sizes of all items on the line, and subtract this from the flex container’s inner main size.
    // For frozen items, use their outer target main size; for other items, use their outer flex base size.
    auto calculate_remaining_free_space = [&]() -> Optional<CSSPixels> {
        // AD-HOC: If the container is sized under max-content constraints, then remaining_free_space won't have
        //         a value to avoid leaking an infinite value into layout calculations.
        if (available_main_size.is_intrinsic_sizing_constraint())
            return {};
        CSSPixels sum = 0;
        for (auto const& item : line.items) {
            if (item.frozen)
                sum += item.outer_target_main_size();
            else
                sum += item.outer_flex_base_size();
        }
        // CSS-FLEXBOX-2: Account for gap between flex items.
        sum += main_gap() * (line.items.size() - 1);

        // AD-HOC: Note that we're using our own "available main size" explained above
        //         instead of the flex container’s inner main size.
        return available_main_size.to_px_or_zero() - sum;
    };
    auto const initial_free_space = calculate_remaining_free_space();

    // 5. Loop
    while (true) {
        // a. Check for flexible items.
        //    If all the flex items on the line are frozen, free space has been distributed; exit this loop.
        if (all_of(line.items, [](auto const& item) { return item.frozen; })) {
            break;
        }

        // b. Calculate the remaining free space as for initial free space, above.
        line.remaining_free_space = calculate_remaining_free_space();

        // If the sum of the unfrozen flex items’ flex factors is less than one, multiply the initial free space by this sum.
        if (auto sum_of_flex_factor_of_unfrozen_items = line.sum_of_flex_factor_of_unfrozen_items(); sum_of_flex_factor_of_unfrozen_items < 1 && initial_free_space.has_value()) {
            auto value = CSSPixels::nearest_value_for(initial_free_space.value() * sum_of_flex_factor_of_unfrozen_items);
            // If the magnitude of this value is less than the magnitude of the remaining free space, use this as the remaining free space.
            if (abs(value) < abs(line.remaining_free_space.value()))
                line.remaining_free_space = value;
        }

        // AD-HOC: We allow the remaining free space to be infinite, but we can't let infinity
        //         leak into the layout geometry, so we treat infinity as zero when used in arithmetic.
        auto remaining_free_space_or_zero_if_infinite = line.remaining_free_space.has_value() ? line.remaining_free_space.value() : 0;

        // c. If the remaining free space is non-zero, distribute it proportional to the flex factors:
        if (line.remaining_free_space != 0) {
            // If using the flex grow factor
            if (used_flex_factor == FlexFactor::FlexGrowFactor) {
                // For every unfrozen item on the line,
                // find the ratio of the item’s flex grow factor to the sum of the flex grow factors of all unfrozen items on the line.
                auto sum_of_flex_factor_of_unfrozen_items = line.sum_of_flex_factor_of_unfrozen_items();
                for (auto& item : line.items) {
                    if (item.frozen)
                        continue;
                    double ratio = item.flex_factor.value() / sum_of_flex_factor_of_unfrozen_items;
                    // Set the item’s target main size to its flex base size plus a fraction of the remaining free space proportional to the ratio.
                    item.target_main_size = item.flex_base_size + remaining_free_space_or_zero_if_infinite.scaled(ratio);
                }
            }
            // If using the flex shrink factor
            else if (used_flex_factor == FlexFactor::FlexShrinkFactor) {
                // For every unfrozen item on the line, multiply its flex shrink factor by its inner flex base size, and note this as its scaled flex shrink factor.
                for (auto& item : line.items) {
                    if (item.frozen)
                        continue;
                    item.scaled_flex_shrink_factor = item.flex_factor.value() * item.flex_base_size.to_double();
                }
                auto sum_of_scaled_flex_shrink_factors_of_all_unfrozen_items_on_line = line.sum_of_scaled_flex_shrink_factor_of_unfrozen_items();
                for (auto& item : line.items) {
                    if (item.frozen)
                        continue;
                    // Find the ratio of the item’s scaled flex shrink factor to the sum of the scaled flex shrink factors of all unfrozen items on the line.
                    double ratio = 1.0;
                    if (sum_of_scaled_flex_shrink_factors_of_all_unfrozen_items_on_line != 0)
                        ratio = item.scaled_flex_shrink_factor / sum_of_scaled_flex_shrink_factors_of_all_unfrozen_items_on_line;

                    // Set the item’s target main size to its flex base size minus a fraction of the absolute value of the remaining free space proportional to the ratio.
                    // (Note this may result in a negative inner main size; it will be corrected in the next step.)
                    item.target_main_size = item.flex_base_size - abs(remaining_free_space_or_zero_if_infinite).scaled(ratio);
                }
            }
        }

        // d. Fix min/max violations.
        CSSPixels total_violation = 0;

        // Clamp each non-frozen item’s target main size by its used min and max main sizes and floor its content-box size at zero.
        for (auto& item : line.items) {
            if (item.frozen)
                continue;
            auto used_min_main_size = has_main_min_size(item.box)
                ? specified_main_min_size(item.box)
                : automatic_minimum_size(item);

            auto used_max_main_size = has_main_max_size(item.box)
                ? specified_main_max_size(item.box)
                : CSSPixels::max();

            auto original_target_main_size = item.target_main_size;
            item.target_main_size = css_clamp(item.target_main_size, used_min_main_size, used_max_main_size);
            item.target_main_size = max(item.target_main_size, CSSPixels(0));

            // If the item’s target main size was made smaller by this, it’s a max violation.
            if (item.target_main_size < original_target_main_size)
                item.is_max_violation = true;

            // If the item’s target main size was made larger by this, it’s a min violation.
            if (item.target_main_size > original_target_main_size)
                item.is_min_violation = true;

            total_violation += item.target_main_size - original_target_main_size;
        }

        // e. Freeze over-flexed items.
        //    The total violation is the sum of the adjustments from the previous step ∑(clamped size - unclamped size).

        // If the total violation is:
        // Zero
        //   Freeze all items.
        if (total_violation == 0) {
            for (auto& item : line.items) {
                if (!item.frozen)
                    item.frozen = true;
            }
        }
        // Positive
        //   Freeze all the items with min violations.
        else if (total_violation > 0) {
            for (auto& item : line.items) {
                if (!item.frozen && item.is_min_violation)
                    item.frozen = true;
            }
        }
        // Negative
        //   Freeze all the items with max violations.
        else {
            for (auto& item : line.items) {
                if (!item.frozen && item.is_max_violation)
                    item.frozen = true;
            }
        }
        // NOTE: This freezes at least one item, ensuring that the loop makes progress and eventually terminates.

        // f. Return to the start of this loop.
    }

    // NOTE: Calculate the remaining free space once again here, since it's needed later when aligning items.
    line.remaining_free_space = calculate_remaining_free_space();

    // 6. Set each item’s used main size to its target main size.
    for (auto& item : line.items) {
        item.main_size = item.target_main_size;
        set_main_size(item.box, item.target_main_size);

        // https://drafts.csswg.org/css-flexbox-1/#definite-sizes
        // 1. If the flex container has a definite main size, then the post-flexing main sizes of its flex items are treated as definite.
        // 2. If a flex item’s flex basis is definite, then its post-flexing main size is also definite.
        if (has_definite_main_size(m_flex_container_state) || item.used_flex_basis_is_definite)
            set_has_definite_main_size(item);
    }
}

// https://drafts.csswg.org/css-flexbox-1/#resolve-flexible-lengths
void FlexFormattingContext::resolve_flexible_lengths()
{
    for (auto& line : m_flex_lines) {
        resolve_flexible_lengths_for_line(line);
    }
}

// https://drafts.csswg.org/css-flexbox-1/#algo-cross-item
void FlexFormattingContext::determine_hypothetical_cross_size_of_item(FlexItem& item, bool resolve_percentage_min_max_sizes)
{
    // Determine the hypothetical cross size of each item by performing layout
    // as if it were an in-flow block-level box with the used main size
    // and the given available space, treating auto as fit-content.

    auto const& computed_min_size = this->computed_cross_min_size(item.box);
    auto const& computed_max_size = this->computed_cross_max_size(item.box);

    auto clamp_min = (!computed_min_size.is_auto() && (resolve_percentage_min_max_sizes || !computed_min_size.contains_percentage())) ? specified_cross_min_size(item.box) : 0;
    auto clamp_max = (!should_treat_cross_max_size_as_none(item.box) && (resolve_percentage_min_max_sizes || !computed_max_size.contains_percentage())) ? specified_cross_max_size(item.box) : CSSPixels::max();

    // If we have a definite cross size, this is easy! No need to perform layout, we can just use it as-is.
    if (has_definite_cross_size(item)) {
        // To avoid subtracting padding and border twice for `box-sizing: border-box` only min and max clamp should happen on a second pass
        if (resolve_percentage_min_max_sizes) {
            item.hypothetical_cross_size = css_clamp(item.hypothetical_cross_size, clamp_min, clamp_max);
            return;
        }

        item.hypothetical_cross_size = css_clamp(inner_cross_size(item), clamp_min, clamp_max);
        return;
    }

    if (item.box->has_preferred_aspect_ratio()) {
        if (item.used_flex_basis_is_definite) {
            item.hypothetical_cross_size = calculate_cross_size_from_main_size_and_aspect_ratio(item.main_size.value(), item.box->preferred_aspect_ratio().value());
            return;
        }
        item.hypothetical_cross_size = inner_cross_size(m_flex_container_state);
        return;
    }

    auto computed_cross_size = [&]() -> CSS::Size {
        // "... treating auto as fit-content"
        if (should_treat_cross_size_as_auto(item.box))
            return CSS::Size::make_fit_content();
        return this->computed_cross_size(item.box);
    }();

    if (computed_cross_size.is_min_content()) {
        item.hypothetical_cross_size = css_clamp(calculate_min_content_cross_size(item), clamp_min, clamp_max);
        return;
    }

    if (computed_cross_size.is_max_content()) {
        item.hypothetical_cross_size = css_clamp(calculate_max_content_cross_size(item), clamp_min, clamp_max);
        return;
    }

    if (computed_cross_size.is_fit_content()) {
        CSSPixels fit_content_cross_size = 0;
        if (is_row_layout()) {
            auto available_width = item.main_size.has_value() ? AvailableSize::make_definite(item.main_size.value()) : AvailableSize::make_indefinite();
            auto available_height = AvailableSize::make_indefinite();
            fit_content_cross_size = calculate_fit_content_height(item.box, AvailableSpace(available_width, available_height));
        } else {
            fit_content_cross_size = calculate_fit_content_width(item.box, m_available_space_for_items->space);
        }

        item.hypothetical_cross_size = css_clamp(fit_content_cross_size, clamp_min, clamp_max);
        return;
    }

    // For indefinite cross sizes, we perform a throwaway layout and then measure it.
    LayoutState throwaway_state(&m_state);

    auto& box_state = throwaway_state.get_mutable(item.box);
    if (is_row_layout()) {
        box_state.set_content_width(item.main_size.value());
    } else {
        box_state.set_content_height(item.main_size.value());
    }

    // Item has definite main size, layout with that as the used main size.
    auto independent_formatting_context = create_independent_formatting_context_if_needed(throwaway_state, item.box);
    // NOTE: Flex items should always create an independent formatting context!
    VERIFY(independent_formatting_context);

    auto available_width = is_row_layout() ? AvailableSize::make_definite(item.main_size.value()) : AvailableSize::make_indefinite();
    auto available_height = is_row_layout() ? AvailableSize::make_indefinite() : AvailableSize::make_definite(item.main_size.value());

    independent_formatting_context->run(item.box, LayoutMode::Normal, AvailableSpace(available_width, available_height));

    auto automatic_cross_size = is_row_layout() ? independent_formatting_context->automatic_content_height()
                                                : independent_formatting_context->automatic_content_width();

    item.hypothetical_cross_size = css_clamp(automatic_cross_size, clamp_min, clamp_max);
}

// https://www.w3.org/TR/css-flexbox-1/#algo-cross-line
void FlexFormattingContext::calculate_cross_size_of_each_flex_line()
{
    // If the flex container is single-line and has a definite cross size, the cross size of the flex line is the flex container’s inner cross size.
    if (is_single_line() && has_definite_cross_size(m_flex_container_state)) {
        m_flex_lines[0].cross_size = inner_cross_size(m_flex_container_state);
        return;
    }

    // Otherwise, for each flex line:
    for (auto& flex_line : m_flex_lines) {
        // FIXME: 1. Collect all the flex items whose inline-axis is parallel to the main-axis, whose align-self is baseline,
        //           and whose cross-axis margins are both non-auto. Find the largest of the distances between each item’s baseline
        //           and its hypothetical outer cross-start edge, and the largest of the distances between each item’s baseline
        //           and its hypothetical outer cross-end edge, and sum these two values.

        // 2. Among all the items not collected by the previous step, find the largest outer hypothetical cross size.
        CSSPixels largest_hypothetical_cross_size = 0;
        for (auto& item : flex_line.items) {
            if (largest_hypothetical_cross_size < item.hypothetical_cross_size_with_margins())
                largest_hypothetical_cross_size = item.hypothetical_cross_size_with_margins();
        }

        // 3. The used cross-size of the flex line is the largest of the numbers found in the previous two steps and zero.
        flex_line.cross_size = max(CSSPixels(0), largest_hypothetical_cross_size);
    }

    // If the flex container is single-line, then clamp the line’s cross-size to be within the container’s computed min and max cross sizes.
    // Note that if CSS 2.1’s definition of min/max-width/height applied more generally, this behavior would fall out automatically.
    // AD-HOC: We don't do this when the flex container is being sized under a min-content or max-content constraint.
    if (is_single_line() && !m_available_space_for_items->cross.is_intrinsic_sizing_constraint()) {
        auto const& computed_min_size = this->computed_cross_min_size(flex_container());
        auto const& computed_max_size = this->computed_cross_max_size(flex_container());
        auto cross_min_size = (!computed_min_size.is_auto() && !computed_min_size.contains_percentage()) ? specified_cross_min_size(flex_container()) : 0;
        auto cross_max_size = (!computed_max_size.is_none() && !computed_max_size.contains_percentage()) ? specified_cross_max_size(flex_container()) : CSSPixels::max();
        m_flex_lines[0].cross_size = css_clamp(m_flex_lines[0].cross_size, cross_min_size, cross_max_size);
    }
}

// https://www.w3.org/TR/css-flexbox-1/#algo-stretch
void FlexFormattingContext::determine_used_cross_size_of_each_flex_item()
{
    for (auto& flex_line : m_flex_lines) {
        for (auto& item : flex_line.items) {
            //  If a flex item has align-self: stretch, its computed cross size property is auto,
            //  and neither of its cross-axis margins are auto, the used outer cross size is the used cross size of its flex line,
            //  clamped according to the item’s used min and max cross sizes.
            auto flex_item_alignment = alignment_for_item(item.box);
            if ((flex_item_alignment == CSS::AlignItems::Stretch || flex_item_alignment == CSS::AlignItems::Normal)
                && is_cross_auto(item.box)
                && !item.margins.cross_before_is_auto
                && !item.margins.cross_after_is_auto) {
                auto unclamped_cross_size = flex_line.cross_size
                    - item.margins.cross_before - item.margins.cross_after
                    - item.padding.cross_before - item.padding.cross_after
                    - item.borders.cross_before - item.borders.cross_after;

                auto const& computed_min_size = computed_cross_min_size(item.box);
                auto const& computed_max_size = computed_cross_max_size(item.box);
                auto cross_min_size = (!computed_min_size.is_auto() && !computed_min_size.contains_percentage()) ? specified_cross_min_size(item.box) : 0;
                auto cross_max_size = (!should_treat_cross_max_size_as_none(item.box) && !computed_max_size.contains_percentage()) ? specified_cross_max_size(item.box) : CSSPixels::max();

                item.cross_size = css_clamp(unclamped_cross_size, cross_min_size, cross_max_size);
            } else {
                // Otherwise, the used cross size is the item’s hypothetical cross size.
                item.cross_size = item.hypothetical_cross_size;
            }
        }
    }
}

// https://www.w3.org/TR/css-flexbox-1/#algo-main-align
void FlexFormattingContext::distribute_any_remaining_free_space()
{
    for (auto& flex_line : m_flex_lines) {
        // 12.1.
        CSSPixels used_main_space = 0;
        size_t auto_margins = 0;
        for (auto& item : flex_line.items) {
            used_main_space += item.main_size.value();
            if (item.margins.main_before_is_auto)
                ++auto_margins;

            if (item.margins.main_after_is_auto)
                ++auto_margins;

            used_main_space += item.margins.main_before + item.margins.main_after
                + item.borders.main_before + item.borders.main_after
                + item.padding.main_before + item.padding.main_after;
        }

        // CSS-FLEXBOX-2: Account for gap between flex items.
        used_main_space += main_gap() * (flex_line.items.size() - 1);

        if (flex_line.remaining_free_space.has_value() && flex_line.remaining_free_space.value() > 0 && auto_margins > 0) {
            CSSPixels size_per_auto_margin = flex_line.remaining_free_space.value() / auto_margins;
            for (auto& item : flex_line.items) {
                if (item.margins.main_before_is_auto)
                    set_main_axis_first_margin(item, size_per_auto_margin);
                if (item.margins.main_after_is_auto)
                    set_main_axis_second_margin(item, size_per_auto_margin);
            }
        } else {
            for (auto& item : flex_line.items) {
                if (item.margins.main_before_is_auto)
                    set_main_axis_first_margin(item, 0);
                if (item.margins.main_after_is_auto)
                    set_main_axis_second_margin(item, 0);
            }
        }

        // 12.2.
        // CSS-FLEXBOX-2: Account for gap between items.
        CSSPixels space_between_items = main_gap();
        CSSPixels initial_offset = 0;
        auto number_of_items = flex_line.items.size();

        if (auto_margins == 0 && number_of_items > 0) {
            switch (flex_container().computed_values().justify_content()) {
            case CSS::JustifyContent::Start:
                initial_offset = 0;
                break;
            case CSS::JustifyContent::Stretch:
            case CSS::JustifyContent::Normal:
            case CSS::JustifyContent::FlexStart:
                if (is_direction_reverse()) {
                    initial_offset = inner_main_size(m_flex_container_state);
                } else {
                    initial_offset = 0;
                }
                break;
            case CSS::JustifyContent::End:
                initial_offset = inner_main_size(m_flex_container_state);
                break;
            case CSS::JustifyContent::FlexEnd:
                if (is_direction_reverse()) {
                    initial_offset = 0;
                } else {
                    initial_offset = inner_main_size(m_flex_container_state);
                }
                break;
            case CSS::JustifyContent::Center:
                initial_offset = (inner_main_size(m_flex_container_state) - used_main_space) / 2;
                if (is_direction_reverse()) {
                    initial_offset = inner_main_size(m_flex_container_state) - initial_offset;
                }
                break;
            case CSS::JustifyContent::SpaceBetween:
                if (is_direction_reverse()) {
                    initial_offset = inner_main_size(m_flex_container_state);
                } else {
                    initial_offset = 0;
                }
                if (flex_line.remaining_free_space.has_value() && number_of_items > 1)
                    space_between_items = max(CSSPixels(0), flex_line.remaining_free_space.value() / (number_of_items - 1));
                break;
            case CSS::JustifyContent::SpaceAround:
                if (flex_line.remaining_free_space.has_value())
                    space_between_items = max(CSSPixels(0), flex_line.remaining_free_space.value() / number_of_items);
                if (is_direction_reverse()) {
                    initial_offset = inner_main_size(m_flex_container_state) - space_between_items / 2;
                } else {
                    initial_offset = space_between_items / 2;
                }
                break;
            case CSS::JustifyContent::SpaceEvenly:
                if (flex_line.remaining_free_space.has_value())
                    space_between_items = max(CSSPixels(0), flex_line.remaining_free_space.value() / (number_of_items + 1));
                if (is_direction_reverse()) {
                    initial_offset = inner_main_size(m_flex_container_state) - space_between_items;
                } else {
                    initial_offset = space_between_items;
                }
                break;
            }
        }

        // For reverse, we use FlexRegionRenderCursor::Right
        // to indicate the cursor offset is the end and render backwards
        // Otherwise the cursor offset is the 'start' of the region or initial offset
        enum class FlexRegionRenderCursor {
            Left,
            Right
        };
        auto flex_region_render_cursor = FlexRegionRenderCursor::Left;

        if (auto_margins == 0) {
            switch (flex_container().computed_values().justify_content()) {
            case CSS::JustifyContent::Normal:
            case CSS::JustifyContent::FlexStart:
            case CSS::JustifyContent::Center:
            case CSS::JustifyContent::SpaceAround:
            case CSS::JustifyContent::SpaceBetween:
            case CSS::JustifyContent::SpaceEvenly:
            case CSS::JustifyContent::Stretch:
                if (is_direction_reverse()) {
                    flex_region_render_cursor = FlexRegionRenderCursor::Right;
                }
                break;
            case CSS::JustifyContent::End:
                flex_region_render_cursor = FlexRegionRenderCursor::Right;
                break;
            case CSS::JustifyContent::FlexEnd:
                if (!is_direction_reverse()) {
                    flex_region_render_cursor = FlexRegionRenderCursor::Right;
                }
                break;
            default:
                break;
            }
        }

        CSSPixels cursor_offset = initial_offset;

        auto place_item = [&](FlexItem& item) {
            auto amount_of_main_size_used = item.main_size.value()
                + item.margins.main_before
                + item.borders.main_before
                + item.padding.main_before
                + item.margins.main_after
                + item.borders.main_after
                + item.padding.main_after
                + space_between_items;

            if (is_direction_reverse() && flex_region_render_cursor == FlexRegionRenderCursor::Right) {
                item.main_offset = cursor_offset - item.main_size.value() - item.margins.main_after - item.borders.main_after - item.padding.main_after;
                cursor_offset -= amount_of_main_size_used;
            } else if (flex_region_render_cursor == FlexRegionRenderCursor::Right) {
                cursor_offset -= amount_of_main_size_used;
                item.main_offset = cursor_offset + item.margins.main_before + item.borders.main_before + item.padding.main_before;
            } else {
                item.main_offset = cursor_offset + item.margins.main_before + item.borders.main_before + item.padding.main_before;
                cursor_offset += amount_of_main_size_used;
            }
        };

        if (flex_region_render_cursor == FlexRegionRenderCursor::Right) {
            for (ssize_t i = flex_line.items.size() - 1; i >= 0; --i) {
                auto& item = flex_line.items[i];
                place_item(item);
            }
        } else {
            for (size_t i = 0; i < flex_line.items.size(); ++i) {
                auto& item = flex_line.items[i];
                place_item(item);
            }
        }
    }
}

void FlexFormattingContext::dump_items() const
{
    dbgln("\033[34;1mflex-container\033[0m {}, direction: {}, current-size: {}x{}", flex_container().debug_description(), is_row_layout() ? "row" : "column", m_flex_container_state.content_width(), m_flex_container_state.content_height());
    for (size_t i = 0; i < m_flex_lines.size(); ++i) {
        dbgln("{} flex-line #{}:", flex_container().debug_description(), i);
        for (size_t j = 0; j < m_flex_lines[i].items.size(); ++j) {
            auto& item = m_flex_lines[i].items[j];
            dbgln("{}   flex-item #{}: {} (main:{}, cross:{})", flex_container().debug_description(), j, item.box->debug_description(), item.main_size.value_or(-1), item.cross_size.value_or(-1));
        }
    }
}

CSS::AlignItems FlexFormattingContext::alignment_for_item(Box const& box) const
{
    switch (box.computed_values().align_self()) {
    case CSS::AlignSelf::Auto:
        return flex_container().computed_values().align_items();
    case CSS::AlignSelf::End:
        return CSS::AlignItems::End;
    case CSS::AlignSelf::Normal:
        return CSS::AlignItems::Normal;
    case CSS::AlignSelf::SelfStart:
        return CSS::AlignItems::SelfStart;
    case CSS::AlignSelf::SelfEnd:
        return CSS::AlignItems::SelfEnd;
    case CSS::AlignSelf::FlexStart:
        return CSS::AlignItems::FlexStart;
    case CSS::AlignSelf::FlexEnd:
        return CSS::AlignItems::FlexEnd;
    case CSS::AlignSelf::Center:
        return CSS::AlignItems::Center;
    case CSS::AlignSelf::Baseline:
        return CSS::AlignItems::Baseline;
    case CSS::AlignSelf::Start:
        return CSS::AlignItems::Start;
    case CSS::AlignSelf::Stretch:
        return CSS::AlignItems::Stretch;
    case CSS::AlignSelf::Safe:
        return CSS::AlignItems::Safe;
    case CSS::AlignSelf::Unsafe:
        return CSS::AlignItems::Unsafe;
    default:
        VERIFY_NOT_REACHED();
    }
}

void FlexFormattingContext::align_all_flex_items_along_the_cross_axis()
{
    // FIXME: Take better care of margins
    for (auto& flex_line : m_flex_lines) {
        for (auto& item : flex_line.items) {
            CSSPixels half_line_size = flex_line.cross_size / 2;
            switch (alignment_for_item(item.box)) {
            case CSS::AlignItems::Baseline:
                // FIXME: Implement this
                //  Fallthrough
            case CSS::AlignItems::Start:
            case CSS::AlignItems::FlexStart:
            case CSS::AlignItems::Stretch:
            case CSS::AlignItems::Normal:
                item.cross_offset = -half_line_size + item.margins.cross_before + item.borders.cross_before + item.padding.cross_before;
                break;
            case CSS::AlignItems::End:
            case CSS::AlignItems::FlexEnd:
                item.cross_offset = half_line_size - item.cross_size.value() - item.margins.cross_after - item.borders.cross_after - item.padding.cross_after;
                break;
            case CSS::AlignItems::Center:
                item.cross_offset = -(item.cross_size.value() / 2);
                break;
            default:
                break;
            }
        }
    }
}

// https://www.w3.org/TR/css-flexbox-1/#algo-line-align
void FlexFormattingContext::align_all_flex_lines()
{
    if (m_flex_lines.is_empty())
        return;

    // FIXME: Support reverse

    CSSPixels cross_size_of_flex_container = inner_cross_size(m_flex_container_state);

    if (is_single_line()) {
        // For single-line flex containers, we only need to center the line along the cross axis.
        auto& flex_line = m_flex_lines[0];
        CSSPixels center_of_line = cross_size_of_flex_container / 2;
        for (auto& item : flex_line.items) {
            item.cross_offset += center_of_line;
        }
    } else {

        CSSPixels sum_of_flex_line_cross_sizes = 0;
        for (auto& line : m_flex_lines)
            sum_of_flex_line_cross_sizes += line.cross_size;

        // CSS-FLEXBOX-2: Account for gap between flex lines.
        sum_of_flex_line_cross_sizes += cross_gap() * (m_flex_lines.size() - 1);

        CSSPixels start_of_current_line = 0;
        CSSPixels gap_size = 0;
        switch (flex_container().computed_values().align_content()) {
        case CSS::AlignContent::FlexStart:
            start_of_current_line = 0;
            break;
        case CSS::AlignContent::FlexEnd:
            start_of_current_line = cross_size_of_flex_container - sum_of_flex_line_cross_sizes;
            break;
        case CSS::AlignContent::Center:
            start_of_current_line = (cross_size_of_flex_container / 2) - (sum_of_flex_line_cross_sizes / 2);
            break;
        case CSS::AlignContent::SpaceBetween: {
            start_of_current_line = 0;

            auto leftover_free_space = cross_size_of_flex_container - sum_of_flex_line_cross_sizes;
            auto leftover_flex_lines_size = m_flex_lines.size();
            if (leftover_free_space >= 0 && leftover_flex_lines_size > 1) {
                int gap_count = leftover_flex_lines_size - 1;
                gap_size = leftover_free_space / gap_count;
            }
            break;
        }
        case CSS::AlignContent::SpaceAround: {
            auto leftover_free_space = cross_size_of_flex_container - sum_of_flex_line_cross_sizes;
            if (leftover_free_space < 0) {
                // If the leftover free-space is negative this value is identical to center.
                start_of_current_line = (cross_size_of_flex_container / 2) - (sum_of_flex_line_cross_sizes / 2);
                break;
            }

            gap_size = leftover_free_space / m_flex_lines.size();

            // The spacing between the first/last lines and the flex container edges is half the size of the spacing between flex lines.
            start_of_current_line = gap_size / 2;
            break;
        }
        case CSS::AlignContent::SpaceEvenly: {
            auto leftover_free_space = cross_size_of_flex_container - sum_of_flex_line_cross_sizes;
            if (leftover_free_space < 0) {
                // If the leftover free-space is negative this value is identical to center.
                start_of_current_line = (cross_size_of_flex_container / 2) - (sum_of_flex_line_cross_sizes / 2);
                break;
            }

            gap_size = leftover_free_space / (m_flex_lines.size() + 1);

            // The spacing between the first/last lines and the flex container edges is the size of the spacing between flex lines.
            start_of_current_line = gap_size;
            break;
        }

        case CSS::AlignContent::Normal:
        case CSS::AlignContent::Stretch:
            start_of_current_line = 0;
            break;
        }

        for (auto& flex_line : m_flex_lines) {
            CSSPixels center_of_current_line = start_of_current_line + (flex_line.cross_size / 2);
            for (auto& item : flex_line.items) {
                item.cross_offset += center_of_current_line;
            }
            start_of_current_line += flex_line.cross_size + gap_size;
            // CSS-FLEXBOX-2: Account for gap between flex lines.
            start_of_current_line += cross_gap();
        }
    }
}

void FlexFormattingContext::copy_dimensions_from_flex_items_to_boxes()
{
    for (auto& item : m_flex_items) {
        auto const& box = item.box;

        item.used_values.padding_left = box->computed_values().padding().left().to_px(box, m_flex_container_state.content_width());
        item.used_values.padding_right = box->computed_values().padding().right().to_px(box, m_flex_container_state.content_width());
        item.used_values.padding_top = box->computed_values().padding().top().to_px(box, m_flex_container_state.content_width());
        item.used_values.padding_bottom = box->computed_values().padding().bottom().to_px(box, m_flex_container_state.content_width());

        item.used_values.margin_left = box->computed_values().margin().left().to_px(box, m_flex_container_state.content_width());
        item.used_values.margin_right = box->computed_values().margin().right().to_px(box, m_flex_container_state.content_width());
        item.used_values.margin_top = box->computed_values().margin().top().to_px(box, m_flex_container_state.content_width());
        item.used_values.margin_bottom = box->computed_values().margin().bottom().to_px(box, m_flex_container_state.content_width());

        item.used_values.border_left = box->computed_values().border_left().width;
        item.used_values.border_right = box->computed_values().border_right().width;
        item.used_values.border_top = box->computed_values().border_top().width;
        item.used_values.border_bottom = box->computed_values().border_bottom().width;

        set_main_size(box, item.main_size.value());
        set_cross_size(box, item.cross_size.value());
        set_offset(box, item.main_offset, item.cross_offset);
    }
}

// https://drafts.csswg.org/css-flexbox-1/#intrinsic-sizes
void FlexFormattingContext::determine_intrinsic_size_of_flex_container()
{
    if (m_available_space_for_items->main.is_intrinsic_sizing_constraint()) {
        CSSPixels main_size = calculate_intrinsic_main_size_of_flex_container();
        set_main_size(flex_container(), main_size);
    }
    if (m_available_space_for_items->cross.is_intrinsic_sizing_constraint()) {
        CSSPixels cross_size = calculate_intrinsic_cross_size_of_flex_container();
        set_cross_size(flex_container(), cross_size);
    }
}

// https://drafts.csswg.org/css-flexbox-1/#intrinsic-main-sizes
CSSPixels FlexFormattingContext::calculate_intrinsic_main_size_of_flex_container()
{
    // The min-content main size of a single-line flex container is calculated identically to the max-content main size,
    // except that the flex items’ min-content contributions are used instead of their max-content contributions.
    // However, for a multi-line container, it is simply the largest min-content contribution of all the non-collapsed flex items in the flex container.
    if (!is_single_line() && m_available_space_for_items->main.is_min_content()) {
        CSSPixels largest_contribution = 0;
        for (auto const& item : m_flex_items) {
            // FIXME: Skip collapsed flex items.
            largest_contribution = max(largest_contribution, calculate_main_min_content_contribution(item));
        }
        return largest_contribution;
    }

    // The max-content main size of a flex container is, fundamentally, the smallest size the flex container
    // can take such that when flex layout is run with that container size, each flex item ends up at least
    // as large as its max-content contribution, to the extent allowed by the items’ flexibility.
    // It is calculated, considering only non-collapsed flex items, by:

    // 1. For each flex item, subtract its outer flex base size from its max-content contribution size.
    //    If that result is positive, divide it by the item’s flex grow factor if the flex grow factor is ≥ 1,
    //    or multiply it by the flex grow factor if the flex grow factor is < 1; if the result is negative,
    //    divide it by the item’s scaled flex shrink factor (if dividing by zero, treat the result as negative infinity).
    //    This is the item’s desired flex fraction.

    for (auto& item : m_flex_items) {
        CSSPixels contribution = 0;
        if (m_available_space_for_items->main.is_min_content())
            contribution = calculate_main_min_content_contribution(item);
        else if (m_available_space_for_items->main.is_max_content())
            contribution = calculate_main_max_content_contribution(item);

        CSSPixels outer_flex_base_size = item.flex_base_size + item.margins.main_before + item.margins.main_after + item.borders.main_before + item.borders.main_after + item.padding.main_before + item.padding.main_after;

        CSSPixels result = contribution - outer_flex_base_size;
        if (result > 0) {
            if (item.box->computed_values().flex_grow() >= 1) {
                result.scale_by(1 / item.box->computed_values().flex_grow());
            } else {
                result.scale_by(item.box->computed_values().flex_grow());
            }
        } else if (result < 0) {
            if (item.scaled_flex_shrink_factor == 0)
                result = CSSPixels::min();
            else
                result.scale_by(1 / item.scaled_flex_shrink_factor);
        }

        item.desired_flex_fraction = result.to_double();
    }

    // 2. Place all flex items into lines of infinite length.
    m_flex_lines.clear();
    if (!m_flex_items.is_empty())
        m_flex_lines.append(FlexLine {});
    for (auto& item : m_flex_items) {
        // FIXME: Honor breaking requests.
        m_flex_lines.last().items.append(item);
    }

    //    Within each line, find the greatest (most positive) desired flex fraction among all the flex items.
    //    This is the line’s chosen flex fraction.
    for (auto& flex_line : m_flex_lines) {
        float greatest_desired_flex_fraction = 0;
        float sum_of_flex_grow_factors = 0;
        float sum_of_flex_shrink_factors = 0;
        for (auto& item : flex_line.items) {
            greatest_desired_flex_fraction = max(greatest_desired_flex_fraction, item.desired_flex_fraction);
            sum_of_flex_grow_factors += item.box->computed_values().flex_grow();
            sum_of_flex_shrink_factors += item.box->computed_values().flex_shrink();
        }
        float chosen_flex_fraction = greatest_desired_flex_fraction;

        // 3. If the chosen flex fraction is positive, and the sum of the line’s flex grow factors is less than 1,
        //    divide the chosen flex fraction by that sum.
        if (chosen_flex_fraction > 0 && sum_of_flex_grow_factors < 1)
            chosen_flex_fraction /= sum_of_flex_grow_factors;

        // If the chosen flex fraction is negative, and the sum of the line’s flex shrink factors is less than 1,
        // multiply the chosen flex fraction by that sum.
        if (chosen_flex_fraction < 0 && sum_of_flex_shrink_factors < 1)
            chosen_flex_fraction *= sum_of_flex_shrink_factors;

        flex_line.chosen_flex_fraction = chosen_flex_fraction;
    }

    auto determine_main_size = [&]() -> CSSPixels {
        CSSPixels largest_sum = 0;
        for (auto& flex_line : m_flex_lines) {
            // 4. Add each item’s flex base size to the product of its flex grow factor (scaled flex shrink factor, if shrinking)
            //    and the chosen flex fraction, then clamp that result by the max main size floored by the min main size.
            CSSPixels sum = 0;
            for (auto& item : flex_line.items) {
                double product = 0;
                if (item.desired_flex_fraction > 0)
                    product = flex_line.chosen_flex_fraction * static_cast<double>(item.box->computed_values().flex_grow());
                else if (item.desired_flex_fraction < 0)
                    product = flex_line.chosen_flex_fraction * item.scaled_flex_shrink_factor;
                auto result = item.flex_base_size + CSSPixels::nearest_value_for(product);

                auto const& computed_min_size = this->computed_main_min_size(item.box);
                auto const& computed_max_size = this->computed_main_max_size(item.box);

                auto clamp_min = (!computed_min_size.is_auto() && !computed_min_size.contains_percentage()) ? specified_main_min_size(item.box) : automatic_minimum_size(item);
                auto clamp_max = (!should_treat_main_max_size_as_none(item.box) && !computed_max_size.contains_percentage()) ? specified_main_max_size(item.box) : CSSPixels::max();

                result = css_clamp(result, clamp_min, clamp_max);

                // NOTE: The spec doesn't mention anything about the *outer* size here, but if we don't add the margin box,
                //       flex items with non-zero padding/border/margin in the main axis end up overflowing the container.
                result = item.add_main_margin_box_sizes(result);

                sum += result;
            }
            // CSS-FLEXBOX-2: Account for gap between flex items.
            sum += main_gap() * (flex_line.items.size() - 1);
            largest_sum = max(largest_sum, sum);
        }
        // 5. The flex container’s max-content size is the largest sum (among all the lines) of the afore-calculated sizes of all items within a single line.
        return largest_sum;
    };

    auto main_size = determine_main_size();
    set_main_size(flex_container(), main_size);
    return main_size;
}

// https://drafts.csswg.org/css-flexbox-1/#intrinsic-cross-sizes
CSSPixels FlexFormattingContext::calculate_intrinsic_cross_size_of_flex_container()
{
    // The min-content/max-content cross size of a single-line flex container
    // is the largest min-content contribution/max-content contribution (respectively) of its flex items.
    if (is_single_line()) {
        auto calculate_largest_contribution = [&](bool resolve_percentage_min_max_sizes) {
            CSSPixels largest_contribution = 0;
            for (auto& item : m_flex_items) {
                CSSPixels contribution = 0;
                if (m_available_space_for_items->cross.is_min_content())
                    contribution = calculate_cross_min_content_contribution(item, resolve_percentage_min_max_sizes);
                else if (m_available_space_for_items->cross.is_max_content())
                    contribution = calculate_cross_max_content_contribution(item, resolve_percentage_min_max_sizes);
                largest_contribution = max(largest_contribution, contribution);
            }
            return largest_contribution;
        };

        auto first_pass_largest_contribution = calculate_largest_contribution(false);
        set_cross_size(flex_container(), first_pass_largest_contribution);
        auto second_pass_largest_contribution = calculate_largest_contribution(true);
        return second_pass_largest_contribution;
    }

    if (is_row_layout()) {
        // row multi-line flex container cross-size

        // The min-content/max-content cross size is the sum of the flex line cross sizes resulting from
        // sizing the flex container under a cross-axis min-content constraint/max-content constraint (respectively).

        // NOTE: We fall through to the ad-hoc section below.
    } else {
        // column multi-line flex container cross-size

        // The min-content cross size is the largest min-content contribution among all of its flex items.
        if (m_available_space_for_items->cross.is_min_content()) {
            auto calculate_largest_contribution = [&](bool resolve_percentage_min_max_sizes) {
                CSSPixels largest_contribution = 0;
                for (auto& item : m_flex_items) {
                    CSSPixels contribution = calculate_cross_min_content_contribution(item, resolve_percentage_min_max_sizes);
                    largest_contribution = max(largest_contribution, contribution);
                }
                return largest_contribution;
            };

            auto first_pass_largest_contribution = calculate_largest_contribution(false);
            set_cross_size(flex_container(), first_pass_largest_contribution);
            auto second_pass_largest_contribution = calculate_largest_contribution(true);
            return second_pass_largest_contribution;
        }

        // The max-content cross size is the sum of the flex line cross sizes resulting from
        // sizing the flex container under a cross-axis max-content constraint,
        // using the largest max-content cross-size contribution among the flex items
        // as the available space in the cross axis for each of the flex items during layout.

        // NOTE: We fall through to the ad-hoc section below.
    }

    CSSPixels sum_of_flex_line_cross_sizes = 0;
    for (auto& flex_line : m_flex_lines) {
        sum_of_flex_line_cross_sizes += flex_line.cross_size;
    }
    // CSS-FLEXBOX-2: Account for gap between flex lines.
    sum_of_flex_line_cross_sizes += cross_gap() * (m_flex_lines.size() - 1);
    return sum_of_flex_line_cross_sizes;
}

// https://drafts.csswg.org/css-flexbox-1/#intrinsic-item-contributions
CSSPixels FlexFormattingContext::calculate_main_min_content_contribution(FlexItem const& item) const
{
    // The main-size min-content contribution of a flex item is
    // the larger of its outer min-content size and outer preferred size if that is not auto,
    // clamped by its min/max main size.
    auto larger_size = [&] {
        auto inner_min_content_size = calculate_min_content_main_size(item);
        if (computed_main_size(item.box).is_auto())
            return inner_min_content_size;
        auto inner_preferred_size = is_row_layout() ? get_pixel_width(item.box, computed_main_size(item.box)) : get_pixel_height(item.box, computed_main_size(item.box));
        return max(inner_min_content_size, inner_preferred_size);
    }();

    auto clamp_min = has_main_min_size(item.box) ? specified_main_min_size(item.box) : automatic_minimum_size(item);
    auto clamp_max = has_main_max_size(item.box) ? specified_main_max_size(item.box) : CSSPixels::max();
    auto clamped_inner_size = css_clamp(larger_size, clamp_min, clamp_max);

    return item.add_main_margin_box_sizes(clamped_inner_size);
}

// https://drafts.csswg.org/css-flexbox-1/#intrinsic-item-contributions
CSSPixels FlexFormattingContext::calculate_main_max_content_contribution(FlexItem const& item) const
{
    // The main-size max-content contribution of a flex item is
    // the larger of its outer max-content size and outer preferred size if that is not auto,
    // clamped by its min/max main size.
    auto larger_size = [&] {
        auto inner_max_content_size = calculate_max_content_main_size(item);
        if (computed_main_size(item.box).is_auto())
            return inner_max_content_size;
        auto inner_preferred_size = is_row_layout() ? get_pixel_width(item.box, computed_main_size(item.box)) : get_pixel_height(item.box, computed_main_size(item.box));
        return max(inner_max_content_size, inner_preferred_size);
    }();

    auto clamp_min = has_main_min_size(item.box) ? specified_main_min_size(item.box) : automatic_minimum_size(item);
    auto clamp_max = has_main_max_size(item.box) ? specified_main_max_size(item.box) : CSSPixels::max();
    auto clamped_inner_size = css_clamp(larger_size, clamp_min, clamp_max);

    return item.add_main_margin_box_sizes(clamped_inner_size);
}

bool FlexFormattingContext::should_treat_main_size_as_auto(Box const& box) const
{
    if (is_row_layout())
        return should_treat_width_as_auto(box, m_available_space_for_items->space);
    return should_treat_height_as_auto(box, m_available_space_for_items->space);
}

bool FlexFormattingContext::should_treat_cross_size_as_auto(Box const& box) const
{
    if (is_row_layout())
        return should_treat_height_as_auto(box, m_available_space_for_items->space);
    return should_treat_width_as_auto(box, m_available_space_for_items->space);
}

bool FlexFormattingContext::should_treat_main_max_size_as_none(Box const& box) const
{
    if (is_row_layout())
        return should_treat_max_width_as_none(box, m_available_space_for_items->space.width);
    return should_treat_max_height_as_none(box, m_available_space_for_items->space.height);
}

bool FlexFormattingContext::should_treat_cross_max_size_as_none(Box const& box) const
{
    if (is_row_layout())
        return should_treat_max_height_as_none(box, m_available_space_for_items->space.height);
    return should_treat_max_width_as_none(box, m_available_space_for_items->space.width);
}

CSSPixels FlexFormattingContext::calculate_cross_min_content_contribution(FlexItem const& item, bool resolve_percentage_min_max_sizes) const
{
    auto size = [&] {
        if (should_treat_cross_size_as_auto(item.box))
            return calculate_min_content_cross_size(item);
        return !is_row_layout() ? get_pixel_width(item.box, computed_cross_size(item.box)) : get_pixel_height(item.box, computed_cross_size(item.box));
    }();

    auto const& computed_min_size = this->computed_cross_min_size(item.box);
    auto const& computed_max_size = this->computed_cross_max_size(item.box);

    auto clamp_min = (!computed_min_size.is_auto() && (resolve_percentage_min_max_sizes || !computed_min_size.contains_percentage())) ? specified_cross_min_size(item.box) : 0;
    auto clamp_max = (!should_treat_cross_max_size_as_none(item.box) && (resolve_percentage_min_max_sizes || !computed_max_size.contains_percentage())) ? specified_cross_max_size(item.box) : CSSPixels::max();

    auto clamped_inner_size = css_clamp(size, clamp_min, clamp_max);

    return item.add_cross_margin_box_sizes(clamped_inner_size);
}

CSSPixels FlexFormattingContext::calculate_cross_max_content_contribution(FlexItem const& item, bool resolve_percentage_min_max_sizes) const
{
    auto size = [&] {
        if (should_treat_cross_size_as_auto(item.box))
            return calculate_max_content_cross_size(item);
        return !is_row_layout() ? get_pixel_width(item.box, computed_cross_size(item.box)) : get_pixel_height(item.box, computed_cross_size(item.box));
    }();

    auto const& computed_min_size = this->computed_cross_min_size(item.box);
    auto const& computed_max_size = this->computed_cross_max_size(item.box);

    auto clamp_min = (!computed_min_size.is_auto() && (resolve_percentage_min_max_sizes || !computed_min_size.contains_percentage())) ? specified_cross_min_size(item.box) : 0;
    auto clamp_max = (!should_treat_cross_max_size_as_none(item.box) && (resolve_percentage_min_max_sizes || !computed_max_size.contains_percentage())) ? specified_cross_max_size(item.box) : CSSPixels::max();

    auto clamped_inner_size = css_clamp(size, clamp_min, clamp_max);

    return item.add_cross_margin_box_sizes(clamped_inner_size);
}

CSSPixels FlexFormattingContext::calculate_width_to_use_when_determining_intrinsic_height_of_item(FlexItem const& item) const
{
    auto const& box = *item.box;
    auto computed_width = box.computed_values().width();
    auto const& computed_min_width = box.computed_values().min_width();
    auto const& computed_max_width = box.computed_values().max_width();
    auto clamp_min = (!computed_min_width.is_auto() && (!computed_min_width.contains_percentage())) ? get_pixel_width(box, computed_min_width) : 0;
    auto clamp_max = (!should_treat_max_width_as_none(box, m_available_space_for_items->space.width) && (!computed_max_width.contains_percentage())) ? get_pixel_width(box, computed_max_width) : CSSPixels::max();

    CSSPixels width;
    if (should_treat_width_as_auto(box, m_available_space_for_items->space) || computed_width.is_fit_content())
        width = calculate_fit_content_width(box, m_available_space_for_items->space);
    else if (computed_width.is_min_content())
        width = calculate_min_content_width(box);
    else if (computed_width.is_max_content())
        width = calculate_max_content_width(box);

    return css_clamp(width, clamp_min, clamp_max);
}

CSSPixels FlexFormattingContext::calculate_min_content_main_size(FlexItem const& item) const
{
    if (is_row_layout()) {
        return calculate_min_content_width(item.box);
    }
    auto available_space = item.used_values.available_inner_space_or_constraints_from(m_available_space_for_items->space);
    if (available_space.width.is_indefinite()) {
        available_space.width = AvailableSize::make_definite(calculate_width_to_use_when_determining_intrinsic_height_of_item(item));
    }
    return calculate_min_content_height(item.box, available_space.width.to_px_or_zero());
}

CSSPixels FlexFormattingContext::calculate_max_content_main_size(FlexItem const& item) const
{
    if (is_row_layout()) {
        return calculate_max_content_width(item.box);
    }
    auto available_space = item.used_values.available_inner_space_or_constraints_from(m_available_space_for_items->space);
    if (available_space.width.is_indefinite()) {
        available_space.width = AvailableSize::make_definite(calculate_width_to_use_when_determining_intrinsic_height_of_item(item));
    }
    return calculate_max_content_height(item.box, available_space.width.to_px_or_zero());
}

CSSPixels FlexFormattingContext::calculate_fit_content_main_size(FlexItem const& item) const
{
    if (is_row_layout())
        return calculate_fit_content_width(item.box, m_available_space_for_items->space);
    return calculate_fit_content_height(item.box, m_available_space_for_items->space);
}

CSSPixels FlexFormattingContext::calculate_fit_content_cross_size(FlexItem const& item) const
{
    if (!is_row_layout())
        return calculate_fit_content_width(item.box, m_available_space_for_items->space);
    return calculate_fit_content_height(item.box, m_available_space_for_items->space);
}

CSSPixels FlexFormattingContext::calculate_min_content_cross_size(FlexItem const& item) const
{
    if (is_row_layout()) {
        auto available_space = item.used_values.available_inner_space_or_constraints_from(m_available_space_for_items->space);
        if (available_space.width.is_indefinite()) {
            available_space.width = AvailableSize::make_definite(calculate_width_to_use_when_determining_intrinsic_height_of_item(item));
        }
        return calculate_min_content_height(item.box, available_space.width.to_px_or_zero());
    }
    return calculate_min_content_width(item.box);
}

CSSPixels FlexFormattingContext::calculate_max_content_cross_size(FlexItem const& item) const
{
    if (is_row_layout()) {
        auto available_space = item.used_values.available_inner_space_or_constraints_from(m_available_space_for_items->space);
        if (available_space.width.is_indefinite()) {
            available_space.width = AvailableSize::make_definite(calculate_width_to_use_when_determining_intrinsic_height_of_item(item));
        }
        return calculate_max_content_height(item.box, available_space.width.to_px_or_zero());
    }
    return calculate_max_content_width(item.box);
}

// https://drafts.csswg.org/css-flexbox-1/#stretched
bool FlexFormattingContext::flex_item_is_stretched(FlexItem const& item) const
{
    auto alignment = alignment_for_item(item.box);
    if (alignment != CSS::AlignItems::Stretch && alignment != CSS::AlignItems::Normal)
        return false;
    // If the cross size property of the flex item computes to auto, and neither of the cross-axis margins are auto, the flex item is stretched.
    auto const& computed_cross_size = is_row_layout() ? item.box->computed_values().height() : item.box->computed_values().width();
    return computed_cross_size.is_auto() && !item.margins.cross_before_is_auto && !item.margins.cross_after_is_auto;
}

CSS::Size const& FlexFormattingContext::computed_main_size(Box const& box) const
{
    return is_row_layout() ? box.computed_values().width() : box.computed_values().height();
}

CSS::Size const& FlexFormattingContext::computed_main_min_size(Box const& box) const
{
    return is_row_layout() ? box.computed_values().min_width() : box.computed_values().min_height();
}

CSS::Size const& FlexFormattingContext::computed_main_max_size(Box const& box) const
{
    return is_row_layout() ? box.computed_values().max_width() : box.computed_values().max_height();
}

CSS::Size const& FlexFormattingContext::computed_cross_size(Box const& box) const
{
    return !is_row_layout() ? box.computed_values().width() : box.computed_values().height();
}

CSS::Size const& FlexFormattingContext::computed_cross_min_size(Box const& box) const
{
    return !is_row_layout() ? box.computed_values().min_width() : box.computed_values().min_height();
}

CSS::Size const& FlexFormattingContext::computed_cross_max_size(Box const& box) const
{
    return !is_row_layout() ? box.computed_values().max_width() : box.computed_values().max_height();
}

// https://drafts.csswg.org/css-flexbox-1/#algo-cross-margins
void FlexFormattingContext::resolve_cross_axis_auto_margins()
{
    for (auto& line : m_flex_lines) {
        for (auto& item : line.items) {
            //  If a flex item has auto cross-axis margins:
            if (!item.margins.cross_before_is_auto && !item.margins.cross_after_is_auto)
                continue;

            // If its outer cross size (treating those auto margins as zero) is less than the cross size of its flex line,
            // distribute the difference in those sizes equally to the auto margins.
            auto outer_cross_size = item.cross_size.value() + item.padding.cross_before + item.padding.cross_after + item.borders.cross_before + item.borders.cross_after;
            if (outer_cross_size < line.cross_size) {
                CSSPixels remainder = line.cross_size - outer_cross_size;
                if (item.margins.cross_before_is_auto && item.margins.cross_after_is_auto) {
                    item.margins.cross_before = remainder / 2;
                    item.margins.cross_after = remainder / 2;
                } else if (item.margins.cross_before_is_auto) {
                    item.margins.cross_before = remainder;
                } else {
                    item.margins.cross_after = remainder;
                }
            } else {
                // FIXME: Otherwise, if the block-start or inline-start margin (whichever is in the cross axis) is auto, set it to zero.
                //        Set the opposite margin so that the outer cross size of the item equals the cross size of its flex line.
            }
        }
    }
}

// https://drafts.csswg.org/css-flexbox-1/#algo-line-stretch
void FlexFormattingContext::handle_align_content_stretch()
{
    // If the flex container has a definite cross size,
    if (!has_definite_cross_size(m_flex_container_state))
        return;

    // align-content is stretch,
    if (flex_container().computed_values().align_content() != CSS::AlignContent::Stretch && flex_container().computed_values().align_content() != CSS::AlignContent::Normal)
        return;

    // and the sum of the flex lines' cross sizes is less than the flex container’s inner cross size,
    CSSPixels sum_of_flex_line_cross_sizes = 0;
    for (auto& line : m_flex_lines)
        sum_of_flex_line_cross_sizes += line.cross_size;

    // CSS-FLEXBOX-2: Account for gap between flex lines.
    sum_of_flex_line_cross_sizes += cross_gap() * (m_flex_lines.size() - 1);

    if (sum_of_flex_line_cross_sizes >= inner_cross_size(m_flex_container_state))
        return;

    // increase the cross size of each flex line by equal amounts
    // such that the sum of their cross sizes exactly equals the flex container’s inner cross size.
    CSSPixels remainder = inner_cross_size(m_flex_container_state) - sum_of_flex_line_cross_sizes;
    CSSPixels extra_per_line = remainder / m_flex_lines.size();

    for (auto& line : m_flex_lines)
        line.cross_size += extra_per_line;
}

// https://drafts.csswg.org/css-flexbox-1/#abspos-items
CSSPixelPoint FlexFormattingContext::calculate_static_position(Box const& box) const
{
    // The cross-axis edges of the static-position rectangle of an absolutely-positioned child
    // of a flex container are the content edges of the flex container.
    CSSPixels cross_offset = 0;
    CSSPixels half_line_size = inner_cross_size(m_flex_container_state) / 2;

    auto cross_to_px = [&](CSS::LengthPercentage const& length_percentage) -> CSSPixels {
        return length_percentage.to_px(box, m_flex_container_state.content_width());
    };

    auto main_to_px = [&](CSS::LengthPercentage const& length_percentage) -> CSSPixels {
        return length_percentage.to_px(box, m_flex_container_state.content_width());
    };

    auto const& box_state = m_state.get(box);
    CSSPixels cross_margin_before = is_row_layout() ? cross_to_px(box.computed_values().margin().top()) : cross_to_px(box.computed_values().margin().left());
    CSSPixels cross_margin_after = is_row_layout() ? cross_to_px(box.computed_values().margin().bottom()) : cross_to_px(box.computed_values().margin().right());
    CSSPixels cross_border_before = is_row_layout() ? box.computed_values().border_top().width : box.computed_values().border_left().width;
    CSSPixels cross_border_after = is_row_layout() ? box.computed_values().border_bottom().width : box.computed_values().border_right().width;
    CSSPixels cross_padding_before = is_row_layout() ? cross_to_px(box.computed_values().padding().top()) : cross_to_px(box.computed_values().padding().left());
    CSSPixels cross_padding_after = is_row_layout() ? cross_to_px(box.computed_values().padding().bottom()) : cross_to_px(box.computed_values().padding().right());
    CSSPixels main_margin_before = is_row_layout() ? main_to_px(box.computed_values().margin().left()) : main_to_px(box.computed_values().margin().top());
    CSSPixels main_margin_after = is_row_layout() ? main_to_px(box.computed_values().margin().right()) : main_to_px(box.computed_values().margin().bottom());
    CSSPixels main_border_before = is_row_layout() ? box.computed_values().border_left().width : box.computed_values().border_top().width;
    CSSPixels main_border_after = is_row_layout() ? box.computed_values().border_right().width : box.computed_values().border_bottom().width;
    CSSPixels main_padding_before = is_row_layout() ? main_to_px(box.computed_values().padding().left()) : main_to_px(box.computed_values().padding().top());
    CSSPixels main_padding_after = is_row_layout() ? main_to_px(box.computed_values().padding().right()) : main_to_px(box.computed_values().padding().bottom());

    switch (alignment_for_item(box)) {
    case CSS::AlignItems::Baseline:
        // FIXME: Implement this
        //  Fallthrough
    case CSS::AlignItems::Start:
    case CSS::AlignItems::FlexStart:
    case CSS::AlignItems::SelfStart:
    case CSS::AlignItems::Stretch:
    case CSS::AlignItems::Normal:
        cross_offset = -half_line_size;
        break;
    case CSS::AlignItems::End:
    case CSS::AlignItems::SelfEnd:
    case CSS::AlignItems::FlexEnd:
        cross_offset = half_line_size - inner_cross_size(box_state) - (cross_margin_before + cross_margin_after) - (cross_border_before + cross_border_after) - (cross_padding_before + cross_padding_after);
        break;
    case CSS::AlignItems::Center:
        cross_offset = -((inner_cross_size(box_state) + cross_margin_after + cross_margin_before + cross_border_before + cross_border_after + cross_padding_before + cross_padding_after) / 2);
        break;
    default:
        break;
    }

    cross_offset += inner_cross_size(m_flex_container_state) / 2;

    // The main-axis edges of the static-position rectangle are where the margin edges of the child
    // would be positioned if it were the sole flex item in the flex container,
    // assuming both the child and the flex container were fixed-size boxes of their used size.
    // (For this purpose, auto margins are treated as zero.

    bool pack_from_end = true;
    CSSPixels main_offset = 0;
    switch (flex_container().computed_values().justify_content()) {
    case CSS::JustifyContent::Start:
        pack_from_end = false;
        break;
    case CSS::JustifyContent::Stretch:
    case CSS::JustifyContent::Normal:
    case CSS::JustifyContent::FlexStart:
    case CSS::JustifyContent::SpaceBetween:
        pack_from_end = is_direction_reverse();
        break;
    case CSS::JustifyContent::End:
        pack_from_end = true;
        break;
    case CSS::JustifyContent::FlexEnd:
        pack_from_end = !is_direction_reverse();
        break;
    case CSS::JustifyContent::Center:
    case CSS::JustifyContent::SpaceAround:
    case CSS::JustifyContent::SpaceEvenly:
        pack_from_end = false;
        main_offset = (inner_main_size(m_flex_container_state) - inner_main_size(box_state) - main_margin_before - main_margin_after - main_border_before - main_border_after - main_padding_before - main_padding_after) / 2;
        break;
    }

    if (pack_from_end)
        main_offset += inner_main_size(m_flex_container_state) - inner_main_size(box_state) - main_margin_before - main_margin_after - main_border_before - main_border_after - main_padding_before - main_padding_after;

    auto static_position_offset = is_row_layout() ? CSSPixelPoint { main_offset, cross_offset } : CSSPixelPoint { cross_offset, main_offset };

    auto absolute_position_of_flex_container = absolute_content_rect(flex_container()).location();
    auto absolute_position_of_abspos_containing_block = absolute_content_rect(*box.containing_block()).location();
    auto diff = absolute_position_of_flex_container - absolute_position_of_abspos_containing_block;

    return static_position_offset + diff;
}

double FlexFormattingContext::FlexLine::sum_of_flex_factor_of_unfrozen_items() const
{
    double sum = 0;
    for (auto const& item : items) {
        if (!item.frozen)
            sum += item.flex_factor.value();
    }
    return sum;
}

double FlexFormattingContext::FlexLine::sum_of_scaled_flex_shrink_factor_of_unfrozen_items() const
{
    double sum = 0;
    for (auto const& item : items) {
        if (!item.frozen)
            sum += item.scaled_flex_shrink_factor;
    }
    return sum;
}

CSSPixels FlexFormattingContext::main_gap() const
{
    auto const& computed_values = flex_container().computed_values();
    auto gap = is_row_layout() ? computed_values.column_gap() : computed_values.row_gap();
    return gap.to_px(flex_container(), inner_main_size(m_flex_container_state));
}

CSSPixels FlexFormattingContext::cross_gap() const
{
    auto const& computed_values = flex_container().computed_values();
    auto gap = is_row_layout() ? computed_values.row_gap() : computed_values.column_gap();
    return gap.to_px(flex_container(), inner_cross_size(m_flex_container_state));
}

}
