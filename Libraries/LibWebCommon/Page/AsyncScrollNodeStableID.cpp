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

}
