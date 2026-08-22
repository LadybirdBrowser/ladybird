/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/AnalyserRenderNode.h>
#include <LibWeb/WebAudio/Rendering/RenderGraph.h>

namespace Web::WebAudio::Rendering {

AnalyserRenderNode::AnalyserRenderNode(NodeID node_id, size_t quantum_size, NonnullRefPtr<Media::SpscAudioFrameRing> time_domain_ring)
    : RenderNode(node_id, 1, 1, quantum_size)
    , m_time_domain_ring(move(time_domain_ring))
{
    VERIFY(m_time_domain_ring->channel_count() == 1);

    // NB: The control thread queues SetChannelConfig before AddNode and messages addressed to
    //     unknown nodes are dropped, so the constructor must establish the node's channel
    //     configuration itself.
    // https://webaudio.github.io/web-audio-api/#AnalyserNode
    set_channel_count(2);
    set_channel_count_mode(Bindings::ChannelCountMode::Max);
    set_channel_interpretation(Bindings::ChannelInterpretation::Speakers);

    m_mono_scratch.resize(quantum_size);
    m_discard_scratch.resize(quantum_size);
}

void AnalyserRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    auto const& input = pull_input(graph, context, 0);

    // The AnalyserNode passes its input through unprocessed.
    output(0).copy_from(input);

    // https://webaudio.github.io/web-audio-api/#current-time-domain-data
    // The input signal must be down-mixed to mono as if channelCount is 1, channelCountMode
    // is "max" and channelInterpretation is "speakers".
    // AD-HOC: The down-mix is an unweighted channel average, which matches the speakers rules
    //         for mono and stereo input; inputs with more channels would need the weighted
    //         speakers down-mix.
    auto channel_count = input.channel_count();
    for (size_t frame = 0; frame < context.quantum_size; ++frame) {
        float sum = 0;
        for (size_t channel_index = 0; channel_index < channel_count; ++channel_index)
            sum += input.channel(channel_index)[frame];
        m_mono_scratch[frame] = channel_count > 0 ? sum / static_cast<float>(channel_count) : 0.f;
    }

    // The ring must always end up holding the most recent samples, so a full ring (the page
    // is not currently polling any of the analyser getters) drops its oldest quantum first.
    // NB: Popping from the producing side bends the SPSC discipline. It can only race with
    //     the control thread while the ring is simultaneously full and being drained by a
    //     getter, in which case the worst outcome is one repeated or skipped quantum in the
    //     analysis window; the ring's masked indices keep the race memory-safe.
    if (m_time_domain_ring->frames_free() < context.quantum_size)
        m_time_domain_ring->try_pop(m_discard_scratch);
    m_time_domain_ring->try_push(m_mono_scratch);
}

}
