/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>
#include <LibWeb/WebAudio/GraphNodes/ConvolverGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/ConvolverRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> ConvolverGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_u8(normalize ? 1 : 0));
    TRY(encoder.append_u64(buffer_id));
    TRY(encoder.append_u32(static_cast<u32>(channel_count)));
    TRY(encoder.append_u8(static_cast<u8>(channel_count_mode)));
    TRY(encoder.append_u8(static_cast<u8>(channel_interpretation)));
    return {};
}

ErrorOr<ConvolverGraphNode> ConvolverGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    ConvolverGraphNode node;
    node.normalize = TRY(decoder.read_u8()) != 0;
    node.buffer_id = TRY(decoder.read_u64());
    node.channel_count = TRY(decoder.read_u32());
    node.channel_count_mode = static_cast<ChannelCountMode>(TRY(decoder.read_u8()));
    node.channel_interpretation = static_cast<ChannelInterpretation>(TRY(decoder.read_u8()));
    return node;
}

OwnPtr<RenderNode> ConvolverGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const& resources) const
{
    ASSERT_CONTROL_THREAD();
    RefPtr<SharedAudioBuffer> impulse;
    if (buffer_id != 0)
        impulse = resources.resolve_audio_buffer(buffer_id);
    return make<ConvolverRenderNode>(node_id, *this, move(impulse), quantum_size);
}

GraphUpdateKind ConvolverGraphNode::classify_update(ConvolverGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (buffer_id != new_desc.buffer_id)
        return GraphUpdateKind::RebuildRequired;
    if (channel_count != new_desc.channel_count)
        return GraphUpdateKind::Topology;
    if (channel_count_mode != new_desc.channel_count_mode)
        return GraphUpdateKind::Topology;
    if (channel_interpretation != new_desc.channel_interpretation)
        return GraphUpdateKind::Topology;

    if (normalize != new_desc.normalize)
        return GraphUpdateKind::Parameter;

    return GraphUpdateKind::None;
}

}
