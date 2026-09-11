/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Compositor/Types.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::AsyncScrollNodeStableID const& stable_node_id)
{
    TRY(encoder.encode(stable_node_id.node_id));
    TRY(encoder.encode(stable_node_id.kind));
    TRY(encoder.encode(stable_node_id.pseudo_element_type));
    return {};
}

template<>
ErrorOr<Web::Compositor::AsyncScrollNodeStableID> decode(Decoder& decoder)
{
    return Web::Compositor::AsyncScrollNodeStableID {
        .node_id = TRY(decoder.decode<Web::UniqueNodeID>()),
        .kind = TRY(decoder.decode<Web::Compositor::AsyncScrollNodeKind>()),
        .pseudo_element_type = TRY(decoder.decode<u8>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::AsyncScrollOffset const& offset)
{
    TRY(encoder.encode(offset.stable_node_id));
    TRY(encoder.encode(offset.compositor_scroll_offset));
    TRY(encoder.encode(offset.unadopted_scroll_delta));
    return {};
}

template<>
ErrorOr<Web::Compositor::AsyncScrollOffset> decode(Decoder& decoder)
{
    return Web::Compositor::AsyncScrollOffset {
        .stable_node_id = TRY(decoder.decode<Web::Compositor::AsyncScrollNodeStableID>()),
        .compositor_scroll_offset = TRY(decoder.decode<Gfx::FloatPoint>()),
        .unadopted_scroll_delta = TRY(decoder.decode<Gfx::FloatPoint>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::SnapAreaIdentity const& identity)
{
    TRY(encoder.encode(identity.node_id));
    TRY(encoder.encode(identity.pseudo_element_type));
    return {};
}

template<>
ErrorOr<Web::Compositor::SnapAreaIdentity> decode(Decoder& decoder)
{
    return Web::Compositor::SnapAreaIdentity {
        .node_id = TRY(decoder.decode<Web::UniqueNodeID>()),
        .pseudo_element_type = TRY(decoder.decode<u8>()),
    };
}

// NB: CSS pixel points travel as their raw fixed-point values, so that a destination is exactly the one selected.
template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::SnapDestination const& destination)
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
ErrorOr<Web::Compositor::SnapDestination> decode(Decoder& decoder)
{
    return Web::Compositor::SnapDestination {
        .position = TRY(decoder.decode<Web::CSSPixelPoint>()),
        .snapped_x = TRY(decoder.decode<bool>()),
        .snapped_y = TRY(decoder.decode<bool>()),
        .evaluated_x = TRY(decoder.decode<bool>()),
        .evaluated_y = TRY(decoder.decode<bool>()),
        .snapped_areas = {
            .x = TRY(decoder.decode<Vector<Web::Compositor::SnapAreaIdentity>>()),
            .y = TRY(decoder.decode<Vector<Web::Compositor::SnapAreaIdentity>>()),
        },
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::StartedSnapScroll const& started_snap_scroll)
{
    TRY(encoder.encode(started_snap_scroll.stable_node_id));
    TRY(encoder.encode(started_snap_scroll.operation_id));
    TRY(encoder.encode(started_snap_scroll.initial_scroll_offset));
    TRY(encoder.encode(started_snap_scroll.unsnapped_scroll_destination));
    TRY(encoder.encode(started_snap_scroll.selection));
    TRY(encoder.encode(started_snap_scroll.settles_gesture));
    return {};
}

template<>
ErrorOr<Web::Compositor::StartedSnapScroll> decode(Decoder& decoder)
{
    return Web::Compositor::StartedSnapScroll {
        .stable_node_id = TRY(decoder.decode<Web::Compositor::AsyncScrollNodeStableID>()),
        .operation_id = TRY(decoder.decode<Web::Compositor::AsyncScrollOperationID>()),
        .initial_scroll_offset = TRY(decoder.decode<Web::CSSPixelPoint>()),
        .unsnapped_scroll_destination = TRY(decoder.decode<Web::CSSPixelPoint>()),
        .selection = TRY(decoder.decode<Web::Compositor::SnapDestination>()),
        .settles_gesture = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::PendingAsyncScrollUpdates const& updates)
{
    TRY(encoder.encode(updates.sequence));
    TRY(encoder.encode(updates.scroll_offsets));
    TRY(encoder.encode(updates.completed_operation_ids));
    TRY(encoder.encode(updates.operation_ids_taken_over_by_user_input));
    TRY(encoder.encode(updates.started_snap_scrolls));
    TRY(encoder.encode(updates.user_scroll_gesture_in_progress));
    TRY(encoder.encode(updates.user_scroll_gesture_ended));
    return {};
}

template<>
ErrorOr<Web::Compositor::PendingAsyncScrollUpdates> decode(Decoder& decoder)
{
    return Web::Compositor::PendingAsyncScrollUpdates {
        .sequence = TRY(decoder.decode<u64>()),
        .scroll_offsets = TRY(decoder.decode<Vector<Web::Compositor::AsyncScrollOffset>>()),
        .completed_operation_ids = TRY(decoder.decode<Vector<Web::Compositor::AsyncScrollOperationID>>()),
        .operation_ids_taken_over_by_user_input = TRY(decoder.decode<Vector<Web::Compositor::AsyncScrollOperationID>>()),
        .started_snap_scrolls = TRY(decoder.decode<Vector<Web::Compositor::StartedSnapScroll>>()),
        .user_scroll_gesture_in_progress = TRY(decoder.decode<bool>()),
        .user_scroll_gesture_ended = TRY(decoder.decode<bool>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::AsyncScrollEnqueueResult const& result)
{
    TRY(encoder.encode(result.accepted));
    TRY(encoder.encode(result.operation_id));
    return {};
}

template<>
ErrorOr<Web::Compositor::AsyncScrollEnqueueResult> decode(Decoder& decoder)
{
    return Web::Compositor::AsyncScrollEnqueueResult {
        .accepted = TRY(decoder.decode<bool>()),
        .operation_id = TRY(decoder.decode<Optional<Web::Compositor::AsyncScrollOperationID>>()),
    };
}

}
