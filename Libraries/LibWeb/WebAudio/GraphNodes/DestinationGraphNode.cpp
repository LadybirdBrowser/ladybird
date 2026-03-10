/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/DestinationGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/DestinationRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> DestinationGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    size_t const clamped = min(channel_count, static_cast<size_t>(AK::NumericLimits<u32>::max()));
    return encoder.append_u32(static_cast<u32>(clamped));
}

ErrorOr<DestinationGraphNode> DestinationGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    DestinationGraphNode node;
    node.channel_count = TRY(decoder.read_u32());
    return node;
}

OwnPtr<RenderNode> DestinationGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<DestinationRenderNode>(node_id, channel_count, quantum_size);
}

GraphUpdateKind DestinationGraphNode::classify_update(DestinationGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (channel_count != new_desc.channel_count)
        return GraphUpdateKind::Topology;
    return GraphUpdateKind::None;
}

}
