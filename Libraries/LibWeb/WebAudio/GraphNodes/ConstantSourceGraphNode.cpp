/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/ConstantSourceGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/ConstantSourceRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> ConstantSourceGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_f32(offset));
    TRY(append_optional_size_as_u64(encoder, start_frame));
    TRY(append_optional_size_as_u64(encoder, stop_frame));
    return {};
}

ErrorOr<ConstantSourceGraphNode> ConstantSourceGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    ConstantSourceGraphNode node;
    node.offset = TRY(decoder.read_f32());
    node.start_frame = TRY(read_optional_size_from_u64(decoder));
    node.stop_frame = TRY(read_optional_size_from_u64(decoder));
    return node;
}

OwnPtr<RenderNode> ConstantSourceGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<ConstantSourceRenderNode>(node_id, *this, quantum_size);
}

GraphUpdateKind ConstantSourceGraphNode::classify_update(ConstantSourceGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (offset != new_desc.offset)
        return GraphUpdateKind::Parameter;
    if (start_frame != new_desc.start_frame)
        return GraphUpdateKind::Parameter;
    if (stop_frame != new_desc.stop_frame)
        return GraphUpdateKind::Parameter;

    return GraphUpdateKind::None;
}

}
