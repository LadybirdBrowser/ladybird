/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/DOM/DOMEventListener.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/TraversalDecision.h>

namespace Web::Compositor {

static Gfx::FloatPoint css_point_to_device_point(CSSPixelPoint point, double device_pixels_per_css_pixel)
{
    auto scale = static_cast<float>(device_pixels_per_css_pixel);
    return { point.x().to_float() * scale, point.y().to_float() * scale };
}

static Gfx::FloatSize css_size_to_device_size(CSSPixelSize size, double device_pixels_per_css_pixel)
{
    auto scale = static_cast<float>(device_pixels_per_css_pixel);
    return { size.width().to_float() * scale, size.height().to_float() * scale };
}

static Gfx::FloatRect css_rect_to_device_rect(CSSPixelRect rect, double device_pixels_per_css_pixel)
{
    return { css_point_to_device_point(rect.location(), device_pixels_per_css_pixel), css_size_to_device_size(rect.size(), device_pixels_per_css_pixel) };
}

static Optional<float> css_inset_to_device_inset(Optional<CSSPixels> inset, double device_pixels_per_css_pixel)
{
    if (!inset.has_value())
        return {};
    return inset->to_float() * static_cast<float>(device_pixels_per_css_pixel);
}

static CSSPixelPoint maximum_scroll_offset_for(Painting::PaintableBox const& paintable_box)
{
    CSSPixelPoint max_scroll_offset;
    auto scrollable_overflow_rect = paintable_box.scrollable_overflow_rect();
    if (!scrollable_overflow_rect.has_value())
        return max_scroll_offset;

    auto scrollport_rect = paintable_box.absolute_padding_box_rect();
    max_scroll_offset.set_x(max(CSSPixels(0), scrollable_overflow_rect->width() - scrollport_rect.width()));
    max_scroll_offset.set_y(max(CSSPixels(0), scrollable_overflow_rect->height() - scrollport_rect.height()));
    return max_scroll_offset;
}

static AsyncScrollNodeID scroll_node_id_for(UniqueNodeID document_id, Painting::ScrollFrameIndex scroll_frame_index)
{
    return { .document_id = document_id, .scroll_frame_index = scroll_frame_index };
}

static Optional<AsyncScrollNodeID> scroll_node_id_for_scroll_frame_index(AsyncScrollingState const& async_scrolling_state, Painting::ScrollFrameIndex scroll_frame_index)
{
    for (auto const& scroll_node : async_scrolling_state.scroll_nodes) {
        if (scroll_node.node_id.scroll_frame_index == scroll_frame_index)
            return scroll_node.node_id;
    }
    return {};
}

static Optional<AsyncScrollNodeID> parent_scroll_node_id_for(AsyncScrollingState const& async_scrolling_state, Painting::ScrollState const& scroll_state, Painting::ScrollFrameIndex parent_scroll_frame_index)
{
    for (auto scroll_frame_index = parent_scroll_frame_index; scroll_frame_index.value(); scroll_frame_index = scroll_state.frame_at(scroll_frame_index).parent_index()) {
        if (auto node_id = scroll_node_id_for_scroll_frame_index(async_scrolling_state, scroll_frame_index); node_id.has_value())
            return node_id;
    }
    return {};
}

static void collect_viewport_scrollbar_state(AsyncScrollingState& async_scrolling_state, HTML::Navigable& navigable, Painting::PaintableBox const& paintable_box)
{
    if (!paintable_box.is_viewport_paintable())
        return;
    if (!Painting::should_paint_viewport_scrollbars())
        return;
    if (paintable_box.computed_values().scrollbar_width() == CSS::ScrollbarWidth::None)
        return;

    auto scrollbar_colors = paintable_box.computed_values().scrollbar_color();
    auto metrics = paintable_box.document().page().chrome_metrics();

    for (auto direction : { Painting::PaintableBox::ScrollDirection::Vertical, Painting::PaintableBox::ScrollDirection::Horizontal }) {
        auto scrollbar_data = paintable_box.compute_scrollbar_data(direction, metrics, nullptr, Painting::PaintableBox::ScrollbarSizing::Regular);
        if (!scrollbar_data.has_value())
            continue;
        auto expanded_scrollbar_data = paintable_box.compute_scrollbar_data(direction, metrics, nullptr, Painting::PaintableBox::ScrollbarSizing::Enlarged);
        VERIFY(expanded_scrollbar_data.has_value());

        auto gutter_rect = navigable.page().css_to_device_rect(scrollbar_data->gutter_rect).to_type<int>();
        auto max_scroll_offset = css_point_to_device_point(maximum_scroll_offset_for(paintable_box), navigable.page().client().device_pixels_per_css_pixel());
        auto orientation = direction == Painting::PaintableBox::ScrollDirection::Horizontal ? Gfx::Orientation::Horizontal : Gfx::Orientation::Vertical;
        auto thumb_color = scrollbar_colors.thumb_color;
        if (gutter_rect.is_empty() && thumb_color == CSS::InitialValues::scrollbar_color().thumb_color)
            thumb_color = thumb_color.with_alpha(128);

        async_scrolling_state.viewport_scrollbars.append({
            .scroll_node_id = scroll_node_id_for(paintable_box.document().unique_id(), paintable_box.own_scroll_frame_index()),
            .scroll_frame_index = paintable_box.own_scroll_frame_index(),
            .gutter_rect = gutter_rect,
            .thumb_rect = navigable.page().css_to_device_rect(scrollbar_data->thumb_rect).to_type<int>(),
            .expanded_gutter_rect = navigable.page().css_to_device_rect(expanded_scrollbar_data->gutter_rect).to_type<int>(),
            .expanded_thumb_rect = navigable.page().css_to_device_rect(expanded_scrollbar_data->thumb_rect).to_type<int>(),
            .scroll_size = scrollbar_data->thumb_travel_to_scroll_ratio.to_double(),
            .expanded_scroll_size = expanded_scrollbar_data->thumb_travel_to_scroll_ratio.to_double(),
            .max_scroll_offset = max_scroll_offset.primary_offset_for_orientation(orientation),
            .thumb_color = thumb_color,
            .track_color = scrollbar_colors.track_color,
            .vertical = direction == Painting::PaintableBox::ScrollDirection::Vertical,
        });
    }
}

static bool has_blocking_wheel_event_listener(DOM::EventTarget& event_target)
{
    for (auto listener : event_target.event_listener_list()) {
        if (AK::first_is_one_of(listener->type, "wheel"sv, "mousewheel"sv) && listener->passive != true)
            return true;
    }
    return false;
}

static bool is_root_wheel_event_target(DOM::Node& node)
{
    auto& document = node.document();
    return &node == document.document_element() || &node == document.body();
}

static void collect_root_blocking_wheel_event_regions(AsyncScrollingState& async_scrolling_state, DOM::Document& document)
{
    if (auto window = document.navigable() ? document.navigable()->active_window() : nullptr; window && has_blocking_wheel_event_listener(*window)) {
        async_scrolling_state.has_blocking_wheel_event_listeners = true;
        async_scrolling_state.has_blocking_wheel_event_region_covering_viewport = true;
    }
    if (has_blocking_wheel_event_listener(document)) {
        async_scrolling_state.has_blocking_wheel_event_listeners = true;
        async_scrolling_state.has_blocking_wheel_event_region_covering_viewport = true;
    }
    if (auto* document_element = document.document_element(); document_element && has_blocking_wheel_event_listener(*document_element)) {
        async_scrolling_state.has_blocking_wheel_event_listeners = true;
        async_scrolling_state.has_blocking_wheel_event_region_covering_viewport = true;
    }
    if (auto* body = document.body(); body && has_blocking_wheel_event_listener(*body)) {
        async_scrolling_state.has_blocking_wheel_event_listeners = true;
        async_scrolling_state.has_blocking_wheel_event_region_covering_viewport = true;
    }
}

AsyncScrollingState collect_async_scrolling_state(HTML::Navigable& navigable, Painting::ViewportPaintable& document_paintable, Gfx::IntRect viewport_rect)
{
    auto device_pixels_per_css_pixel = navigable.page().client().device_pixels_per_css_pixel();
    auto const& scroll_state = document_paintable.scroll_state();
    auto document_id = document_paintable.document().unique_id();
    AsyncScrollingState async_scrolling_state;
    Vector<Painting::ScrollFrameIndex> parent_scroll_frame_indices;
    async_scrolling_state.viewport_rect = viewport_rect;
    async_scrolling_state.wheel_event_listener_state_generation = navigable.page().wheel_event_listener_state_generation();
    collect_root_blocking_wheel_event_regions(async_scrolling_state, document_paintable.document());

    document_paintable.for_each_in_inclusive_subtree_of_type<Painting::PaintableBox>([&](auto& paintable_box) {
        if (!async_scrolling_state.has_blocking_wheel_event_region_covering_viewport) {
            if (auto node = paintable_box.dom_node(); node && !is_root_wheel_event_target(*node) && has_blocking_wheel_event_listener(*node)) {
                async_scrolling_state.has_blocking_wheel_event_listeners = true;
                async_scrolling_state.blocking_wheel_event_regions.append({
                    .visual_context_index = paintable_box.accumulated_visual_context_index(),
                    .rect = css_rect_to_device_rect(paintable_box.absolute_united_border_box_rect(), device_pixels_per_css_pixel),
                });
            }
        }

        if (auto node = paintable_box.dom_node(); node && node->is_navigable_container() && as<HTML::NavigableContainer>(*node).content_navigable()) {
            async_scrolling_state.main_thread_wheel_event_regions.append({
                .visual_context_index = paintable_box.accumulated_visual_context_index(),
                .rect = css_rect_to_device_rect(paintable_box.absolute_united_border_box_rect(), device_pixels_per_css_pixel),
            });
        }

        auto sticky_frame_index = paintable_box.enclosing_scroll_frame_index();
        if (paintable_box.is_sticky_position() && sticky_frame_index.value()) {
            auto const& frame = scroll_state.frame_at(sticky_frame_index);
            if (frame.is_sticky() && frame.has_sticky_constraints()) {
                auto const& constraints = frame.sticky_constraints();
                auto const& insets = constraints.insets;
                async_scrolling_state.sticky_areas.append({
                    .document_id = document_id,
                    .scroll_frame_index = sticky_frame_index,
                    .parent_scroll_frame_index = frame.parent_index(),
                    .nearest_scrolling_ancestor_index = scroll_state.nearest_scrolling_ancestor(sticky_frame_index),
                    .position_relative_to_scroll_ancestor = css_point_to_device_point(constraints.position_relative_to_scroll_ancestor, device_pixels_per_css_pixel),
                    .border_box_size = css_size_to_device_size(constraints.border_box_size, device_pixels_per_css_pixel),
                    .scrollport_size = css_size_to_device_size(constraints.scrollport_size, device_pixels_per_css_pixel),
                    .containing_block_region = css_rect_to_device_rect(constraints.containing_block_region, device_pixels_per_css_pixel),
                    .needs_parent_offset_adjustment = constraints.needs_parent_offset_adjustment,
                    .inset_top = css_inset_to_device_inset(insets.top, device_pixels_per_css_pixel),
                    .inset_right = css_inset_to_device_inset(insets.right, device_pixels_per_css_pixel),
                    .inset_bottom = css_inset_to_device_inset(insets.bottom, device_pixels_per_css_pixel),
                    .inset_left = css_inset_to_device_inset(insets.left, device_pixels_per_css_pixel),
                });
            }
        }

        auto scroll_frame_index = paintable_box.own_scroll_frame_index();
        if (scroll_frame_index.value() && paintable_box.could_be_scrolled_by_wheel_event()) {
            auto parent_scroll_frame_index = scroll_state.frame_at(scroll_frame_index).parent_index();
            async_scrolling_state.scroll_nodes.append({
                .node_id = scroll_node_id_for(document_id, scroll_frame_index),
                .parent_node_id = {},
                .hit_test_visual_context_index = paintable_box.accumulated_visual_context_index(),
                .scrollport_rect = navigable.page().css_to_device_rect(paintable_box.absolute_padding_box_rect()).template to_type<int>(),
                .scroll_offset = css_point_to_device_point(paintable_box.scroll_offset(), device_pixels_per_css_pixel),
                .max_scroll_offset = css_point_to_device_point(maximum_scroll_offset_for(paintable_box), device_pixels_per_css_pixel),
                .is_viewport = paintable_box.is_viewport_paintable(),
            });
            parent_scroll_frame_indices.append(parent_scroll_frame_index);
        }

        collect_viewport_scrollbar_state(async_scrolling_state, navigable, paintable_box);

        return TraversalDecision::Continue;
    });

    VERIFY(parent_scroll_frame_indices.size() == async_scrolling_state.scroll_nodes.size());
    for (size_t i = 0; i < async_scrolling_state.scroll_nodes.size(); ++i)
        async_scrolling_state.scroll_nodes[i].parent_node_id = parent_scroll_node_id_for(async_scrolling_state, scroll_state, parent_scroll_frame_indices[i]);

    async_scrolling_state.blocking_wheel_event_regions_are_current = async_scrolling_state.has_blocking_wheel_event_listeners;
    return async_scrolling_state;
}

WheelRoutingAdmission wheel_routing_admission_for(AsyncScrollingState const& state)
{
    if (state.has_blocking_wheel_event_listeners)
        return WheelRoutingAdmission::BlockingWheelEventListeners;

    bool found_viewport_node = false;
    for (auto const& node : state.scroll_nodes) {
        if (node.is_viewport) {
            found_viewport_node = true;
            break;
        }
    }
    if (!found_viewport_node)
        return WheelRoutingAdmission::NoViewportScrollNode;
    return WheelRoutingAdmission::Accepted;
}

StringView wheel_routing_admission_to_string(WheelRoutingAdmission admission)
{
    switch (admission) {
    case WheelRoutingAdmission::Accepted:
        return "accepted"sv;
    case WheelRoutingAdmission::NoAsyncScrollingState:
        return "no async scrolling state"sv;
    case WheelRoutingAdmission::BlockingWheelEventListeners:
        return "blocking wheel event listeners"sv;
    case WheelRoutingAdmission::NoViewportScrollNode:
        return "no viewport scroll node"sv;
    case WheelRoutingAdmission::StaleWheelEventListeners:
        return "stale wheel event listeners"sv;
    }
    VERIFY_NOT_REACHED();
}

bool blocks_wheel_event_at_position(AsyncScrollingState const& async_scrolling_state, RefPtr<Painting::DisplayList> const& display_list, Painting::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position)
{
    if (async_scrolling_state.has_blocking_wheel_event_region_covering_viewport)
        return true;

    // If a caller knows blocking wheel listeners exist but cannot provide a display list for visual-context hit
    // testing, async scrolling must fail closed. Sending the input to the main thread is slower, but it preserves
    // cancelability.
    if (!display_list)
        return async_scrolling_state.has_blocking_wheel_event_listeners;

    auto const& visual_context_tree = display_list->visual_context_tree();
    for (auto const& region : async_scrolling_state.blocking_wheel_event_regions) {
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(region.visual_context_index, position, scroll_state_snapshot);
        if (position_in_context.has_value() && region.rect.contains(*position_in_context))
            return true;
    }
    return false;
}

bool requires_main_thread_wheel_event_at_position(AsyncScrollingState const& async_scrolling_state, RefPtr<Painting::DisplayList> const& display_list, Painting::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position)
{
    if (async_scrolling_state.main_thread_wheel_event_regions.is_empty())
        return false;

    if (!display_list)
        return true;

    auto const& visual_context_tree = display_list->visual_context_tree();
    for (auto const& region : async_scrolling_state.main_thread_wheel_event_regions) {
        auto position_in_context = visual_context_tree.transform_point_for_hit_test(region.visual_context_index, position, scroll_state_snapshot);
        if (position_in_context.has_value() && region.rect.contains(*position_in_context))
            return true;
    }
    return false;
}

static bool scroll_node_can_scroll_by_delta(AsyncScrollNode const& node, Gfx::FloatPoint delta)
{
    if (delta.x() < 0 && node.scroll_offset.x() > 0)
        return true;
    if (delta.x() > 0 && node.scroll_offset.x() < node.max_scroll_offset.x())
        return true;
    if (delta.y() < 0 && node.scroll_offset.y() > 0)
        return true;
    if (delta.y() > 0 && node.scroll_offset.y() < node.max_scroll_offset.y())
        return true;
    return false;
}

static bool has_scroll_node_at_position(AsyncScrollingState const& async_scrolling_state, RefPtr<Painting::DisplayList> const& display_list, Painting::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position, Gfx::FloatPoint delta)
{
    if (!display_list)
        return false;

    auto const& visual_context_tree = display_list->visual_context_tree();
    for (auto const& node : async_scrolling_state.scroll_nodes) {
        if (!scroll_node_can_scroll_by_delta(node, delta))
            continue;

        auto scrollport_rect = node.is_viewport
            ? Gfx::FloatRect { {}, async_scrolling_state.viewport_rect.size().to_type<float>() }
            : visual_context_tree.transform_rect_to_viewport(node.hit_test_visual_context_index, node.scrollport_rect.to_type<float>(), scroll_state_snapshot);
        if (scrollport_rect.contains(position))
            return true;
    }
    return false;
}

WheelScrollAdmission admit_wheel_scroll(AsyncScrollingState const& async_scrolling_state, RefPtr<Painting::DisplayList> const& display_list, Painting::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position, Gfx::FloatPoint delta, bool has_blocking_wheel_event_listeners, bool blocking_wheel_event_regions_are_current)
{
    if (requires_main_thread_wheel_event_at_position(async_scrolling_state, display_list, scroll_state_snapshot, position))
        return WheelScrollAdmission::BlockedByMainThreadRegion;

    // Async scrolling may only start when the snapshot can prove that the wheel event cannot be canceled by script
    // at this position. Stale or missing blocker information sends the event back to the main thread.
    if (has_blocking_wheel_event_listeners) {
        if (!blocking_wheel_event_regions_are_current)
            return WheelScrollAdmission::StaleBlockingWheelEventRegions;
        if (blocks_wheel_event_at_position(async_scrolling_state, display_list, scroll_state_snapshot, position))
            return WheelScrollAdmission::BlockedByWheelEventRegion;
    }

    if (!has_scroll_node_at_position(async_scrolling_state, display_list, scroll_state_snapshot, position, delta))
        return WheelScrollAdmission::NoScrollableTarget;
    return WheelScrollAdmission::Accepted;
}

}
