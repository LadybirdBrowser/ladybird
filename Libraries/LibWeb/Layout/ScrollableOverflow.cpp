/*
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Tuple.h>
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
        if (auto const* containing_block = box->containing_block(); containing_block && needs_measurement(*containing_block))
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

CSSPixelRect measure_scrollable_overflow(Box const& box, ContainedBoxesMap const& contained_boxes_map)
{
    if (!box.paintable_box())
        return {};

    auto const& paintable_box = *box.paintable_box();

    if (paintable_box.scrollable_overflow_rect().has_value())
        return paintable_box.scrollable_overflow_rect().value();

    // The scrollable overflow area is the union of:

    // - The scroll container’s own padding box.
    auto const paintable_absolute_padding_box = paintable_box.absolute_padding_box_rect();
    auto const paintable_absolute_content_box = paintable_box.absolute_rect();
    auto scrollable_overflow_rect = paintable_absolute_padding_box;

    // Replaced SVG viewports clip their content
    if (is<SVGSVGBox>(box)) {
        const_cast<Painting::Paintable&>(paintable_box).set_overflow_data({
            .scrollable_overflow_rect = scrollable_overflow_rect,
            .has_scrollable_overflow = false,
        });
        return scrollable_overflow_rect;
    }

    auto overflow_directions = physical_overflow_directions(box);
    auto content_overflows_content_box_in_x_axis = false;
    auto content_overflows_content_box_in_y_axis = false;
    auto content_overflows_content_box_on_right = false;
    auto content_overflows_content_box_on_bottom = false;

    auto update_content_overflow_x_axis = [&](CSSPixelRect const& rect) {
        if (rect.left() < paintable_absolute_content_box.left()
            || rect.right() > paintable_absolute_content_box.right()) {
            content_overflows_content_box_in_x_axis = true;
        }
        if (rect.right() > paintable_absolute_content_box.right())
            content_overflows_content_box_on_right = true;
    };

    auto update_content_overflow_y_axis = [&](CSSPixelRect const& rect) {
        if (rect.top() < paintable_absolute_content_box.top()
            || rect.bottom() > paintable_absolute_content_box.bottom()) {
            content_overflows_content_box_in_y_axis = true;
        }
        if (rect.bottom() > paintable_absolute_content_box.bottom())
            content_overflows_content_box_on_bottom = true;
    };

    auto update_content_overflow_axes = [&](CSSPixelRect const& rect) {
        update_content_overflow_x_axis(rect);
        update_content_overflow_y_axis(rect);
    };

    // - All line boxes directly contained by the scroll container.
    if (auto first_paintable = box.first_paintable(); auto const* paintable_with_lines = as_if<Painting::PaintableWithLines>(first_paintable.ptr())) {
        for (auto const& fragment : paintable_with_lines->fragments()) {
            auto fragment_rect = fragment.absolute_rect();
            scrollable_overflow_rect.unite(fragment_rect);
            update_content_overflow_axes(fragment_rect);
        }
    }

    auto content_overflow_rect = scrollable_overflow_rect;

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

            auto child_border_box = apply_css_transform_to_overflow_rect(child, child.paintable_box()->absolute_border_box_rect());

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
                content_overflow_rect.unite(child_border_box);
                update_content_overflow_axes(child_border_box);
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
                        update_content_overflow_x_axis(child_scrollable_overflow);
                    }
                    if (child.computed_values().overflow_y() == CSS::Overflow::Visible) {
                        scrollable_overflow_rect.unite_vertically(child_scrollable_overflow);
                        update_content_overflow_y_axis(child_scrollable_overflow);
                    }
                }
            }
        }
    }

    // FIXME: - The margin areas of grid item and flex item boxes for which the box establishes a containing block.

    // - Additional padding added to the scrollable overflow rectangle as necessary to enable scroll positions that
    //   satisfy the requirements of both place-content: start and place-content: end alignment.
    auto has_scrollable_overflow_in_x_axis = box.is_scroll_container()
        && (scrollable_overflow_rect.left() < paintable_absolute_padding_box.left()
            || scrollable_overflow_rect.right() > paintable_absolute_padding_box.right());
    auto has_scrollable_overflow_in_y_axis = box.is_scroll_container()
        && (scrollable_overflow_rect.top() < paintable_absolute_padding_box.top()
            || scrollable_overflow_rect.bottom() > paintable_absolute_padding_box.bottom());
    auto has_scrollable_overflow = has_scrollable_overflow_in_x_axis || has_scrollable_overflow_in_y_axis;
    if (has_scrollable_overflow) {
        auto left = scrollable_overflow_rect.x();
        auto top = scrollable_overflow_rect.y();
        auto right = scrollable_overflow_rect.right();
        auto bottom = scrollable_overflow_rect.bottom();

        if (content_overflows_content_box_in_x_axis) {
            if (overflow_directions.x_positive && content_overflows_content_box_on_right)
                right = max(right, content_overflow_rect.right() + paintable_box.box_model().padding.right);
            else if (!overflow_directions.x_positive)
                left = min(left, content_overflow_rect.x() - paintable_box.box_model().padding.left);
        }

        if (content_overflows_content_box_in_y_axis) {
            if (overflow_directions.y_positive && content_overflows_content_box_on_bottom)
                bottom = max(bottom, content_overflow_rect.bottom() + paintable_box.box_model().padding.bottom);
            else if (!overflow_directions.y_positive)
                top = min(top, content_overflow_rect.y() - paintable_box.box_model().padding.top);
        }

        scrollable_overflow_rect = {
            left,
            top,
            max(right - left, CSSPixels { 0 }),
            max(bottom - top, CSSPixels { 0 }),
        };
    }

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
