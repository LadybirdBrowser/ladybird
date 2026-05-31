/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Compositor {

static AsyncScrollNodeID scroll_node_id_for(UniqueNodeID document_id, Painting::ScrollFrameIndex scroll_frame_index)
{
    return { .document_id = document_id, .scroll_frame_index = scroll_frame_index };
}

static AsyncScrollNodeKind async_scroll_node_kind_for(Painting::CompositorScrollNodeKind kind)
{
    switch (kind) {
    case Painting::CompositorScrollNodeKind::Viewport:
        return AsyncScrollNodeKind::Viewport;
    case Painting::CompositorScrollNodeKind::Element:
        return AsyncScrollNodeKind::Element;
    case Painting::CompositorScrollNodeKind::PseudoElement:
        return AsyncScrollNodeKind::PseudoElement;
    }
    VERIFY_NOT_REACHED();
}

static AsyncScrollNodeStableID stable_scroll_node_id_for(UniqueNodeID scrollable_node_id, Painting::CompositorScrollNodeKind kind, u8 pseudo_element_type)
{
    return {
        .node_id = scrollable_node_id,
        .kind = async_scroll_node_kind_for(kind),
        .pseudo_element_type = pseudo_element_type,
    };
}

AsyncScrollingState async_scrolling_state_from_display_list(Painting::DisplayList const& display_list)
{
    AsyncScrollingState async_scrolling_state;
    Vector<Painting::ScrollFrameIndex> parent_scroll_frame_indices;
    Vector<Painting::ScrollFrameIndex> wheel_hit_test_target_scroll_frame_indices;
    Vector<UniqueNodeID> wheel_hit_test_target_document_ids;

    if (auto const& metadata = display_list.async_scrolling_metadata(); metadata.has_value()) {
        async_scrolling_state.viewport_rect = metadata->viewport_rect;
        async_scrolling_state.wheel_event_listener_state_generation = metadata->wheel_event_listener_state_generation;
        async_scrolling_state.has_blocking_wheel_event_listeners = metadata->has_blocking_wheel_event_listeners;
        async_scrolling_state.has_blocking_wheel_event_region_covering_viewport = metadata->has_blocking_wheel_event_region_covering_viewport;
    }

    display_list.for_each_command_header([&](Painting::DisplayListCommandHeader const& header, ReadonlyBytes payload) {
        auto append_wheel_hit_test_target = [&](auto const& command, Gfx::CornerRadii corner_radii) {
            async_scrolling_state.wheel_hit_test_targets.append({
                .visual_context_index = header.context_index,
                .rect = command.rect,
                .corner_radii = corner_radii,
                .target_node_id = {},
            });
            wheel_hit_test_target_scroll_frame_indices.append(command.target_scroll_frame_index);
            wheel_hit_test_target_document_ids.append(command.document_id);
        };

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
                .stable_node_id = stable_scroll_node_id_for(command.scrollable_node_id, command.scroll_node_kind, command.pseudo_element_type),
                .parent_node_id = {},
                .hit_test_visual_context_index = header.context_index,
                .scrollport_rect = command.scrollport_rect,
                .max_scroll_offset = command.max_scroll_offset,
                .is_viewport = command.is_viewport,
                .can_be_wheel_scrolled_horizontally = command.can_be_wheel_scrolled_horizontally,
                .can_be_wheel_scrolled_vertically = command.can_be_wheel_scrolled_vertically,
            });
            parent_scroll_frame_indices.append(command.parent_scroll_frame_index);
            break;
        }
        case Painting::DisplayListCommandType::CompositorWheelHitTestTarget: {
            auto command = Painting::read_display_list_command_payload<Painting::CompositorWheelHitTestTarget>(payload);
            append_wheel_hit_test_target(command, {});
            break;
        }
        case Painting::DisplayListCommandType::CompositorWheelHitTestTargetWithCornerRadii: {
            auto command = Painting::read_display_list_command_payload<Painting::CompositorWheelHitTestTargetWithCornerRadii>(payload);
            append_wheel_hit_test_target(command, command.corner_radii);
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

    VERIFY(wheel_hit_test_target_scroll_frame_indices.size() == async_scrolling_state.wheel_hit_test_targets.size());
    VERIFY(wheel_hit_test_target_document_ids.size() == async_scrolling_state.wheel_hit_test_targets.size());
    for (size_t i = 0; i < async_scrolling_state.wheel_hit_test_targets.size(); ++i) {
        auto target_scroll_frame_index = wheel_hit_test_target_scroll_frame_indices[i];
        if (target_scroll_frame_index.value())
            async_scrolling_state.wheel_hit_test_targets[i].target_node_id = scroll_node_id_for(wheel_hit_test_target_document_ids[i], target_scroll_frame_index);
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

StringView wheel_routing_admission_to_string(WheelRoutingAdmission admission)
{
    switch (admission) {
    case WheelRoutingAdmission::Accepted:
        return "accepted"sv;
    case WheelRoutingAdmission::NoAsyncScrollingState:
        return "no async scrolling state"sv;
    case WheelRoutingAdmission::BlockingWheelEventListeners:
        return "blocking wheel event listeners"sv;
    case WheelRoutingAdmission::NoScrollNode:
        return "no scroll node"sv;
    case WheelRoutingAdmission::StaleWheelEventListeners:
        return "stale wheel event listeners"sv;
    }
    VERIFY_NOT_REACHED();
}

bool blocks_wheel_event_at_position(AsyncScrollingState const& async_scrolling_state, RefPtr<Painting::DisplayList const> const& display_list, Painting::AccumulatedVisualContextTree const* visual_context_tree, Painting::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position)
{
    if (async_scrolling_state.has_blocking_wheel_event_region_covering_viewport)
        return true;

    // If a caller knows blocking wheel listeners exist but cannot provide a display list for visual-context hit
    // testing, async scrolling must fail closed. Sending the input to the main thread is slower, but it preserves
    // cancelability.
    if (!display_list || !visual_context_tree)
        return async_scrolling_state.has_blocking_wheel_event_listeners;

    VERIFY(display_list->compatible_visual_context_tree_version() == visual_context_tree->version());
    for (auto const& region : async_scrolling_state.blocking_wheel_event_regions) {
        auto position_in_context = visual_context_tree->transform_point_for_hit_test(region.visual_context_index, position, scroll_state_snapshot);
        if (position_in_context.has_value() && region.rect.contains(*position_in_context))
            return true;
    }
    return false;
}

static WheelHitTestResult hit_test_scroll_node_at_position(AsyncScrollingState const& async_scrolling_state, RefPtr<Painting::DisplayList const> const& display_list, Painting::AccumulatedVisualContextTree const* visual_context_tree, Painting::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position, Gfx::FloatPoint delta)
{
    if (!display_list || !visual_context_tree)
        return {};

    AsyncScrollTree scroll_tree;
    auto async_scrolling_state_copy = async_scrolling_state;
    scroll_tree.set_state(move(async_scrolling_state_copy));
    scroll_tree.rebuild_wheel_hit_test_targets(display_list, visual_context_tree, scroll_state_snapshot);
    return scroll_tree.hit_test_scroll_node_for_wheel(position, delta);
}

WheelScrollAdmission admit_wheel_scroll(AsyncScrollingState const& async_scrolling_state, RefPtr<Painting::DisplayList const> const& display_list, Painting::AccumulatedVisualContextTree const* visual_context_tree, Painting::ScrollStateSnapshot const& scroll_state_snapshot, Gfx::FloatPoint position, Gfx::FloatPoint delta, bool blocking_wheel_event_regions_are_current)
{
    auto hit_test_result = hit_test_scroll_node_at_position(async_scrolling_state, display_list, visual_context_tree, scroll_state_snapshot, position, delta);
    if (hit_test_result.blocked_by_main_thread_region)
        return WheelScrollAdmission::BlockedByMainThreadRegion;

    // Async scrolling may only start when the snapshot can prove that the wheel event cannot be canceled by script
    // at this position. Stale or missing blocker information sends the event back to the main thread.
    if (async_scrolling_state.has_blocking_wheel_event_listeners) {
        if (!blocking_wheel_event_regions_are_current)
            return WheelScrollAdmission::StaleBlockingWheelEventRegions;
        if (blocks_wheel_event_at_position(async_scrolling_state, display_list, visual_context_tree, scroll_state_snapshot, position))
            return WheelScrollAdmission::BlockedByWheelEventRegion;
    }

    if (!hit_test_result.node_id.has_value())
        return WheelScrollAdmission::NoScrollableTarget;
    return WheelScrollAdmission::Accepted;
}

}
