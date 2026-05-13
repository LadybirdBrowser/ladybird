/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/DOM/DOMEventListener.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ScrollState.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/PixelUnits.h>

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

static AsyncScrollNodeID scroll_node_id_for(UniqueNodeID document_id, Painting::ScrollFrameIndex scroll_frame_index)
{
    return { .document_id = document_id, .scroll_frame_index = scroll_frame_index };
}

static bool is_nested_navigable_container(Painting::PaintableBox const& paintable_box)
{
    auto node = paintable_box.dom_node();
    return node && node->is_navigable_container() && as<HTML::NavigableContainer const>(*node).content_navigable();
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

static void record_scroll_node(Painting::PaintableBox const& paintable_box, DisplayListRecordingContext& context)
{
    auto parent_scroll_frame_index = Painting::ScrollFrameIndex {};
    if (auto scrollable_ancestor = paintable_box.nearest_scrollable_ancestor())
        parent_scroll_frame_index = scrollable_ancestor->own_scroll_frame_index();

    auto scrollport_rect = paintable_box.is_viewport_paintable()
        ? Gfx::IntRect { {}, context.device_viewport_rect().size().to_type<int>() }
        : context.rounded_device_rect(paintable_box.absolute_padding_box_rect()).to_type<int>();

    auto& recorder = context.display_list_recorder();
    recorder.compositor_scroll_node({
        .document_id = paintable_box.document().unique_id(),
        .scroll_frame_index = paintable_box.own_scroll_frame_index(),
        .parent_scroll_frame_index = parent_scroll_frame_index,
        .scrollport_rect = scrollport_rect,
        .max_scroll_offset = css_point_to_device_point(maximum_scroll_offset_for(paintable_box), context.device_pixels_per_css_pixel()),
        .is_viewport = paintable_box.is_viewport_paintable(),
    });
}

static void record_main_thread_wheel_event_region(Painting::PaintableBox const& paintable_box, DisplayListRecordingContext& context)
{
    auto rect = css_rect_to_device_rect(paintable_box.absolute_united_border_box_rect(), context.device_pixels_per_css_pixel());
    if (rect.is_empty())
        return;

    context.display_list_recorder().compositor_main_thread_wheel_event_region({
        .rect = rect,
    });
}

static void record_viewport_scrollbar_state(Painting::PaintableBox const& paintable_box, DisplayListRecordingContext& context)
{
    if (!paintable_box.is_viewport_paintable())
        return;
    if (!paintable_box.document().page().async_scrolling_enabled())
        return;
    if (!Painting::should_paint_viewport_scrollbars())
        return;
    if (paintable_box.computed_values().scrollbar_width() == CSS::ScrollbarWidth::None)
        return;

    auto scrollbar_colors = paintable_box.computed_values().scrollbar_color();
    auto const& metrics = context.chrome_metrics();

    for (auto direction : { Painting::PaintableBox::ScrollDirection::Vertical, Painting::PaintableBox::ScrollDirection::Horizontal }) {
        auto scrollbar_data = paintable_box.compute_scrollbar_data(direction, metrics, nullptr, Painting::PaintableBox::ScrollbarSizing::Regular);
        if (!scrollbar_data.has_value())
            continue;
        auto expanded_scrollbar_data = paintable_box.compute_scrollbar_data(direction, metrics, nullptr, Painting::PaintableBox::ScrollbarSizing::Enlarged);
        VERIFY(expanded_scrollbar_data.has_value());

        auto gutter_rect = context.rounded_device_rect(scrollbar_data->gutter_rect).to_type<int>();
        auto max_scroll_offset = css_point_to_device_point(maximum_scroll_offset_for(paintable_box), context.device_pixels_per_css_pixel());
        auto orientation = direction == Painting::PaintableBox::ScrollDirection::Horizontal ? Gfx::Orientation::Horizontal : Gfx::Orientation::Vertical;
        auto thumb_color = scrollbar_colors.thumb_color;
        if (gutter_rect.is_empty() && thumb_color == CSS::InitialValues::scrollbar_color().thumb_color)
            thumb_color = thumb_color.with_alpha(128);

        context.display_list_recorder().compositor_viewport_scrollbar({
            .document_id = paintable_box.document().unique_id(),
            .scroll_frame_index = paintable_box.own_scroll_frame_index(),
            .gutter_rect = gutter_rect,
            .thumb_rect = context.rounded_device_rect(scrollbar_data->thumb_rect).to_type<int>(),
            .expanded_gutter_rect = context.rounded_device_rect(expanded_scrollbar_data->gutter_rect).to_type<int>(),
            .expanded_thumb_rect = context.rounded_device_rect(expanded_scrollbar_data->thumb_rect).to_type<int>(),
            .scroll_size = scrollbar_data->thumb_travel_to_scroll_ratio.to_double(),
            .expanded_scroll_size = expanded_scrollbar_data->thumb_travel_to_scroll_ratio.to_double(),
            .max_scroll_offset = max_scroll_offset.primary_offset_for_orientation(orientation),
            .thumb_color = thumb_color,
            .track_color = scrollbar_colors.track_color,
            .vertical = direction == Painting::PaintableBox::ScrollDirection::Vertical,
        });
    }
}

static bool has_blocking_wheel_event_listener(DOM::EventTarget const& event_target)
{
    for (auto listener : event_target.event_listener_list()) {
        if (AK::first_is_one_of(listener->type, "wheel"sv, "mousewheel"sv) && listener->passive != true)
            return true;
    }
    return false;
}

static bool is_root_wheel_event_target(DOM::Node const& node)
{
    auto& document = node.document();
    return &node == document.document_element() || &node == document.body();
}

static void collect_root_blocking_wheel_event_regions(AsyncScrollingState& async_scrolling_state, DOM::Document& document)
{
    DOM::EventTarget* roots[] = {
        document.navigable() ? document.navigable()->active_window() : nullptr,
        &document,
        document.document_element(),
        document.body(),
    };
    for (auto* target : roots) {
        if (target && has_blocking_wheel_event_listener(*target)) {
            async_scrolling_state.has_blocking_wheel_event_listeners = true;
            async_scrolling_state.has_blocking_wheel_event_region_covering_viewport = true;
            return;
        }
    }
}

void initialize_async_scrolling_metadata_recording(DisplayListRecordingContext& context, Painting::ViewportPaintable& document_paintable)
{
    AsyncScrollingState async_scrolling_state;
    collect_root_blocking_wheel_event_regions(async_scrolling_state, document_paintable.document());
    context.set_async_scrolling_metadata_context(
        document_paintable.document().unique_id(),
        document_paintable.scroll_state(),
        async_scrolling_state.has_blocking_wheel_event_listeners,
        async_scrolling_state.has_blocking_wheel_event_region_covering_viewport);
}

void record_async_scrolling_metadata_for_paintable(Painting::PaintableBox const& paintable_box, DisplayListRecordingContext& context)
{
    if (!context.is_recording_async_scrolling_metadata())
        return;

    auto device_pixels_per_css_pixel = context.device_pixels_per_css_pixel();
    auto& recorder = context.display_list_recorder();

    if (!context.has_blocking_wheel_event_region_covering_viewport()) {
        if (auto node = paintable_box.dom_node(); node && !is_root_wheel_event_target(*node) && has_blocking_wheel_event_listener(*node)) {
            context.set_has_blocking_wheel_event_listeners(true);
            recorder.compositor_blocking_wheel_event_region({
                .rect = css_rect_to_device_rect(paintable_box.absolute_united_border_box_rect(), device_pixels_per_css_pixel),
            });
        }
    }

    if (is_nested_navigable_container(paintable_box)) {
        record_main_thread_wheel_event_region(paintable_box, context);
    } else if (paintable_box.own_scroll_frame_index().value() && paintable_box.could_be_scrolled_by_wheel_event()) {
        record_scroll_node(paintable_box, context);
    }
    record_viewport_scrollbar_state(paintable_box, context);

    auto const& scroll_state = context.async_scrolling_scroll_state();
    auto sticky_frame_index = paintable_box.enclosing_scroll_frame_index();
    if (paintable_box.is_sticky_position() && sticky_frame_index.value()) {
        auto const& frame = scroll_state.frame_at(sticky_frame_index);
        if (frame.is_sticky() && frame.has_sticky_constraints()) {
            auto const& constraints = frame.sticky_constraints();
            auto const& insets = constraints.insets;
            recorder.compositor_sticky_area({
                .document_id = context.async_scrolling_document_id(),
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
}

void finalize_async_scrolling_metadata_recording(DisplayListRecordingContext& context, HTML::Navigable& navigable, Gfx::IntRect viewport_rect)
{
    if (!context.is_recording_async_scrolling_metadata())
        return;

    context.display_list_recorder().set_async_scrolling_metadata({
        .viewport_rect = viewport_rect,
        .wheel_event_listener_state_generation = navigable.page().wheel_event_listener_state_generation(),
        .has_blocking_wheel_event_listeners = context.has_blocking_wheel_event_listeners(),
        .has_blocking_wheel_event_region_covering_viewport = context.has_blocking_wheel_event_region_covering_viewport(),
    });
}

AsyncScrollingState async_scrolling_state_from_display_list(Painting::DisplayList const& display_list)
{
    AsyncScrollingState async_scrolling_state;
    Vector<Painting::ScrollFrameIndex> parent_scroll_frame_indices;

    if (auto const& metadata = display_list.async_scrolling_metadata(); metadata.has_value()) {
        async_scrolling_state.viewport_rect = metadata->viewport_rect;
        async_scrolling_state.wheel_event_listener_state_generation = metadata->wheel_event_listener_state_generation;
        async_scrolling_state.has_blocking_wheel_event_listeners = metadata->has_blocking_wheel_event_listeners;
        async_scrolling_state.has_blocking_wheel_event_region_covering_viewport = metadata->has_blocking_wheel_event_region_covering_viewport;
    }

    display_list.for_each_command_header([&](Painting::DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        switch (header.type) {
        case Painting::DisplayListCommandType::CompositorBlockingWheelEventRegion: {
            auto command = Painting::read_display_list_command_payload<Painting::CompositorBlockingWheelEventRegion>(payload);
            async_scrolling_state.has_blocking_wheel_event_listeners = true;
            async_scrolling_state.blocking_wheel_event_regions.append({
                .visual_context_index = header.context_index,
                .rect = command.rect,
            });
            break;
        }
        case Painting::DisplayListCommandType::CompositorStickyArea: {
            auto command = Painting::read_display_list_command_payload<Painting::CompositorStickyArea>(payload);
            async_scrolling_state.sticky_areas.append({
                .document_id = command.document_id,
                .scroll_frame_index = command.scroll_frame_index,
                .parent_scroll_frame_index = command.parent_scroll_frame_index,
                .nearest_scrolling_ancestor_index = command.nearest_scrolling_ancestor_index,
                .position_relative_to_scroll_ancestor = command.position_relative_to_scroll_ancestor,
                .border_box_size = command.border_box_size,
                .scrollport_size = command.scrollport_size,
                .containing_block_region = command.containing_block_region,
                .needs_parent_offset_adjustment = command.needs_parent_offset_adjustment,
                .inset_top = command.inset_top,
                .inset_right = command.inset_right,
                .inset_bottom = command.inset_bottom,
                .inset_left = command.inset_left,
            });
            break;
        }
        case Painting::DisplayListCommandType::CompositorScrollNode: {
            auto command = Painting::read_display_list_command_payload<Painting::CompositorScrollNode>(payload);
            async_scrolling_state.scroll_nodes.append({
                .node_id = scroll_node_id_for(command.document_id, command.scroll_frame_index),
                .parent_node_id = {},
                .hit_test_visual_context_index = header.context_index,
                .scrollport_rect = command.scrollport_rect,
                .max_scroll_offset = command.max_scroll_offset,
                .is_viewport = command.is_viewport,
            });
            parent_scroll_frame_indices.append(command.parent_scroll_frame_index);
            break;
        }
        case Painting::DisplayListCommandType::CompositorMainThreadWheelEventRegion: {
            auto command = Painting::read_display_list_command_payload<Painting::CompositorMainThreadWheelEventRegion>(payload);
            async_scrolling_state.main_thread_wheel_event_regions.append({
                .visual_context_index = header.context_index,
                .rect = command.rect,
            });
            break;
        }
        case Painting::DisplayListCommandType::CompositorViewportScrollbar: {
            auto command = Painting::read_display_list_command_payload<Painting::CompositorViewportScrollbar>(payload);
            async_scrolling_state.viewport_scrollbars.append({
                .scroll_node_id = scroll_node_id_for(command.document_id, command.scroll_frame_index),
                .scroll_frame_index = command.scroll_frame_index,
                .gutter_rect = command.gutter_rect,
                .thumb_rect = command.thumb_rect,
                .expanded_gutter_rect = command.expanded_gutter_rect,
                .expanded_thumb_rect = command.expanded_thumb_rect,
                .scroll_size = command.scroll_size,
                .expanded_scroll_size = command.expanded_scroll_size,
                .max_scroll_offset = command.max_scroll_offset,
                .thumb_color = command.thumb_color,
                .track_color = command.track_color,
                .vertical = command.vertical,
            });
            break;
        }
        default:
            break;
        }
    });

    VERIFY(parent_scroll_frame_indices.size() == async_scrolling_state.scroll_nodes.size());
    for (size_t i = 0; i < async_scrolling_state.scroll_nodes.size(); ++i) {
        auto parent_scroll_frame_index = parent_scroll_frame_indices[i];
        if (parent_scroll_frame_index.value())
            async_scrolling_state.scroll_nodes[i].parent_node_id = scroll_node_id_for(async_scrolling_state.scroll_nodes[i].node_id.document_id, parent_scroll_frame_index);
    }
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

static WheelHitTestResult hit_test_scroll_node_at_position(AsyncScrollingState const& async_scrolling_state, RefPtr<Painting::DisplayList> const& display_list, Painting::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position, Gfx::FloatPoint delta)
{
    if (!display_list)
        return {};

    AsyncScrollTree scroll_tree;
    auto async_scrolling_state_copy = async_scrolling_state;
    scroll_tree.set_state(move(async_scrolling_state_copy));
    scroll_tree.rebuild_wheel_scroll_targets(display_list, scroll_state_snapshot);
    return scroll_tree.hit_test_scroll_node_for_wheel(position, delta);
}

WheelScrollAdmission admit_wheel_scroll(AsyncScrollingState const& async_scrolling_state, RefPtr<Painting::DisplayList> const& display_list, Painting::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position, Gfx::FloatPoint delta, bool blocking_wheel_event_regions_are_current)
{
    auto hit_test_result = hit_test_scroll_node_at_position(async_scrolling_state, display_list, scroll_state_snapshot, position, delta);
    if (hit_test_result.blocked_by_main_thread_region)
        return WheelScrollAdmission::BlockedByMainThreadRegion;

    // Async scrolling may only start when the snapshot can prove that the wheel event cannot be canceled by script
    // at this position. Stale or missing blocker information sends the event back to the main thread.
    if (async_scrolling_state.has_blocking_wheel_event_listeners) {
        if (!blocking_wheel_event_regions_are_current)
            return WheelScrollAdmission::StaleBlockingWheelEventRegions;
        if (blocks_wheel_event_at_position(async_scrolling_state, display_list, scroll_state_snapshot, position))
            return WheelScrollAdmission::BlockedByWheelEventRegion;
    }

    if (!hit_test_result.node_id.has_value())
        return WheelScrollAdmission::NoScrollableTarget;
    return WheelScrollAdmission::Accepted;
}

}
