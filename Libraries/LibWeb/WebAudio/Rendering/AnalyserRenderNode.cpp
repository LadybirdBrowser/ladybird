/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/AnalyserRenderNode.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio::Rendering {

AnalyserRenderNode::AnalyserRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<Audio::SpscAudioFrameRing> time_domain_ring)
    : RenderNode(RustFFI::web_audio_node_create_analyser(node_id.value(), quantum_size,
          { time_domain_ring.ptr(), [](void* ring) { static_cast<Audio::SpscAudioFrameRing*>(ring)->ref(); }, [](void* ring) { static_cast<Audio::SpscAudioFrameRing*>(ring)->unref(); } },
          {
              [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->channel_count(); },
              [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->frames_available(); },
              [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->frames_free(); },
              [](void* ring, Span<float> samples) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->try_pop(samples); },
              [](void* ring, ReadonlySpan<float> samples) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->try_push(samples); },
          }))
{
}

}
