/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/ChannelSplitterGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/ChannelSplitterRenderNode.h>

namespace Web::WebAudio::Render {

static u32 clamp_size_to_u32(size_t value)
{
    ASSERT_CONTROL_THREAD();
    if (value > AK::NumericLimits<u32>::max())
        return AK::NumericLimits<u32>::max();
    return static_cast<u32>(value);
}

ErrorOr<void> ChannelSplitterGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    return encoder.append_u32(clamp_size_to_u32(number_of_outputs));
}

ErrorOr<ChannelSplitterGraphNode> ChannelSplitterGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    ChannelSplitterGraphNode node;
    node.number_of_outputs = TRY(decoder.read_u32());
    return node;
}

OwnPtr<RenderNode> ChannelSplitterGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<ChannelSplitterRenderNode>(node_id, *this, quantum_size);
}

GraphUpdateKind ChannelSplitterGraphNode::classify_update(ChannelSplitterGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (number_of_outputs != new_desc.number_of_outputs)
        return GraphUpdateKind::Topology;
    return GraphUpdateKind::None;
}

}
