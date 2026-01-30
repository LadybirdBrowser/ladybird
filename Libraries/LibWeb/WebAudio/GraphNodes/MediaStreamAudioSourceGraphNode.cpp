/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/MediaStreamAudioSourceGraphNode.h>

#include <LibWeb/WebAudio/Engine/GraphResources.h>
#include <LibWeb/WebAudio/RenderNodes/MediaStreamAudioSourceRenderNode.h>
#include <LibWeb/WebAudio/RenderNodes/OhNoesRenderNode.h>

namespace Web::WebAudio::Render {

ErrorOr<void> MediaStreamAudioSourceGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    return encoder.append_u64(provider_id);
}

ErrorOr<MediaStreamAudioSourceGraphNode> MediaStreamAudioSourceGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    MediaStreamAudioSourceGraphNode node;
    node.provider_id = TRY(decoder.read_u64());
    return node;
}

OwnPtr<RenderNode> MediaStreamAudioSourceGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const& resources) const
{
    ASSERT_CONTROL_THREAD();
    auto provider = resources.resolve_media_stream_audio_source(provider_id);
    if (!provider)
        return make<OhNoesRenderNode>(node_id, quantum_size);
    return make<MediaStreamAudioSourceRenderNode>(node_id, provider.release_nonnull(), quantum_size);
}

GraphUpdateKind MediaStreamAudioSourceGraphNode::classify_update(MediaStreamAudioSourceGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (provider_id != new_desc.provider_id)
        return GraphUpdateKind::RebuildRequired;
    return GraphUpdateKind::None;
}

}
