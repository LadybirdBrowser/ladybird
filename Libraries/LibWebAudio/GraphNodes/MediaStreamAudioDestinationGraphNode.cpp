/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebAudio/GraphNodes/MediaStreamAudioDestinationGraphNode.h>

#include <LibWebAudio/Debug.h>
#include <LibWebAudio/Engine/GraphResources.h>
#include <LibWebAudio/RenderNodes/MediaStreamAudioDestinationRenderNode.h>
#include <LibWebAudio/RenderNodes/OhNoesRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> MediaStreamAudioDestinationGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    return encoder.append_u64(provider_id);
}

ErrorOr<MediaStreamAudioDestinationGraphNode>
MediaStreamAudioDestinationGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    MediaStreamAudioDestinationGraphNode node;
    node.provider_id = TRY(decoder.read_u64());
    return node;
}

OwnPtr<RenderNode> MediaStreamAudioDestinationGraphNode::make_render_node(NodeID node_id,
    size_t quantum_size, GraphResources const& resources) const
{
    ASSERT_CONTROL_THREAD();
    auto it = resources.media_stream_audio_sources.find(provider_id);
    if (it == resources.media_stream_audio_sources.end())
        return make<OhNoesRenderNode>(node_id, quantum_size);
    return make<MediaStreamAudioDestinationRenderNode>(node_id, it->value, quantum_size);
}

GraphUpdateKind MediaStreamAudioDestinationGraphNode::classify_update(
    MediaStreamAudioDestinationGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (provider_id != new_desc.provider_id)
        return GraphUpdateKind::RebuildRequired;
    return GraphUpdateKind::None;
}

} // namespace Web::WebAudio::Render