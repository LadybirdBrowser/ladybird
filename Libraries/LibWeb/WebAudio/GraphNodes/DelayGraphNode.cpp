/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/DelayGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/DelayRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> DelayGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_f32(delay_time_seconds));
    TRY(encoder.append_f32(max_delay_time_seconds));

    size_t const clamped = min(channel_count, static_cast<size_t>(AK::NumericLimits<u32>::max()));
    TRY(encoder.append_u32(static_cast<u32>(clamped)));

    TRY(encoder.append_u8(static_cast<u8>(channel_count_mode)));
    TRY(encoder.append_u8(static_cast<u8>(channel_interpretation)));
    return {};
}

ErrorOr<DelayGraphNode> DelayGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    DelayGraphNode node;
    node.delay_time_seconds = TRY(decoder.read_f32());
    node.max_delay_time_seconds = TRY(decoder.read_f32());
    node.channel_count = TRY(decoder.read_u32());
    node.channel_count_mode = static_cast<ChannelCountMode>(TRY(decoder.read_u8()));
    node.channel_interpretation = static_cast<ChannelInterpretation>(TRY(decoder.read_u8()));
    return node;
}

OwnPtr<RenderNode> DelayGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<DelayRenderNode>(node_id, *this, quantum_size);
}

GraphUpdateKind DelayGraphNode::classify_update(DelayGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (max_delay_time_seconds != new_desc.max_delay_time_seconds)
        return GraphUpdateKind::RebuildRequired;

    if (channel_count != new_desc.channel_count)
        return GraphUpdateKind::Topology;

    if (channel_count_mode != new_desc.channel_count_mode)
        return GraphUpdateKind::Topology;
    if (channel_interpretation != new_desc.channel_interpretation)
        return GraphUpdateKind::Topology;

    if (delay_time_seconds != new_desc.delay_time_seconds)
        return GraphUpdateKind::Parameter;

    return GraphUpdateKind::None;
}

}
