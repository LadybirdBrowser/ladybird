/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/AnalyserGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/AnalyserRenderNode.h>

namespace Web::WebAudio::Render {

static u32 clamp_size_to_u32(size_t value)
{
    ASSERT_CONTROL_THREAD();
    if (value > AK::NumericLimits<u32>::max())
        return AK::NumericLimits<u32>::max();
    return static_cast<u32>(value);
}

ErrorOr<void> AnalyserGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_u32(clamp_size_to_u32(channel_count)));
    TRY(encoder.append_u8(static_cast<u8>(channel_count_mode)));
    TRY(encoder.append_u8(static_cast<u8>(channel_interpretation)));
    TRY(encoder.append_u32(clamp_size_to_u32(fft_size)));
    TRY(encoder.append_f32(smoothing_time_constant));
    return {};
}

ErrorOr<AnalyserGraphNode> AnalyserGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    AnalyserGraphNode node;
    node.channel_count = TRY(decoder.read_u32());
    node.channel_count_mode = static_cast<ChannelCountMode>(TRY(decoder.read_u8()));
    node.channel_interpretation = static_cast<ChannelInterpretation>(TRY(decoder.read_u8()));
    node.fft_size = TRY(decoder.read_u32());
    node.smoothing_time_constant = TRY(decoder.read_f32());
    return node;
}

OwnPtr<RenderNode> AnalyserGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<AnalyserRenderNode>(node_id, *this, quantum_size);
}

GraphUpdateKind AnalyserGraphNode::classify_update(AnalyserGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (channel_count != new_desc.channel_count)
        return GraphUpdateKind::Topology;
    if (channel_count_mode != new_desc.channel_count_mode)
        return GraphUpdateKind::Topology;
    if (channel_interpretation != new_desc.channel_interpretation)
        return GraphUpdateKind::Topology;

    if (fft_size != new_desc.fft_size)
        return GraphUpdateKind::Parameter;

    if (smoothing_time_constant != new_desc.smoothing_time_constant)
        return GraphUpdateKind::Parameter;

    return GraphUpdateKind::None;
}

}
