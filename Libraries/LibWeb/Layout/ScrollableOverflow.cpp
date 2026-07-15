/*
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Tuple.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/SVGSVGBox.h>
#include <LibWeb/Layout/ScrollableOverflow.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/PaintableWithLines.h>

namespace Web::Layout {

ScrollableOverflowMeasurementWork collect_scrollable_overflow_measurement_work(Node const& root)
{
    auto needs_measurement = [](Box const& box) {
        auto paintable = box.paintable_box();
        return paintable && !paintable->overflow_data().has_value();
    };

    ScrollableOverflowMeasurementWork work;
    root.for_each_in_inclusive_subtree([&](auto& node) {
        auto const* box = as_if<Box>(node);
        if (!box || !box->paintable_box())
            return TraversalDecision::Continue;
        if (needs_measurement(*box))
            work.boxes_to_measure.append(box);
        if (auto const* containing_block = box->containing_block())
            work.contained_boxes_map.ensure(containing_block).append(box);
        return TraversalDecision::Continue;
    });
    return work;
}

struct PhysicalOverflowDirections {
    bool x_positive { true };
    bool y_positive { true };
};

struct LogicalAxis {
    bool is_horizontal { false };
    bool is_reverse { false };
};

static bool inline_axis_is_horizontal(CSS::WritingMode writing_mode)
{
    return writing_mode == CSS::WritingMode::HorizontalTb;
}

static bool node_is_in_focused_text_control(DOM::Node const& node)
{
    auto shadow_root = node.containing_shadow_root();
    return shadow_root
        && shadow_root->is_user_agent_internal()
        && is<HTML::FormAssociatedTextControlElement>(shadow_root->host())
        && shadow_root->host()->is_focused();
}

static PhysicalOverflowDirections physical_overflow_directions(Box const& box)
{
    auto const& computed_values = box.computed_values();
    LogicalAxis inline_axis {
        .is_horizontal = inline_axis_is_horizontal(computed_values.writing_mode()),
        .is_reverse = computed_values.inline_axis_is_reverse(),
    };
    LogicalAxis block_axis {
        .is_horizontal = !inline_axis.is_horizontal,
        .is_reverse = computed_values.block_axis_is_reverse(),
    };

    auto horizontal_and_vertical_axes = [&]() {
        if (!box.display().is_flex_inside())
            return AK::Tuple { inline_axis.is_horizontal ? inline_axis : block_axis, inline_axis.is_horizontal ? block_axis : inline_axis };

        auto is_row_layout = computed_values.flex_direction() == CSS::FlexDirection::Row
            || computed_values.flex_direction() == CSS::FlexDirection::RowReverse;

        auto main_axis = is_row_layout ? inline_axis : block_axis;
        if (computed_values.flex_direction() == CSS::FlexDirection::RowReverse
            || computed_values.flex_direction() == CSS::FlexDirection::ColumnReverse) {
            main_axis.is_reverse = !main_axis.is_reverse;
        }

        auto cross_axis = is_row_layout ? block_axis : inline_axis;
        if (computed_values.flex_wrap() == CSS::FlexWrap::WrapReverse)
            cross_axis.is_reverse = !cross_axis.is_reverse;

        return AK::Tuple { main_axis.is_horizontal ? main_axis : cross_axis, main_axis.is_horizontal ? cross_axis : main_axis };
    };

    auto axes = horizontal_and_vertical_axes();
    auto horizontal_axis = axes.get<0>();
    auto vertical_axis = axes.get<1>();
    return {
        .x_positive = !horizontal_axis.is_reverse,
        .y_positive = !vertical_axis.is_reverse,
    };
}

static CSSPixelRect apply_css_transform_to_overflow_rect(Box const& box, CSSPixelRect const& rect)
{
    auto const& paintable_box = *box.paintable_box();
    auto transform_data = Painting::compute_transform(paintable_box, box.computed_values(), 1.0);
    if (!transform_data.has_value())
        return rect;

    auto affine = Gfx::extract_2d_affine_transform(transform_data->matrix);
    auto transformed_rect = rect.to_type<float>();
    transformed_rect.translate_by(-transform_data->origin);
    transformed_rect = affine.map(transformed_rect);
    transformed_rect.translate_by(transform_data->origin);
    return transformed_rect.to_type<CSSPixels>();
}

static CSSPixelRect padding_inflated_scrollable_overflow(Box const& box, CSSPixelRect const& in_flow_and_floated_content_bounds)
{
    auto const& paintable_box = *box.paintable_box();
    auto const content_box = paintable_box.absolute_rect();
    auto const padding_box = paintable_box.absolute_padding_box_rect();
    auto const& padding = paintable_box.box_model().padding;
    auto overflow_directions = physical_overflow_directions(box);

    auto left = in_flow_and_floated_content_bounds.left();
    auto top = in_flow_and_floated_content_bounds.top();
    auto right = in_flow_and_floated_content_bounds.right();
    auto bottom = in_flow_and_floated_content_bounds.bottom();

    auto in_flow_bounds_overflow_content_box_in_x_axis = left < content_box.left() || right > content_box.right();
    if (in_flow_bounds_overflow_content_box_in_x_axis) {
        if (overflow_directions.x_positive && right > content_box.right())
            right += padding.right;
        else if (!overflow_directions.x_positive)
            left = min(left, padding_box.left()) - padding.left;
    }

    auto in_flow_bounds_overflow_content_box_in_y_axis = top < content_box.top() || bottom > content_box.bottom();
    if (in_flow_bounds_overflow_content_box_in_y_axis) {
        if (overflow_directions.y_positive && bottom > content_box.bottom())
            bottom += padding.bottom;
        else if (!overflow_directions.y_positive)
            top = min(top, padding_box.top()) - padding.top;
    }

    return { left, top, right - left, bottom - top };
}

CSSPixelRect measure_scrollable_overflow(Box const& box, ContainedBoxesMap const& contained_boxes_map)
{
    if (!box.paintable_box())
        return {};

    auto const& paintable_box = *box.paintable_box();

    if (paintable_box.scrollable_overflow_rect().has_value())
        return paintable_box.scrollable_overflow_rect().value();

    // https://drafts.csswg.org/css-overflow-3/#scrollable-overflow-calculation
    // The scrollable overflow area of a box is the union of:

    // - Its own padding box.
    auto const paintable_absolute_padding_box = paintable_box.absolute_padding_box_rect();
    auto const paintable_absolute_content_box = paintable_box.absolute_rect();
    auto scrollable_overflow_rect = paintable_absolute_padding_box;
    auto in_flow_and_floated_content_bounds = paintable_absolute_content_box;

    // Replaced SVG viewports clip their content
    if (is<SVGSVGBox>(box)) {
        const_cast<Painting::Paintable&>(paintable_box).set_overflow_data({
            .scrollable_overflow_rect = scrollable_overflow_rect,
            .has_scrollable_overflow = false,
        });
        return scrollable_overflow_rect;
    }

    auto overflow_directions = physical_overflow_directions(box);
    // - All line boxes it directly contains.
    if (auto paintable = box.paintable(); auto const* paintable_with_lines = as_if<Painting::PaintableWithLines>(paintable.ptr())) {
        for (auto const& fragment : paintable_with_lines->fragments()) {
            auto fragment_rect = fragment.absolute_rect();
            if (auto const* dom_node = fragment.layout_node().dom_node(); dom_node && node_is_in_focused_text_control(*dom_node)) {
                // NB: Reserve one pixel of reachable inline-axis overflow for an end-of-line caret. This keeps the
                //     caret at its insertion position while allowing a text control to scroll the painted bar fully
                //     into view, matching the caret overflow accounted for by other engines.
                auto const& computed_values = fragment.style_source().computed_values();
                if (inline_axis_is_horizontal(computed_values.writing_mode())) {
                    if (computed_values.inline_axis_is_reverse())
                        fragment_rect.set_left(fragment_rect.left() - 1);
                    else
                        fragment_rect.set_right(fragment_rect.right() + 1);
                } else {
                    if (computed_values.inline_axis_is_reverse())
                        fragment_rect.set_top(fragment_rect.top() - 1);
                    else
                        fragment_rect.set_bottom(fragment_rect.bottom() + 1);
                }
            }
            scrollable_overflow_rect.unite(fragment_rect);
            in_flow_and_floated_content_bounds.unite(fragment_rect);
        }
    }

    // - The border boxes of all boxes for which it is the containing block and whose border boxes are positioned not
    //   wholly in the negative scrollable overflow region,
    //   FIXME: accounting for 3D transforms by projecting each box onto the plane of the element that establishes
    //          its 3D rendering context. [CSS3-TRANSFORMS]
    if (auto it = contained_boxes_map.find(&box); it != contained_boxes_map.end()) {
        for (auto const* child_ptr : it->value) {
            auto const& child = *child_ptr;

            // https://drafts.csswg.org/css-position/#fixed-positioning-containing-block
            // [..] As a result, parts of fixed-positioned boxes that extend outside the layout viewport/page area
            //      cannot be scrolled to and will not print.
            // FIXME: Properly establish the fixed positioning containing block for `position: fixed`
            if (child.is_fixed_position())
                continue;

            auto untransformed_child_border_box = child.paintable_box()->absolute_border_box_rect();
            auto child_border_box = apply_css_transform_to_overflow_rect(child, untransformed_child_border_box);

            // NOTE: Only boxes that are not wholly in the unreachable scrollable overflow region contribute.
            auto wholly_in_unreachable_x = overflow_directions.x_positive
                ? child_border_box.right() < paintable_absolute_padding_box.x()
                : child_border_box.x() > paintable_absolute_padding_box.right();
            auto wholly_in_unreachable_y = overflow_directions.y_positive
                ? child_border_box.bottom() < paintable_absolute_padding_box.y()
                : child_border_box.y() > paintable_absolute_padding_box.bottom();
            if (wholly_in_unreachable_x || wholly_in_unreachable_y)
                continue;

            // Border boxes with zero area do not affect the scrollable overflow area.
            if (!child_border_box.is_empty()) {
                scrollable_overflow_rect.unite(child_border_box);
                if (child.is_in_flow() || child.is_floating())
                    in_flow_and_floated_content_bounds.unite(untransformed_child_border_box);
            }

            // - The scrollable overflow areas of all of the above boxes (including zero-area boxes and accounting for
            //   transforms as described above), provided they themselves have overflow: visible (i.e. do not themselves
            //   trap the overflow) and that scrollable overflow is not already clipped (e.g. by the clip property or the
            //   contain property).
            // Scrollable overflow is already clipped by the contain property.
            if (child.has_layout_containment() || child.has_paint_containment())
                continue;

            if (child.computed_values().overflow_x() == CSS::Overflow::Visible || child.computed_values().overflow_y() == CSS::Overflow::Visible) {
                auto child_scrollable_overflow = apply_css_transform_to_overflow_rect(child, measure_scrollable_overflow(child, contained_boxes_map));
                if (!child_scrollable_overflow.is_empty()) {
                    if (child.computed_values().overflow_x() == CSS::Overflow::Visible) {
                        scrollable_overflow_rect.unite_horizontally(child_scrollable_overflow);
                    }
                    if (child.computed_values().overflow_y() == CSS::Overflow::Visible) {
                        scrollable_overflow_rect.unite_vertically(child_scrollable_overflow);
                    }
                }
            }
        }
    }

    // FIXME: - The margin areas of grid item and flex item boxes for which the box establishes a containing block.

    // - Additional padding added to the scrollable overflow rectangle as necessary to enable scroll positions that
    //   satisfy the requirements of both place-content: start and place-content: end alignment.
    //
    // NOTE: This padding represents, within the scrollable overflow rectangle, the box’s own padding so that when its
    //       content is scrolled to its end, there is padding between the edge of its in-flow (or floated) content and
    //       the border edge of the box. It typically ends up being exactly the same size as the box's own padding,
    //       except in a few cases--such as when an out-of-flow positioned element, or the visible overflow of a
    //       descendent, has already increased the size of the scrollable overflow rectangle outside the conceptual
    //       “content edge” of the scroll container’s content.
    if (box.is_scroll_container())
        scrollable_overflow_rect.unite(padding_inflated_scrollable_overflow(box, in_flow_and_floated_content_bounds));

    auto has_scrollable_overflow = box.is_scroll_container()
        && !paintable_absolute_padding_box.contains(scrollable_overflow_rect);

    // Additionally, due to Web-compatibility constraints (caused by authors exploiting legacy bugs to surreptitiously
    // hide content from visual readers but not search engines and/or speech output), UAs must clip any content in the
    // unreachable scrollable overflow region.
    //
    // https://drafts.csswg.org/css-overflow-3/#unreachable-scrollable-overflow-region
    // Unless otherwise adjusted (e.g. by content alignment [css-align-3]), the area beyond the scroll origin in either
    // axis is considered the unreachable scrollable overflow region: content rendered here is not accessible to the
    // reader, see § 2.2 Scrollable Overflow.
    auto left = overflow_directions.x_positive ? max(scrollable_overflow_rect.x(), paintable_absolute_padding_box.x()) : scrollable_overflow_rect.x();
    auto top = overflow_directions.y_positive ? max(scrollable_overflow_rect.y(), paintable_absolute_padding_box.y()) : scrollable_overflow_rect.y();
    auto right = overflow_directions.x_positive ? scrollable_overflow_rect.right() : min(scrollable_overflow_rect.right(), paintable_absolute_padding_box.right());
    auto bottom = overflow_directions.y_positive ? scrollable_overflow_rect.bottom() : min(scrollable_overflow_rect.bottom(), paintable_absolute_padding_box.bottom());
    if (left != scrollable_overflow_rect.x() || top != scrollable_overflow_rect.y() || right != scrollable_overflow_rect.right() || bottom != scrollable_overflow_rect.bottom()) {
        scrollable_overflow_rect = {
            left,
            top,
            max(right - left, CSSPixels { 0 }),
            max(bottom - top, CSSPixels { 0 }),
        };
        has_scrollable_overflow = !paintable_absolute_padding_box.contains(scrollable_overflow_rect) && box.is_scroll_container();
    }

    const_cast<Painting::Paintable&>(paintable_box).set_overflow_data({
        .scrollable_overflow_rect = scrollable_overflow_rect,
        .has_scrollable_overflow = has_scrollable_overflow,
    });

    return scrollable_overflow_rect;
}

}
