/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/ChannelMergerGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/ChannelMergerRenderNode.h>

namespace Web::WebAudio::Render {

static u32 clamp_size_to_u32(size_t value)
{
    ASSERT_CONTROL_THREAD();
    if (value > AK::NumericLimits<u32>::max())
        return AK::NumericLimits<u32>::max();
    return static_cast<u32>(value);
}

ErrorOr<void> ChannelMergerGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    return encoder.append_u32(clamp_size_to_u32(number_of_inputs));
}

ErrorOr<ChannelMergerGraphNode> ChannelMergerGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    ChannelMergerGraphNode node;
    node.number_of_inputs = TRY(decoder.read_u32());
    return node;
}

OwnPtr<RenderNode> ChannelMergerGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<ChannelMergerRenderNode>(node_id, *this, quantum_size);
}

GraphUpdateKind ChannelMergerGraphNode::classify_update(ChannelMergerGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (number_of_inputs != new_desc.number_of_inputs)
        return GraphUpdateKind::Topology;
    return GraphUpdateKind::None;
}

}
