/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/ScriptProcessorGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/ScriptProcessorRenderNode.h>

namespace Web::WebAudio::Render {

static u32 clamp_size_to_u32(size_t value)
{
    ASSERT_CONTROL_THREAD();
    if (value > AK::NumericLimits<u32>::max())
        return AK::NumericLimits<u32>::max();
    return static_cast<u32>(value);
}

ErrorOr<void> ScriptProcessorGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_u32(clamp_size_to_u32(buffer_size)));
    TRY(encoder.append_u32(clamp_size_to_u32(input_channel_count)));
    TRY(encoder.append_u32(clamp_size_to_u32(output_channel_count)));
    return {};
}

ErrorOr<ScriptProcessorGraphNode> ScriptProcessorGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    ScriptProcessorGraphNode node;
    node.buffer_size = TRY(decoder.read_u32());
    node.input_channel_count = TRY(decoder.read_u32());
    node.output_channel_count = TRY(decoder.read_u32());
    return node;
}

OwnPtr<RenderNode> ScriptProcessorGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<ScriptProcessorRenderNode>(node_id, *this, quantum_size);
}

GraphUpdateKind ScriptProcessorGraphNode::classify_update(ScriptProcessorGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (buffer_size == new_desc.buffer_size
        && input_channel_count == new_desc.input_channel_count
        && output_channel_count == new_desc.output_channel_count)
        return GraphUpdateKind::None;

    return GraphUpdateKind::RebuildRequired;
}

}
