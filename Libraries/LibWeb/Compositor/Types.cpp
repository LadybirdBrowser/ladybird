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
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::PendingAsyncScrollUpdates const& updates)
{
    TRY(encoder.encode(updates.scroll_offsets));
    TRY(encoder.encode(updates.completed_operation_ids));
    return {};
}

template<>
ErrorOr<Web::Compositor::PendingAsyncScrollUpdates> decode(Decoder& decoder)
{
    return Web::Compositor::PendingAsyncScrollUpdates {
        .scroll_offsets = TRY(decoder.decode<Vector<Web::Compositor::AsyncScrollOffset>>()),
        .completed_operation_ids = TRY(decoder.decode<Vector<Web::Compositor::AsyncScrollOperationID>>()),
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

template<>
ErrorOr<void> encode(Encoder& encoder, Web::Compositor::PublishToCompositorSurface const& mode)
{
    TRY(encoder.encode(mode.target_context_id));
    TRY(encoder.encode(mode.surface_id));
    return {};
}

template<>
ErrorOr<Web::Compositor::PublishToCompositorSurface> decode(Decoder& decoder)
{
    return Web::Compositor::PublishToCompositorSurface {
        .target_context_id = TRY(decoder.decode<Web::Compositor::CompositorContextId>()),
        .surface_id = TRY(decoder.decode<Web::Painting::CompositorSurfaceId>()),
    };
}

template<>
ErrorOr<void> encode(Encoder&, Web::Compositor::PresentToClient const&)
{
    return {};
}

template<>
ErrorOr<Web::Compositor::PresentToClient> decode(Decoder&)
{
    return Web::Compositor::PresentToClient {};
}

}
