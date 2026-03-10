/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>
#include <LibWeb/WebAudio/GraphNodes/MediaElementAudioSourceGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/MediaElementAudioSourceRenderNode.h>
#include <LibWeb/WebAudio/RenderNodes/OhNoesRenderNode.h>

namespace Web::WebAudio::Render {

static u32 clamp_size_to_u32(size_t value)
{
    ASSERT_CONTROL_THREAD();
    if (value > AK::NumericLimits<u32>::max())
        return AK::NumericLimits<u32>::max();
    return static_cast<u32>(value);
}

ErrorOr<void> MediaElementAudioSourceGraphNode::encode_wire_payload(WireEncoder& encoder) const
{
    ASSERT_CONTROL_THREAD();
    TRY(encoder.append_u32(clamp_size_to_u32(channel_count)));
    return encoder.append_u64(provider_id);
}

ErrorOr<MediaElementAudioSourceGraphNode> MediaElementAudioSourceGraphNode::decode_wire_payload(WireDecoder& decoder)
{
    ASSERT_CONTROL_THREAD();
    MediaElementAudioSourceGraphNode node;
    node.channel_count = TRY(decoder.read_u32());
    node.provider_id = TRY(decoder.read_u64());
    return node;
}

OwnPtr<RenderNode> MediaElementAudioSourceGraphNode::make_render_node(NodeID node_id, size_t quantum_size, GraphResourceResolver const& resources) const
{
    ASSERT_CONTROL_THREAD();
    auto provider = resources.resolve_media_element_audio_source(provider_id);
    if (!provider)
        return make<OhNoesRenderNode>(node_id, quantum_size);
    return make<MediaElementAudioSourceRenderNode>(node_id, provider.release_nonnull(), quantum_size);
}

GraphUpdateKind MediaElementAudioSourceGraphNode::classify_update(MediaElementAudioSourceGraphNode const& new_desc) const
{
    ASSERT_CONTROL_THREAD();
    if (provider_id != new_desc.provider_id)
        return GraphUpdateKind::RebuildRequired;
    return GraphUpdateKind::None;
}

}
