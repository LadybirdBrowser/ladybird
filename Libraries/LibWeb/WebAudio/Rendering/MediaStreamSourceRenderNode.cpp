/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/MediaStreamSourceRenderNode.h>
#include <LibWeb/WebAudio/Rendering/RenderGraph.h>

namespace Web::WebAudio::Rendering {

MediaStreamSourceRenderNode::MediaStreamSourceRenderNode(NodeID node_id, size_t quantum_size)
    : RenderNode(node_id, 0, 1, quantum_size, 2)
{
    // NB: The control thread queues SetChannelConfig before AddNode and messages addressed to
    //     unknown nodes are dropped, so the constructor must establish the node's channel
    //     configuration itself.
    // https://webaudio.github.io/web-audio-api/#MediaStreamAudioSourceNode
    set_channel_count(2);
    set_channel_count_mode(Bindings::ChannelCountMode::Max);
    set_channel_interpretation(Bindings::ChannelInterpretation::Speakers);
}

void MediaStreamSourceRenderNode::handle_message(NodeMessage const& message)
{
    message.visit(
        [&](SetMediaStreamSourceRing const& set_ring) {
            m_ring = set_ring.ring;
            // NB: The ring's own channel count wins over the message field so the interleaved
            //     stride below can never disagree with the ring's layout.
            m_channel_count = m_ring ? m_ring->channel_count() : max(set_ring.channel_count, 1u);
        },
        [](auto const&) {});
}

void MediaStreamSourceRenderNode::process(RenderGraph&, RenderContext const& context)
{
    auto& source_output = output(0);

    // The number of channels of the output corresponds to the number of channels of the track.
    source_output.set_channel_count(m_channel_count);
    source_output.zero();
    if (!m_ring)
        return;

    // Drain the ring sequentially, up to one render quantum. When the producer has not
    // delivered enough frames yet (startup, scheduling jitter), the tail of the quantum stays
    // silent; skipping ahead to "the most recent frames" instead would drift against the
    // producer's cadence and repeat or drop content.
    auto frames_to_pop = min(context.quantum_size, m_ring->frames_available());
    if (frames_to_pop == 0)
        return;

    m_interleaved_scratch.resize(frames_to_pop * m_channel_count);
    auto frames_popped = m_ring->try_pop(m_interleaved_scratch);

    for (size_t channel_index = 0; channel_index < m_channel_count; ++channel_index) {
        auto output_samples = source_output.channel(channel_index);
        for (size_t frame = 0; frame < frames_popped; ++frame)
            output_samples[frame] = m_interleaved_scratch[frame * m_channel_count + channel_index];
    }

    // The producer converts track samples to the context's sample rate before filling the ring.
}

}
