/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/OhNoesGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/OhNoesRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> OhNoesGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_string(base_path.bytes_as_string_view()));
    TRY(encoder.append_u8(emit_enabled ? 1 : 0));
    TRY(encoder.append_u8(strip_zero_buffers ? 1 : 0));
    return {};
}

ErrorOr<OhNoesGraphNode> OhNoesGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    OhNoesGraphNode node;
    auto path = TRY(decoder.read_string());
    node.base_path = String::from_utf8_with_replacement_character(path.view());
    node.emit_enabled = TRY(decoder.read_u8()) != 0;
    node.strip_zero_buffers = TRY(decoder.read_u8()) != 0;
    return node;
}

OwnPtr<RenderNode> OhNoesGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<OhNoesRenderNode>(node_id, quantum_size, *this);
}

GraphUpdateKind OhNoesGraphNode::classify_update(OhNoesGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (base_path != new_desc.base_path)
        return GraphUpdateKind::RebuildRequired;
    if (emit_enabled != new_desc.emit_enabled)
        return GraphUpdateKind::Parameter;
    if (strip_zero_buffers != new_desc.strip_zero_buffers)
        return GraphUpdateKind::Parameter;

    return GraphUpdateKind::None;
}

}
