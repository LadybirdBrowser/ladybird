/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/GainGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/GainRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> GainGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_f32(gain));
    TRY(encoder.append_u32(static_cast<u32>(channel_count)));
    TRY(encoder.append_u8(static_cast<u8>(channel_count_mode)));
    TRY(encoder.append_u8(static_cast<u8>(channel_interpretation)));
    return {};
}

ErrorOr<GainGraphNode> GainGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    GainGraphNode node;
    node.gain = TRY(decoder.read_f32());
    node.channel_count = TRY(decoder.read_u32());
    node.channel_count_mode = static_cast<ChannelCountMode>(TRY(decoder.read_u8()));
    node.channel_interpretation = static_cast<ChannelInterpretation>(TRY(decoder.read_u8()));
    return node;
}

OwnPtr<RenderNode> GainGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<GainRenderNode>(node_id, *this, quantum_size);
}

GraphUpdateKind GainGraphNode::classify_update(GainGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (channel_count != new_desc.channel_count)
        return GraphUpdateKind::Topology;
    if (channel_count_mode != new_desc.channel_count_mode)
        return GraphUpdateKind::Topology;
    if (channel_interpretation != new_desc.channel_interpretation)
        return GraphUpdateKind::Topology;

    if (gain != new_desc.gain)
        return GraphUpdateKind::Parameter;

    return GraphUpdateKind::None;
}

}
