/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/GraphNodes/OscillatorGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/OscillatorRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> OscillatorGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    TRY(encoder.append_u8(static_cast<u8>(type)));
    TRY(encoder.append_f32(frequency));
    TRY(encoder.append_f32(detune_cents));
    TRY(append_optional_size_as_u64(encoder, start_frame));
    TRY(append_optional_size_as_u64(encoder, stop_frame));
    return {};
}

ErrorOr<OscillatorGraphNode> OscillatorGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    OscillatorGraphNode node;
    node.type = static_cast<OscillatorType>(TRY(decoder.read_u8()));
    node.frequency = TRY(decoder.read_f32());
    node.detune_cents = TRY(decoder.read_f32());
    node.start_frame = TRY(read_optional_size_from_u64(decoder));
    node.stop_frame = TRY(read_optional_size_from_u64(decoder));
    return node;
}

OwnPtr<RenderNode> OscillatorGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    return make<OscillatorRenderNode>(node_id, *this, quantum_size);
}

GraphUpdateKind OscillatorGraphNode::classify_update(OscillatorGraphNode const& new_desc) const
{
    if (type != new_desc.type)
        return GraphUpdateKind::RebuildRequired;

    if (frequency != new_desc.frequency)
        return GraphUpdateKind::Parameter;
    if (detune_cents != new_desc.detune_cents)
        return GraphUpdateKind::Parameter;
    if (start_frame != new_desc.start_frame)
        return GraphUpdateKind::Parameter;
    if (stop_frame != new_desc.stop_frame)
        return GraphUpdateKind::Parameter;

    return GraphUpdateKind::None;
}

}
