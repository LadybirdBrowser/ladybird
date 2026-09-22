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
ErrorOr<void> encode(Encoder& encoder, Compositing::AsyncScrollNodeStableID const& stable_node_id)
{
    TRY(encoder.encode(stable_node_id.node_id));
    TRY(encoder.encode(stable_node_id.kind));
    TRY(encoder.encode(stable_node_id.pseudo_element_type));
    return {};
}

template<>
ErrorOr<Compositing::AsyncScrollNodeStableID> decode(Decoder& decoder)
{
    return Compositing::AsyncScrollNodeStableID {
        .node_id = TRY(decoder.decode<Compositing::UniqueNodeID>()),
        .kind = TRY(decoder.decode<Compositing::AsyncScrollNodeKind>()),
        .pseudo_element_type = TRY(decoder.decode<u8>()),
    };
}

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
        .target = TRY(decoder.decode<Optional<Compositing::AsyncScrollNodeStableID>>()),
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
    return {};
}

template<>
ErrorOr<Compositing::AsyncScrollOffset> decode(Decoder& decoder)
{
    return Compositing::AsyncScrollOffset {
        .stable_node_id = TRY(decoder.decode<Compositing::AsyncScrollNodeStableID>()),
        .compositor_scroll_offset = TRY(decoder.decode<Gfx::FloatPoint>()),
        .unadopted_scroll_delta = TRY(decoder.decode<Gfx::FloatPoint>()),
        .last_relative_scroll_delta = TRY(decoder.decode<Gfx::FloatPoint>()),
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
        .node_id = TRY(decoder.decode<Compositing::UniqueNodeID>()),
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
        .position = TRY(decoder.decode<Compositing::CSSPixelPoint>()),
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
    TRY(encoder.encode(started_user_scroll.selection));
    TRY(encoder.encode(started_user_scroll.settles_gesture));
    return {};
}

template<>
ErrorOr<Compositing::StartedUserScroll> decode(Decoder& decoder)
{
    return Compositing::StartedUserScroll {
        .stable_node_id = TRY(decoder.decode<Compositing::AsyncScrollNodeStableID>()),
        .operation_id = TRY(decoder.decode<Compositing::AsyncScrollOperationID>()),
        .initial_scroll_offset = TRY(decoder.decode<Compositing::CSSPixelPoint>()),
        .unsnapped_scroll_destination = TRY(decoder.decode<Compositing::CSSPixelPoint>()),
        .selection = TRY(decoder.decode<Compositing::SnapDestination>()),
        .settles_gesture = TRY(decoder.decode<bool>()),
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
    TRY(encoder.encode(updates.user_scroll_gesture_in_progress));
    TRY(encoder.encode(updates.user_scroll_gesture_ended));
    return {};
}

template<>
ErrorOr<Compositing::PendingAsyncScrollUpdates> decode(Decoder& decoder)
{
    return Compositing::PendingAsyncScrollUpdates {
        .document_id = TRY(decoder.decode<Optional<Compositing::UniqueNodeID>>()),
        .sequence = TRY(decoder.decode<u64>()),
        .scroll_offsets = TRY(decoder.decode<Vector<Compositing::AsyncScrollOffset>>()),
        .completed_operation_ids = TRY(decoder.decode<Vector<Compositing::AsyncScrollOperationID>>()),
        .operation_ids_taken_over_by_user_input = TRY(decoder.decode<Vector<Compositing::AsyncScrollOperationID>>()),
        .started_user_scrolls = TRY(decoder.decode<Vector<Compositing::StartedUserScroll>>()),
        .user_scroll_gesture_in_progress = TRY(decoder.decode<bool>()),
        .user_scroll_gesture_ended = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::ScrollbarDraggedByCompositor const& scrollbar)
{
    TRY(encoder.encode(scrollbar.scroller_stable_node_id));
    TRY(encoder.encode(scrollbar.vertical));
    return {};
}

template<>
ErrorOr<Compositing::ScrollbarDraggedByCompositor> decode(Decoder& decoder)
{
    return Compositing::ScrollbarDraggedByCompositor {
        .scroller_stable_node_id = TRY(decoder.decode<Compositing::AsyncScrollNodeStableID>()),
        .vertical = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::MouseEventHandlingResult const& result)
{
    TRY(encoder.encode(result.handled));
    TRY(encoder.encode(result.scrollbar_dragged_by_compositor));
    return {};
}

template<>
ErrorOr<Compositing::MouseEventHandlingResult> decode(Decoder& decoder)
{
    return Compositing::MouseEventHandlingResult {
        .handled = TRY(decoder.decode<bool>()),
        .scrollbar_dragged_by_compositor = TRY(decoder.decode<Optional<Compositing::ScrollbarDraggedByCompositor>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Compositing::AsyncScrollEnqueueResult const& result)
{
    TRY(encoder.encode(result.accepted));
    TRY(encoder.encode(result.operation_id));
    return {};
}

template<>
ErrorOr<Compositing::AsyncScrollEnqueueResult> decode(Decoder& decoder)
{
    return Compositing::AsyncScrollEnqueueResult {
        .accepted = TRY(decoder.decode<bool>()),
        .operation_id = TRY(decoder.decode<Optional<Compositing::AsyncScrollOperationID>>()),
    };
}

}
