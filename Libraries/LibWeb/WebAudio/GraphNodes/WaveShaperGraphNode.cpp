/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/WaveShaperGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/WaveShaperRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> WaveShaperGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    if (curve.size() > NumericLimits<u32>::max())
        return Error::from_string_literal("WaveShaper curve too large for wire encoding");

    TRY(encoder.append_u8(static_cast<u8>(oversample)));
    TRY(encoder.append_u32(static_cast<u32>(channel_count)));
    TRY(encoder.append_u8(static_cast<u8>(channel_count_mode)));
    TRY(encoder.append_u8(static_cast<u8>(channel_interpretation)));
    TRY(encoder.append_u32(static_cast<u32>(curve.size())));
    for (auto value : curve)
        TRY(encoder.append_f32(value));
    return {};
}

ErrorOr<WaveShaperGraphNode> WaveShaperGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    WaveShaperGraphNode node;
    node.oversample = static_cast<OverSampleType>(TRY(decoder.read_u8()));
    node.channel_count = TRY(decoder.read_u32());
    node.channel_count_mode = static_cast<ChannelCountMode>(TRY(decoder.read_u8()));
    node.channel_interpretation = static_cast<ChannelInterpretation>(TRY(decoder.read_u8()));

    auto curve_size = TRY(decoder.read_u32());
    TRY(node.curve.try_resize(curve_size));
    for (u32 i = 0; i < curve_size; ++i)
        node.curve[i] = TRY(decoder.read_f32());

    return node;
}

OwnPtr<RenderNode> WaveShaperGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<WaveShaperRenderNode>(node_id, *this, quantum_size);
}

GraphUpdateKind WaveShaperGraphNode::classify_update(WaveShaperGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (channel_count != new_desc.channel_count)
        return GraphUpdateKind::Topology;
    if (channel_count_mode != new_desc.channel_count_mode)
        return GraphUpdateKind::Topology;
    if (channel_interpretation != new_desc.channel_interpretation)
        return GraphUpdateKind::Topology;

    if (oversample != new_desc.oversample)
        return GraphUpdateKind::RebuildRequired;

    if (curve.size() != new_desc.curve.size())
        return GraphUpdateKind::RebuildRequired;

    for (size_t i = 0; i < curve.size(); ++i) {
        if (curve[i] != new_desc.curve[i])
            return GraphUpdateKind::RebuildRequired;
    }

    return GraphUpdateKind::None;
}

}
