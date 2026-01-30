/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/PannerGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/PannerRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> PannerGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_u8(static_cast<u8>(panning_model)));
    TRY(encoder.append_u8(static_cast<u8>(distance_model)));
    TRY(encoder.append_f64(ref_distance));
    TRY(encoder.append_f64(max_distance));
    TRY(encoder.append_f64(rolloff_factor));
    TRY(encoder.append_f64(cone_inner_angle));
    TRY(encoder.append_f64(cone_outer_angle));
    TRY(encoder.append_f64(cone_outer_gain));
    TRY(encoder.append_f32(position_x));
    TRY(encoder.append_f32(position_y));
    TRY(encoder.append_f32(position_z));
    TRY(encoder.append_f32(orientation_x));
    TRY(encoder.append_f32(orientation_y));
    TRY(encoder.append_f32(orientation_z));
    TRY(encoder.append_u32(static_cast<u32>(channel_count)));
    TRY(encoder.append_u8(static_cast<u8>(channel_count_mode)));
    TRY(encoder.append_u8(static_cast<u8>(channel_interpretation)));
    return {};
}

ErrorOr<PannerGraphNode> PannerGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    PannerGraphNode node;
    node.panning_model = static_cast<PanningModelType>(TRY(decoder.read_u8()));
    node.distance_model = static_cast<DistanceModelType>(TRY(decoder.read_u8()));
    node.ref_distance = TRY(decoder.read_f64());
    node.max_distance = TRY(decoder.read_f64());
    node.rolloff_factor = TRY(decoder.read_f64());
    node.cone_inner_angle = TRY(decoder.read_f64());
    node.cone_outer_angle = TRY(decoder.read_f64());
    node.cone_outer_gain = TRY(decoder.read_f64());
    node.position_x = TRY(decoder.read_f32());
    node.position_y = TRY(decoder.read_f32());
    node.position_z = TRY(decoder.read_f32());
    node.orientation_x = TRY(decoder.read_f32());
    node.orientation_y = TRY(decoder.read_f32());
    node.orientation_z = TRY(decoder.read_f32());
    node.channel_count = TRY(decoder.read_u32());
    node.channel_count_mode = static_cast<ChannelCountMode>(TRY(decoder.read_u8()));
    node.channel_interpretation = static_cast<ChannelInterpretation>(TRY(decoder.read_u8()));
    return node;
}

OwnPtr<RenderNode> PannerGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const&) const
{
    ASSERT_CONTROL_THREAD();
    return make<PannerRenderNode>(node_id, quantum_size, *this);
}

GraphUpdateKind PannerGraphNode::classify_update(PannerGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (channel_count != new_desc.channel_count || channel_count_mode != new_desc.channel_count_mode || channel_interpretation != new_desc.channel_interpretation)
        return GraphUpdateKind::Parameter;

    if (panning_model != new_desc.panning_model || distance_model != new_desc.distance_model)
        return GraphUpdateKind::Parameter;

    if (ref_distance != new_desc.ref_distance || max_distance != new_desc.max_distance || rolloff_factor != new_desc.rolloff_factor)
        return GraphUpdateKind::Parameter;

    if (cone_inner_angle != new_desc.cone_inner_angle || cone_outer_angle != new_desc.cone_outer_angle || cone_outer_gain != new_desc.cone_outer_gain)
        return GraphUpdateKind::Parameter;

    return GraphUpdateKind::None;
}

}
