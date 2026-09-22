/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/Scrolling/AsyncScrollTree.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibCompositing/Scrolling/ScrollState.h>

namespace Compositing {

static AsyncScrollNodeID scroll_node_id_for(UniqueNodeID document_id, Compositing::SpatialNodeIndex scroll_node_index)
{
    return { .document_id = document_id, .scroll_node_index = scroll_node_index };
}

AsyncScrollNodeKind async_scroll_node_kind_for(Compositing::CompositorScrollNodeKind kind)
{
    switch (kind) {
    case Compositing::CompositorScrollNodeKind::Viewport:
        return AsyncScrollNodeKind::Viewport;
    case Compositing::CompositorScrollNodeKind::Element:
        return AsyncScrollNodeKind::Element;
    case Compositing::CompositorScrollNodeKind::PseudoElement:
        return AsyncScrollNodeKind::PseudoElement;
    }
    VERIFY_NOT_REACHED();
}

static AsyncScrollNodeStableID stable_scroll_node_id_for(UniqueNodeID scrollable_node_id, Compositing::CompositorScrollNodeKind kind, u8 pseudo_element_type)
{
    return {
        .node_id = scrollable_node_id,
        .kind = async_scroll_node_kind_for(kind),
        .pseudo_element_type = pseudo_element_type,
    };
}

AsyncScrollingState async_scrolling_state_from_display_list(Compositing::DisplayList const& display_list)
{
    AsyncScrollingState async_scrolling_state;
    u32 next_paint_order_index = 0;

    if (auto const& metadata = display_list.async_scrolling_metadata(); metadata.has_value()) {
        async_scrolling_state.viewport_rect = metadata->viewport_rect;
        async_scrolling_state.wheel_event_listener_state_generation = metadata->wheel_event_listener_state_generation;
        async_scrolling_state.has_blocking_wheel_event_listeners = metadata->has_blocking_wheel_event_listeners;
        async_scrolling_state.has_blocking_wheel_event_region_covering_viewport = metadata->has_blocking_wheel_event_region_covering_viewport;
        async_scrolling_state.device_pixels_per_css_pixel = metadata->device_pixels_per_css_pixel;
    }

    auto read_compositor_metadata = [&](Compositing::DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        auto append_wheel_hit_test_target = [&](auto const& command, Gfx::CornerRadii corner_radii) {
            Optional<AsyncScrollNodeID> target_node_id;
            if (command.target_scroll_node_index.value())
                target_node_id = scroll_node_id_for(command.document_id, command.target_scroll_node_index);
            async_scrolling_state.wheel_hit_test_targets.append({
                .context = header.context,
                .rect = command.rect,
                .corner_radii = corner_radii,
                .target_node_id = target_node_id,
                .paint_order_index = next_paint_order_index++,
            });
        };

        switch (header.command_type) {
        case Compositing::DisplayListCommandType::CompositorBlockingWheelEventRegion: {
            auto command = Compositing::read_display_list_command_payload<Compositing::CompositorBlockingWheelEventRegion>(payload);
            async_scrolling_state.has_blocking_wheel_event_listeners = true;
            async_scrolling_state.blocking_wheel_event_regions.append({
                .context = header.context,
                .rect = command.rect,
            });
            break;
        }
        case Compositing::DisplayListCommandType::CompositorScrollNode: {
            auto command = Compositing::read_display_list_command_payload<Compositing::CompositorScrollNode>(payload);
            Optional<AsyncScrollNodeID> parent_node_id;
            if (command.parent_scroll_node_index.value())
                parent_node_id = scroll_node_id_for(command.document_id, command.parent_scroll_node_index);
            async_scrolling_state.scroll_nodes.append({
                .node_id = scroll_node_id_for(command.document_id, command.scroll_node_index),
                .stable_node_id = stable_scroll_node_id_for(command.scrollable_node_id, command.scroll_node_kind, command.pseudo_element_type),
                .parent_node_id = parent_node_id,
                .scrollport_rect = command.scrollport_rect,
                .min_scroll_offset = command.min_scroll_offset,
                .max_scroll_offset = command.max_scroll_offset,
                .is_viewport = command.is_viewport,
                .can_be_wheel_scrolled_horizontally = command.can_be_wheel_scrolled_horizontally,
                .can_be_wheel_scrolled_vertically = command.can_be_wheel_scrolled_vertically,
            });
            break;
        }
        case Compositing::DisplayListCommandType::CompositorWheelHitTestTarget: {
            auto command = Compositing::read_display_list_command_payload<Compositing::CompositorWheelHitTestTarget>(payload);
            append_wheel_hit_test_target(command, {});
            break;
        }
        case Compositing::DisplayListCommandType::CompositorWheelHitTestTargetWithCornerRadii: {
            auto command = Compositing::read_display_list_command_payload<Compositing::CompositorWheelHitTestTargetWithCornerRadii>(payload);
            append_wheel_hit_test_target(command, command.corner_radii);
            break;
        }
        case Compositing::DisplayListCommandType::CompositorMainThreadWheelEventRegion: {
            auto command = Compositing::read_display_list_command_payload<Compositing::CompositorMainThreadWheelEventRegion>(payload);
            async_scrolling_state.main_thread_wheel_event_regions.append({
                .context = header.context,
                .rect = command.rect,
            });
            break;
        }
        case Compositing::DisplayListCommandType::CompositorScrollbar: {
            auto command = Compositing::read_display_list_command_payload<Compositing::CompositorScrollbar>(payload);
            async_scrolling_state.scrollbars.append({
                .scroll_node_id = scroll_node_id_for(command.document_id, command.scroll_node_index),
                .scroller_stable_node_id = {},
                .scroll_node_index = command.scroll_node_index,
                .context = header.context,
                .paint_order_index = next_paint_order_index++,
                .gutter_rect = command.gutter_rect,
                .thumb_rect = command.thumb_rect,
                .track_rect = command.track_rect,
                .expanded_gutter_rect = command.expanded_gutter_rect,
                .expanded_thumb_rect = command.expanded_thumb_rect,
                .scroll_size = command.scroll_size,
                .expanded_scroll_size = command.expanded_scroll_size,
                .min_scroll_offset = command.min_scroll_offset,
                .max_scroll_offset = command.max_scroll_offset,
                .thumb_color = command.thumb_color,
                .track_color = command.track_color,
                .vertical = command.vertical,
                .is_painted_by_compositor = command.is_painted_by_compositor,
                .display_list_paints_enlarged_scrollbar = command.display_list_paints_enlarged_scrollbar,
            });
            break;
        }
        case Compositing::DisplayListCommandType::CompositorSnapContainer: {
            auto command = Compositing::read_display_list_command_payload<Compositing::CompositorSnapContainer>(payload);
            async_scrolling_state.snap_containers.append({
                .node_id = scroll_node_id_for(command.document_id, command.scroll_node_index),
                .geometry = {
                    .snapport = command.snapport,
                    .min_scroll_offset = command.min_scroll_offset,
                    .max_scroll_offset = command.max_scroll_offset,
                    .strictness = static_cast<SnapStrictness>(command.strictness),
                    .axes = { .x = command.snaps_x, .y = command.snaps_y },
                    .horizontal_writing_mode = command.horizontal_writing_mode,
                },
                .areas = {},
            });
            break;
        }
        case Compositing::DisplayListCommandType::CompositorSnapArea: {
            auto command = Compositing::read_display_list_command_payload<Compositing::CompositorSnapArea>(payload);
            auto& snap_containers = async_scrolling_state.snap_containers;
            // A snap container's areas are recorded right after it.
            if (snap_containers.is_empty() || snap_containers.last().node_id != scroll_node_id_for(command.document_id, command.scroll_node_index)) {
                dbgln("Ignoring a snap area recorded without its snap container");
                break;
            }
            snap_containers.last().areas.append({
                .identity = { .node_id = command.area_node_id, .pseudo_element_type = command.pseudo_element_type },
                .rect = command.rect,
                .align_x = static_cast<SnapAlign>(command.align_x),
                .align_y = static_cast<SnapAlign>(command.align_y),
                .always_stop = command.always_stop,
            });
            break;
        }
        default:
            break;
        }
    };
    for (auto const& run : display_list.command_runs()) {
        if (!run.has_compositor_metadata)
            continue;
        Compositing::DisplayList::for_each_command_header(display_list.command_bytes_of_run(run), read_compositor_metadata);
    }

    for (auto& scrollbar : async_scrolling_state.scrollbars) {
        for (auto const& scroll_node : async_scrolling_state.scroll_nodes) {
            if (scroll_node.node_id == scrollbar.scroll_node_id) {
                scrollbar.scroller_stable_node_id = scroll_node.stable_node_id;
                break;
            }
        }
    }

    return async_scrolling_state;
}

WheelRoutingAdmission wheel_routing_admission_for(AsyncScrollingState const& state)
{
    if (state.has_blocking_wheel_event_region_covering_viewport)
        return WheelRoutingAdmission::BlockingWheelEventListeners;

    if (state.scroll_nodes.is_empty())
        return WheelRoutingAdmission::NoScrollNode;
    return WheelRoutingAdmission::Accepted;
}

Utf16View wheel_routing_admission_to_utf16_view(WheelRoutingAdmission admission)
{
    switch (admission) {
    case WheelRoutingAdmission::Accepted:
        return u"accepted"sv;
    case WheelRoutingAdmission::NoAsyncScrollingState:
        return u"no async scrolling state"sv;
    case WheelRoutingAdmission::BlockingWheelEventListeners:
        return u"blocking wheel event listeners"sv;
    case WheelRoutingAdmission::NoScrollNode:
        return u"no scroll node"sv;
    case WheelRoutingAdmission::StaleWheelEventListeners:
        return u"stale wheel event listeners"sv;
    }
    VERIFY_NOT_REACHED();
}

static AsyncScrollTree scroll_tree_for_hit_testing(AsyncScrollingState async_scrolling_state, RefPtr<Compositing::DisplayList const> const& display_list, Compositing::AccumulatedVisualContextTree const* visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state_snapshot)
{
    AsyncScrollTree scroll_tree;
    scroll_tree.set_state(move(async_scrolling_state));
    scroll_tree.rebuild_wheel_hit_test_targets(display_list, visual_context_tree, scroll_state_snapshot);
    return scroll_tree;
}

bool blocks_wheel_event_at_position(AsyncScrollingState const& async_scrolling_state, RefPtr<Compositing::DisplayList const> const& display_list, Compositing::AccumulatedVisualContextTree const* visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position)
{
    if (async_scrolling_state.has_blocking_wheel_event_region_covering_viewport)
        return true;

    // If a caller knows blocking wheel listeners exist but cannot provide a display list for visual-context hit
    // testing, async scrolling must fail closed. Sending the input to the main thread is slower, but it preserves
    // cancelability.
    if (!display_list || !visual_context_tree)
        return async_scrolling_state.has_blocking_wheel_event_listeners;

    auto scroll_tree = scroll_tree_for_hit_testing(async_scrolling_state, display_list, visual_context_tree, scroll_state_snapshot);
    return scroll_tree.blocks_wheel_event_at_position(*visual_context_tree, position);
}

WheelScrollAdmission admit_wheel_scroll(AsyncScrollingState const& async_scrolling_state, RefPtr<Compositing::DisplayList const> const& display_list, Compositing::AccumulatedVisualContextTree const* visual_context_tree, Compositing::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position, Gfx::FloatPoint delta, bool blocking_wheel_event_regions_are_current)
{
    WheelHitTestResult hit_test_result;
    if (display_list && visual_context_tree) {
        auto scroll_tree = scroll_tree_for_hit_testing(async_scrolling_state, display_list, visual_context_tree, scroll_state_snapshot);
        hit_test_result = scroll_tree.hit_test_scroll_node_for_wheel(*visual_context_tree, position, delta);
    } else {
        hit_test_result.blocked_by_wheel_event_region = async_scrolling_state.has_blocking_wheel_event_listeners;
    }
    if (hit_test_result.blocked_by_main_thread_region)
        return WheelScrollAdmission::BlockedByMainThreadRegion;

    // Async scrolling may only start when the snapshot can prove that the wheel event cannot be canceled by script
    // at this position. Stale or missing blocker information sends the event back to the main thread.
    if (async_scrolling_state.has_blocking_wheel_event_listeners) {
        if (!blocking_wheel_event_regions_are_current)
            return WheelScrollAdmission::StaleBlockingWheelEventRegions;
        if (hit_test_result.blocked_by_wheel_event_region)
            return WheelScrollAdmission::BlockedByWheelEventRegion;
    }

    if (!hit_test_result.node_id.has_value())
        return WheelScrollAdmission::NoScrollableTarget;
    return WheelScrollAdmission::Accepted;
}

}
