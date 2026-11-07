/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/MediaStreamSourceRenderNode.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio::Rendering {

MediaStreamSourceRenderNode::MediaStreamSourceRenderNode(NodeID node_id, size_t quantum_size)
    : RenderNode(RustFFI::web_audio_node_create_media_stream_source(node_id.value(), quantum_size, {
                                                                                                       [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->channel_count(); },
                                                                                                       [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->frames_available(); },
                                                                                                       [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->frames_free(); },
                                                                                                       [](void* ring, Span<float> samples) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->try_pop(samples); },
                                                                                                       [](void* ring, ReadonlySpan<float> samples) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->try_push(samples); },
                                                                                                   }))
{
}

}
