/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/IIRFilterGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/IIRFilterRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> IIRFilterGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    if (feedforward.size() > NumericLimits<u32>::max())
        return Error::from_string_literal("IIRFilter feedforward too large for wire encoding");
    if (feedback.size() > NumericLimits<u32>::max())
        return Error::from_string_literal("IIRFilter feedback too large for wire encoding");

    TRY(encoder.append_u32(static_cast<u32>(channel_count)));
    TRY(encoder.append_u8(static_cast<u8>(channel_count_mode)));
    TRY(encoder.append_u8(static_cast<u8>(channel_interpretation)));

    TRY(encoder.append_u32(static_cast<u32>(feedforward.size())));
    for (auto value : feedforward)
        TRY(encoder.append_f64(value));

    TRY(encoder.append_u32(static_cast<u32>(feedback.size())));
    for (auto value : feedback)
        TRY(encoder.append_f64(value));

    return {};
}

ErrorOr<IIRFilterGraphNode> IIRFilterGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    IIRFilterGraphNode node;
    node.channel_count = TRY(decoder.read_u32());
    node.channel_count_mode = static_cast<ChannelCountMode>(TRY(decoder.read_u8()));
    node.channel_interpretation = static_cast<ChannelInterpretation>(TRY(decoder.read_u8()));

    auto feedforward_size = TRY(decoder.read_u32());
    TRY(node.feedforward.try_resize(feedforward_size));
    for (u32 i = 0; i < feedforward_size; ++i)
        node.feedforward[i] = TRY(decoder.read_f64());

    auto feedback_size = TRY(decoder.read_u32());
    TRY(node.feedback.try_resize(feedback_size));
    for (u32 i = 0; i < feedback_size; ++i)
        node.feedback[i] = TRY(decoder.read_f64());

    return node;
}

OwnPtr<RenderNode> IIRFilterGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<IIRFilterRenderNode>(node_id, *this, quantum_size);
}

GraphUpdateKind IIRFilterGraphNode::classify_update(IIRFilterGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (channel_count != new_desc.channel_count)
        return GraphUpdateKind::Topology;
    if (channel_count_mode != new_desc.channel_count_mode)
        return GraphUpdateKind::Topology;
    if (channel_interpretation != new_desc.channel_interpretation)
        return GraphUpdateKind::Topology;

    if (feedforward.size() != new_desc.feedforward.size())
        return GraphUpdateKind::RebuildRequired;
    if (feedback.size() != new_desc.feedback.size())
        return GraphUpdateKind::RebuildRequired;

    for (size_t i = 0; i < feedforward.size(); ++i) {
        if (feedforward[i] != new_desc.feedforward[i])
            return GraphUpdateKind::RebuildRequired;
    }

    for (size_t i = 0; i < feedback.size(); ++i) {
        if (feedback[i] != new_desc.feedback[i])
            return GraphUpdateKind::RebuildRequired;
    }

    return GraphUpdateKind::None;
}

}
