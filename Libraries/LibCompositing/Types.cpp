/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/Types.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::KeyboardScrollState const& state)
{
    TRY(encoder.encode(state.generation));
    TRY(encoder.encode(state.visual_context_tree_structural_epoch));
    TRY(encoder.encode(state.target));
    TRY(encoder.encode(state.page_scroll_distance));
    TRY(encoder.encode(state.arrow_scroll_distance));
    return {};
}

template<>
ErrorOr<Compositing::KeyboardScrollState> decode(Decoder& decoder)
{
    return Compositing::KeyboardScrollState {
        .generation = TRY(decoder.decode<u64>()),
        .visual_context_tree_structural_epoch = TRY(decoder.decode<u64>()),
        .target = TRY(decoder.decode<Optional<Web::AsyncScrollNodeStableID>>()),
        .page_scroll_distance = TRY(decoder.decode<float>()),
        .arrow_scroll_distance = TRY(decoder.decode<float>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::AsyncScrollOffset const& offset)
{
    TRY(encoder.encode(offset.stable_node_id));
    TRY(encoder.encode(offset.compositor_scroll_offset));
    TRY(encoder.encode(offset.unadopted_scroll_delta));
    TRY(encoder.encode(offset.last_relative_scroll_delta));
    TRY(encoder.encode(offset.visual_viewport_scale));
    TRY(encoder.encode(offset.applied_scroll_request_id));
    TRY(encoder.encode(offset.user_input_scroll_count));
    return {};
}

template<>
ErrorOr<Compositing::AsyncScrollOffset> decode(Decoder& decoder)
{
    return Compositing::AsyncScrollOffset {
        .stable_node_id = TRY(decoder.decode<Web::AsyncScrollNodeStableID>()),
        .compositor_scroll_offset = TRY(decoder.decode<Gfx::FloatPoint>()),
        .unadopted_scroll_delta = TRY(decoder.decode<Gfx::FloatPoint>()),
        .last_relative_scroll_delta = TRY(decoder.decode<Gfx::FloatPoint>()),
        .visual_viewport_scale = TRY(decoder.decode<Optional<float>>()),
        .applied_scroll_request_id = TRY(decoder.decode<u64>()),
        .user_input_scroll_count = TRY(decoder.decode<u64>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::SnapAreaIdentity const& identity)
{
    TRY(encoder.encode(identity.node_id));
    TRY(encoder.encode(identity.pseudo_element_type));
    return {};
}

template<>
ErrorOr<Compositing::SnapAreaIdentity> decode(Decoder& decoder)
{
    return Compositing::SnapAreaIdentity {
        .node_id = TRY(decoder.decode<Web::UniqueNodeID>()),
        .pseudo_element_type = TRY(decoder.decode<u8>()),
    };
}

// NB: CSS pixel points travel as their raw fixed-point values, so that a destination is exactly the one selected.
template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::SnapDestination const& destination)
{
    TRY(encoder.encode(destination.position));
    TRY(encoder.encode(destination.snapped_x));
    TRY(encoder.encode(destination.snapped_y));
    TRY(encoder.encode(destination.evaluated_x));
    TRY(encoder.encode(destination.evaluated_y));
    TRY(encoder.encode(destination.snapped_areas.x));
    TRY(encoder.encode(destination.snapped_areas.y));
    return {};
}

template<>
ErrorOr<Compositing::SnapDestination> decode(Decoder& decoder)
{
    return Compositing::SnapDestination {
        .position = TRY(decoder.decode<Web::CSSPixelPoint>()),
        .snapped_x = TRY(decoder.decode<bool>()),
        .snapped_y = TRY(decoder.decode<bool>()),
        .evaluated_x = TRY(decoder.decode<bool>()),
        .evaluated_y = TRY(decoder.decode<bool>()),
        .snapped_areas = {
            .x = TRY(decoder.decode<Vector<Compositing::SnapAreaIdentity>>()),
            .y = TRY(decoder.decode<Vector<Compositing::SnapAreaIdentity>>()),
        },
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::StartedUserScroll const& started_user_scroll)
{
    TRY(encoder.encode(started_user_scroll.stable_node_id));
    TRY(encoder.encode(started_user_scroll.operation_id));
    TRY(encoder.encode(started_user_scroll.initial_scroll_offset));
    TRY(encoder.encode(started_user_scroll.unsnapped_scroll_destination));
    TRY(encoder.encode(started_user_scroll.relative_scroll_delta));
    TRY(encoder.encode(started_user_scroll.selection));
    TRY(encoder.encode(started_user_scroll.settles_gesture));
    TRY(encoder.encode(started_user_scroll.is_absolute_scroll));
    TRY(encoder.encode(started_user_scroll.animation_kind));
    return {};
}

template<>
ErrorOr<Compositing::StartedUserScroll> decode(Decoder& decoder)
{
    return Compositing::StartedUserScroll {
        .stable_node_id = TRY(decoder.decode<Web::AsyncScrollNodeStableID>()),
        .operation_id = TRY(decoder.decode<Compositing::AsyncScrollOperationID>()),
        .initial_scroll_offset = TRY(decoder.decode<Web::CSSPixelPoint>()),
        .unsnapped_scroll_destination = TRY(decoder.decode<Web::CSSPixelPoint>()),
        .relative_scroll_delta = TRY(decoder.decode<Web::CSSPixelPoint>()),
        .selection = TRY(decoder.decode<Compositing::SnapDestination>()),
        .settles_gesture = TRY(decoder.decode<bool>()),
        .is_absolute_scroll = TRY(decoder.decode<bool>()),
        .animation_kind = TRY(decoder.decode<Optional<Compositing::ScrollAnimationKind>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::AppliedScrollRequest const& applied_scroll_request)
{
    TRY(encoder.encode(applied_scroll_request.stable_node_id));
    TRY(encoder.encode(applied_scroll_request.request_id));
    TRY(encoder.encode(applied_scroll_request.moved_the_box));
    return {};
}

template<>
ErrorOr<Compositing::AppliedScrollRequest> decode(Decoder& decoder)
{
    return Compositing::AppliedScrollRequest {
        .stable_node_id = TRY(decoder.decode<Web::AsyncScrollNodeStableID>()),
        .request_id = TRY(decoder.decode<u64>()),
        .moved_the_box = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::PendingAsyncScrollUpdates const& updates)
{
    TRY(encoder.encode(updates.document_id));
    TRY(encoder.encode(updates.sequence));
    TRY(encoder.encode(updates.scroll_offsets));
    TRY(encoder.encode(updates.completed_operation_ids));
    TRY(encoder.encode(updates.operation_ids_taken_over_by_user_input));
    TRY(encoder.encode(updates.started_user_scrolls));
    TRY(encoder.encode(updates.applied_scroll_requests));
    TRY(encoder.encode(updates.boxes_in_user_scroll_gesture));
    TRY(encoder.encode(updates.boxes_whose_user_scroll_gesture_ended));
    TRY(encoder.encode(updates.wheel_latched_scroller));
    return {};
}

template<>
ErrorOr<Compositing::PendingAsyncScrollUpdates> decode(Decoder& decoder)
{
    return Compositing::PendingAsyncScrollUpdates {
        .document_id = TRY(decoder.decode<Optional<Web::UniqueNodeID>>()),
        .sequence = TRY(decoder.decode<u64>()),
        .scroll_offsets = TRY(decoder.decode<Vector<Compositing::AsyncScrollOffset>>()),
        .completed_operation_ids = TRY(decoder.decode<Vector<Compositing::AsyncScrollOperationID>>()),
        .operation_ids_taken_over_by_user_input = TRY(decoder.decode<Vector<Compositing::AsyncScrollOperationID>>()),
        .started_user_scrolls = TRY(decoder.decode<Vector<Compositing::StartedUserScroll>>()),
        .applied_scroll_requests = TRY(decoder.decode<Vector<Compositing::AppliedScrollRequest>>()),
        .boxes_in_user_scroll_gesture = TRY(decoder.decode<Vector<Web::AsyncScrollNodeStableID>>()),
        .boxes_whose_user_scroll_gesture_ended = TRY(decoder.decode<Vector<Web::AsyncScrollNodeStableID>>()),
        .wheel_latched_scroller = TRY(decoder.decode<Optional<Web::AsyncScrollNodeStableID>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::RequestedRelativeScroll const& relative_scroll)
{
    TRY(encoder.encode(relative_scroll.delta));
    TRY(encoder.encode(relative_scroll.lower_bound));
    TRY(encoder.encode(relative_scroll.upper_bound));
    return {};
}

template<>
ErrorOr<Compositing::RequestedRelativeScroll> decode(Decoder& decoder)
{
    return Compositing::RequestedRelativeScroll {
        .delta = TRY(decoder.decode<Gfx::FloatPoint>()),
        .lower_bound = TRY(decoder.decode<Gfx::FloatPoint>()),
        .upper_bound = TRY(decoder.decode<Gfx::FloatPoint>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::RequestedSmoothScroll const& smooth_scroll)
{
    TRY(encoder.encode(smooth_scroll.operation_id));
    TRY(encoder.encode(smooth_scroll.destination_offset));
    TRY(encoder.encode(smooth_scroll.destination_is_relative));
    TRY(encoder.encode(smooth_scroll.animation_kind));
    TRY(encoder.encode(smooth_scroll.initiator));
    TRY(encoder.encode(smooth_scroll.retargets_operation));
    TRY(encoder.encode(smooth_scroll.settled_user_input_scroll_count));
    return {};
}

template<>
ErrorOr<Compositing::RequestedSmoothScroll> decode(Decoder& decoder)
{
    return Compositing::RequestedSmoothScroll {
        .operation_id = TRY(decoder.decode<Compositing::AsyncScrollOperationID>()),
        .destination_offset = TRY(decoder.decode<Gfx::FloatPoint>()),
        .destination_is_relative = TRY(decoder.decode<bool>()),
        .animation_kind = TRY(decoder.decode<Compositing::ScrollAnimationKind>()),
        .initiator = TRY(decoder.decode<Compositing::SmoothScrollInitiator>()),
        .retargets_operation = TRY(decoder.decode<bool>()),
        .settled_user_input_scroll_count = TRY(decoder.decode<Optional<u64>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::ScrollRequest const& request)
{
    TRY(encoder.encode(request.stable_node_id));
    TRY(encoder.encode(request.request_id));
    TRY(encoder.encode(request.jump_offset));
    TRY(encoder.encode(request.visual_viewport_scale));
    TRY(encoder.encode(request.relative_scroll));
    TRY(encoder.encode(request.relative_scroll_moves_visual_viewport_first));
    TRY(encoder.encode(request.instant_scroll_is_for_user_input));
    TRY(encoder.encode(request.smooth_scroll));
    TRY(encoder.encode(request.takes_over_from_momentum));
    return {};
}

template<>
ErrorOr<Compositing::ScrollRequest> decode(Decoder& decoder)
{
    return Compositing::ScrollRequest {
        .stable_node_id = TRY(decoder.decode<Web::AsyncScrollNodeStableID>()),
        .request_id = TRY(decoder.decode<u64>()),
        .jump_offset = TRY(decoder.decode<Optional<Gfx::FloatPoint>>()),
        .visual_viewport_scale = TRY(decoder.decode<Optional<float>>()),
        .relative_scroll = TRY(decoder.decode<Optional<Compositing::RequestedRelativeScroll>>()),
        .relative_scroll_moves_visual_viewport_first = TRY(decoder.decode<bool>()),
        .instant_scroll_is_for_user_input = TRY(decoder.decode<bool>()),
        .smooth_scroll = TRY(decoder.decode<Optional<Compositing::RequestedSmoothScroll>>()),
        .takes_over_from_momentum = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::AsyncScrollEnqueueResult const& result)
{
    TRY(encoder.encode(result.accepted));
    TRY(encoder.encode(result.operation_id));
    TRY(encoder.encode(result.routing_decision));
    TRY(encoder.encode(result.routed_scroller));
    return {};
}

template<>
ErrorOr<Compositing::AsyncScrollEnqueueResult> decode(Decoder& decoder)
{
    return Compositing::AsyncScrollEnqueueResult {
        .accepted = TRY(decoder.decode<bool>()),
        .operation_id = TRY(decoder.decode<Optional<Compositing::AsyncScrollOperationID>>()),
        .routing_decision = TRY(decoder.decode<Optional<Web::InputRoutingDecision>>()),
        .routed_scroller = TRY(decoder.decode<Optional<Web::AsyncScrollNodeStableID>>()),
    };
}

}
