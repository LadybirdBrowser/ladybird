/*
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/ScrollableOverflow.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/PaintingRustBridge.h>

namespace Web::Layout {

ContainedBoxesMap collect_scrollable_overflow_contained_boxes(Node const& root, Function<void(Box const&)> box_visitor)
{
    ContainedBoxesMap contained_boxes_map;
    root.for_each_in_inclusive_subtree_of_type<Box>([&](auto& box) {
        if (!box.paintable_box())
            return TraversalDecision::Continue;
        if (box_visitor)
            box_visitor(box);
        if (auto const* containing_block = box.containing_block())
            contained_boxes_map.ensure(containing_block).append(box.template make_weak_ptr<Box const>());
        return TraversalDecision::Continue;
    });
    return contained_boxes_map;
}

struct AxisDirection {
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

// https://drafts.csswg.org/cssom-view/#overflow-directions
PhysicalOverflowDirections physical_overflow_directions(Box const& box)
{
    // A scrolling box of a viewport or element has two overflow directions, which are the block-end and inline-end
    // directions for that viewport or element.

    AxisDirection inline_axis {
        .is_horizontal = inline_axis_is_horizontal(box.writing_mode()),
        .is_reverse = box.inline_axis_is_reverse(),
    };
    AxisDirection block_axis {
        .is_horizontal = !inline_axis.is_horizontal,
        .is_reverse = box.block_axis_is_reverse(),
    };

    if (box.display().is_flex_inside()) {
        auto is_row_layout = box.flex_direction() == CSS::FlexDirection::Row
            || box.flex_direction() == CSS::FlexDirection::RowReverse;
        auto& main_axis = is_row_layout ? inline_axis : block_axis;
        auto& cross_axis = is_row_layout ? block_axis : inline_axis;

        if (box.flex_direction() == CSS::FlexDirection::RowReverse
            || box.flex_direction() == CSS::FlexDirection::ColumnReverse) {
            main_axis.is_reverse = !main_axis.is_reverse;
        }

        // AD-HOC: A legacy webkit box ignores `flex-wrap`, matching other engines.
        if (!box.display().is_webkit_box_inside() && box.flex_wrap() == CSS::FlexWrap::WrapReverse)
            cross_axis.is_reverse = !cross_axis.is_reverse;
    }

    auto horizontal_axis = inline_axis.is_horizontal ? inline_axis : block_axis;
    auto vertical_axis = inline_axis.is_horizontal ? block_axis : inline_axis;
    return {
        .horizontal_axis_is_positive = !horizontal_axis.is_reverse,
        .vertical_axis_is_positive = !vertical_axis.is_reverse,
    };
}

static CSSPixelRect apply_css_transform_to_overflow_rect(Box const& box, CSSPixelRect const& rect, bool has_css_transform)
{
    auto const& paintable_box = *box.paintable_box();
    if (!has_css_transform)
        return rect;
    auto transform_data = Painting::rust_compute_css_transform(paintable_box, 1.0);
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

    auto in_flow_bounds_overflow_content_box_in_horizontal_axis = left < content_box.left() || right > content_box.right();
    if (in_flow_bounds_overflow_content_box_in_horizontal_axis) {
        if (overflow_directions.horizontal_axis_is_positive && right > content_box.right())
            right += padding.right;
        else if (!overflow_directions.horizontal_axis_is_positive)
            left = min(left, padding_box.left()) - padding.left;
    }

    auto in_flow_bounds_overflow_content_box_in_vertical_axis = top < content_box.top() || bottom > content_box.bottom();
    if (in_flow_bounds_overflow_content_box_in_vertical_axis) {
        if (overflow_directions.vertical_axis_is_positive && bottom > content_box.bottom())
            bottom += padding.bottom;
        else if (!overflow_directions.vertical_axis_is_positive)
            top = min(top, padding_box.top()) - padding.top;
    }

    return { left, top, right - left, bottom - top };
}

CSSPixelRect measure_scrollable_overflow(Box const& box, ContainedBoxesMap const& contained_boxes_map)
{
    if (!box.paintable_box())
        return {};

    auto const& paintable_box = *box.paintable_box();

    if (paintable_box.overflow_data().has_value())
        return paintable_box.overflow_data()->scrollable_overflow_rect;

    auto const paintable_absolute_padding_box = paintable_box.absolute_padding_box_rect();
    auto const paintable_absolute_content_box = paintable_box.absolute_rect();

    auto store_overflow_data = [&](Painting::Paintable::OverflowData overflow_data) {
        const_cast<Painting::Paintable&>(paintable_box).set_overflow_data(overflow_data);

        auto rect_relative_to_padding_box = overflow_data.scrollable_overflow_rect;
        rect_relative_to_padding_box.translate_by({ -paintable_absolute_padding_box.x(), -paintable_absolute_padding_box.y() });
        const_cast<Painting::Paintable&>(paintable_box).set_cached_overflow_data({
            .rect_relative_to_padding_box = rect_relative_to_padding_box,
            .has_scrollable_overflow = overflow_data.has_scrollable_overflow,
        });
    };

    if (auto const& cached_overflow = paintable_box.cached_overflow_data(); cached_overflow.has_value()) {
        auto scrollable_overflow_rect = cached_overflow->rect_relative_to_padding_box;
        scrollable_overflow_rect.translate_by(paintable_absolute_padding_box.location());
        const_cast<Painting::Paintable&>(paintable_box).set_overflow_data({
            .scrollable_overflow_rect = scrollable_overflow_rect,
            .has_scrollable_overflow = cached_overflow->has_scrollable_overflow,
        });
        return scrollable_overflow_rect;
    }

    // https://drafts.csswg.org/css-overflow-3/#scrollable-overflow-calculation
    // The scrollable overflow area of a box is the union of:

    // - Its own padding box.
    auto scrollable_overflow_rect = paintable_absolute_padding_box;
    auto in_flow_and_floated_content_bounds = paintable_absolute_content_box;

    // Replaced SVG viewports clip their content
    if (box.is_svg_svg_box()) {
        store_overflow_data({
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
                auto const& style_source = fragment.style_source();
                if (inline_axis_is_horizontal(style_source.writing_mode())) {
                    if (style_source.inline_axis_is_reverse())
                        fragment_rect.set_left(fragment_rect.left() - 1);
                    else
                        fragment_rect.set_right(fragment_rect.right() + 1);
                } else {
                    if (style_source.inline_axis_is_reverse())
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
        for (auto const& weak_child : it->value) {
            auto const* child_ptr = weak_child.ptr();
            if (!child_ptr)
                continue;

            auto const& child = *child_ptr;
            auto child_paintable = child.paintable_box();
            if (!child_paintable || child.containing_block() != &box)
                continue;

            // https://drafts.csswg.org/css-position/#fixed-positioning-containing-block
            // [..] As a result, parts of fixed-positioned boxes that extend outside the layout viewport/page area
            //      cannot be scrolled to and will not print.
            // FIXME: Properly establish the fixed positioning containing block for `position: fixed`
            if (child.is_fixed_position())
                continue;

            auto const child_has_css_transform = child_paintable->has_css_transform();
            if (child.position() == CSS::Positioning::Static && child.display().is_inline_outside() && !child.is_floating() && !child_has_css_transform) {
                if (auto const& cached_overflow = child_paintable->cached_overflow_data(); cached_overflow.has_value()) {
                    auto const& border = child_paintable->box_model().border;
                    auto const has_border = border.top != 0 || border.right != 0 || border.bottom != 0 || border.left != 0;
                    if (!has_border) {
                        auto const& padding = child_paintable->box_model().padding;
                        CSSPixelRect content_box_relative_to_padding_box {
                            { padding.left, padding.top },
                            {
                                child_paintable->content_width(),
                                child_paintable->content_height(),
                            },
                        };
                        // The committed line fragment already contributes this content box. A box with no border whose
                        // cached overflow fits inside the content box cannot expand its containing block's overflow.
                        if (content_box_relative_to_padding_box.contains(cached_overflow->rect_relative_to_padding_box))
                            continue;
                    }
                }
            }

            auto untransformed_child_border_box = child_paintable->absolute_border_box_rect();
            auto child_border_box = apply_css_transform_to_overflow_rect(child, untransformed_child_border_box, child_has_css_transform);

            // NOTE: Only boxes that are not wholly in the unreachable scrollable overflow region contribute.
            auto wholly_in_unreachable_horizontal_axis = overflow_directions.horizontal_axis_is_positive
                ? child_border_box.right() < paintable_absolute_padding_box.x()
                : child_border_box.x() > paintable_absolute_padding_box.right();
            auto wholly_in_unreachable_vertical_axis = overflow_directions.vertical_axis_is_positive
                ? child_border_box.bottom() < paintable_absolute_padding_box.y()
                : child_border_box.y() > paintable_absolute_padding_box.bottom();
            if (wholly_in_unreachable_horizontal_axis || wholly_in_unreachable_vertical_axis)
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

            auto const child_overflow_x = child.overflow_x();
            auto const child_overflow_y = child.overflow_y();
            if (child_overflow_x == CSS::Overflow::Visible || child_overflow_y == CSS::Overflow::Visible) {
                auto child_scrollable_overflow = apply_css_transform_to_overflow_rect(child, measure_scrollable_overflow(child, contained_boxes_map), child_has_css_transform);
                if (!child_scrollable_overflow.is_empty()) {
                    if (child_overflow_x == CSS::Overflow::Visible) {
                        scrollable_overflow_rect.unite_horizontally(child_scrollable_overflow);
                    }
                    if (child_overflow_y == CSS::Overflow::Visible) {
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
    auto left = overflow_directions.horizontal_axis_is_positive ? max(scrollable_overflow_rect.x(), paintable_absolute_padding_box.x()) : scrollable_overflow_rect.x();
    auto top = overflow_directions.vertical_axis_is_positive ? max(scrollable_overflow_rect.y(), paintable_absolute_padding_box.y()) : scrollable_overflow_rect.y();
    auto right = overflow_directions.horizontal_axis_is_positive ? scrollable_overflow_rect.right() : min(scrollable_overflow_rect.right(), paintable_absolute_padding_box.right());
    auto bottom = overflow_directions.vertical_axis_is_positive ? scrollable_overflow_rect.bottom() : min(scrollable_overflow_rect.bottom(), paintable_absolute_padding_box.bottom());
    if (left != scrollable_overflow_rect.x() || top != scrollable_overflow_rect.y() || right != scrollable_overflow_rect.right() || bottom != scrollable_overflow_rect.bottom()) {
        scrollable_overflow_rect = {
            left,
            top,
            max(right - left, CSSPixels { 0 }),
            max(bottom - top, CSSPixels { 0 }),
        };
        has_scrollable_overflow = !paintable_absolute_padding_box.contains(scrollable_overflow_rect) && box.is_scroll_container();
    }

    store_overflow_data({
        .scrollable_overflow_rect = scrollable_overflow_rect,
        .has_scrollable_overflow = has_scrollable_overflow,
    });

    return scrollable_overflow_rect;
}

}
