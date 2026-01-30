/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitCast.h>
#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/AudioListenerGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/AudioListenerRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> AudioListenerGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_f32(position_x));
    TRY(encoder.append_f32(position_y));
    TRY(encoder.append_f32(position_z));
    TRY(encoder.append_f32(forward_x));
    TRY(encoder.append_f32(forward_y));
    TRY(encoder.append_f32(forward_z));
    TRY(encoder.append_f32(up_x));
    TRY(encoder.append_f32(up_y));
    TRY(encoder.append_f32(up_z));
    return {};
}

ErrorOr<AudioListenerGraphNode> AudioListenerGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    AudioListenerGraphNode node;
    node.position_x = TRY(decoder.read_f32());
    node.position_y = TRY(decoder.read_f32());
    node.position_z = TRY(decoder.read_f32());
    node.forward_x = TRY(decoder.read_f32());
    node.forward_y = TRY(decoder.read_f32());
    node.forward_z = TRY(decoder.read_f32());
    node.up_x = TRY(decoder.read_f32());
    node.up_y = TRY(decoder.read_f32());
    node.up_z = TRY(decoder.read_f32());
    return node;
}

OwnPtr<RenderNode> AudioListenerGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<AudioListenerRenderNode>(node_id, quantum_size, *this);
}

GraphUpdateKind AudioListenerGraphNode::classify_update(AudioListenerGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    auto differs = [](f32 a, f32 b) {
        return bit_cast<u32>(a) != bit_cast<u32>(b);
    };

    if (differs(position_x, new_desc.position_x)
        || differs(position_y, new_desc.position_y)
        || differs(position_z, new_desc.position_z)
        || differs(forward_x, new_desc.forward_x)
        || differs(forward_y, new_desc.forward_y)
        || differs(forward_z, new_desc.forward_z)
        || differs(up_x, new_desc.up_x)
        || differs(up_y, new_desc.up_y)
        || differs(up_z, new_desc.up_z)) {
        return GraphUpdateKind::Parameter;
    }

    return GraphUpdateKind::None;
}

}
