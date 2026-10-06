/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Page/AsyncScrollNodeStableID.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::AsyncScrollNodeStableID const& stable_node_id)
{
    TRY(encoder.encode(stable_node_id.node_id));
    TRY(encoder.encode(stable_node_id.kind));
    TRY(encoder.encode(stable_node_id.pseudo_element_type));
    return {};
}

template<>
ErrorOr<Web::AsyncScrollNodeStableID> decode(Decoder& decoder)
{
    return Web::AsyncScrollNodeStableID {
        .node_id = TRY(decoder.decode<Web::UniqueNodeID>()),
        .kind = TRY(decoder.decode<Web::AsyncScrollNodeKind>()),
        .pseudo_element_type = TRY(decoder.decode<u8>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::CompositorScrollOffset const& scroll_offset)
{
    TRY(encoder.encode(scroll_offset.scroll_node));
    TRY(encoder.encode(scroll_offset.offset));
    return {};
}

template<>
ErrorOr<Web::CompositorScrollOffset> decode(Decoder& decoder)
{
    return Web::CompositorScrollOffset {
        .scroll_node = TRY(decoder.decode<Web::AsyncScrollNodeStableID>()),
        .offset = TRY(decoder.decode<Web::CSSPixelPoint>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::ScrollbarDraggedByCompositor const& scrollbar)
{
    TRY(encoder.encode(scrollbar.scroller_stable_node_id));
    TRY(encoder.encode(scrollbar.vertical));
    return {};
}

template<>
ErrorOr<Web::ScrollbarDraggedByCompositor> decode(Decoder& decoder)
{
    return Web::ScrollbarDraggedByCompositor {
        .scroller_stable_node_id = TRY(decoder.decode<Web::AsyncScrollNodeStableID>()),
        .vertical = TRY(decoder.decode<bool>()),
    };
}

}
