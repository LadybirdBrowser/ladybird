/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/DynamicsCompressorGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/DynamicsCompressorRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> DynamicsCompressorGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_f32(threshold_db));
    TRY(encoder.append_f32(knee_db));
    TRY(encoder.append_f32(ratio));
    TRY(encoder.append_f32(attack_seconds));
    TRY(encoder.append_f32(release_seconds));

    size_t const clamped = min(channel_count, static_cast<size_t>(AK::NumericLimits<u32>::max()));
    TRY(encoder.append_u32(static_cast<u32>(clamped)));
    TRY(encoder.append_u8(static_cast<u8>(channel_count_mode)));
    TRY(encoder.append_u8(static_cast<u8>(channel_interpretation)));
    return {};
}

ErrorOr<DynamicsCompressorGraphNode> DynamicsCompressorGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    DynamicsCompressorGraphNode node;
    node.threshold_db = TRY(decoder.read_f32());
    node.knee_db = TRY(decoder.read_f32());
    node.ratio = TRY(decoder.read_f32());
    node.attack_seconds = TRY(decoder.read_f32());
    node.release_seconds = TRY(decoder.read_f32());

    node.channel_count = TRY(decoder.read_u32());
    node.channel_count_mode = static_cast<ChannelCountMode>(TRY(decoder.read_u8()));
    node.channel_interpretation = static_cast<ChannelInterpretation>(TRY(decoder.read_u8()));
    return node;
}

OwnPtr<RenderNode> DynamicsCompressorGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<DynamicsCompressorRenderNode>(node_id, *this, quantum_size);
}

GraphUpdateKind DynamicsCompressorGraphNode::classify_update(DynamicsCompressorGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (channel_count != new_desc.channel_count)
        return GraphUpdateKind::Topology;
    if (channel_count_mode != new_desc.channel_count_mode)
        return GraphUpdateKind::Topology;
    if (channel_interpretation != new_desc.channel_interpretation)
        return GraphUpdateKind::Topology;

    if (threshold_db != new_desc.threshold_db)
        return GraphUpdateKind::Parameter;
    if (knee_db != new_desc.knee_db)
        return GraphUpdateKind::Parameter;
    if (ratio != new_desc.ratio)
        return GraphUpdateKind::Parameter;
    if (attack_seconds != new_desc.attack_seconds)
        return GraphUpdateKind::Parameter;
    if (release_seconds != new_desc.release_seconds)
        return GraphUpdateKind::Parameter;

    return GraphUpdateKind::None;
}

}
