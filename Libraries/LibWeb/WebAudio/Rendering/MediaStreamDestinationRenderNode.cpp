/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/WebAudio/Rendering/MediaStreamDestinationRenderNode.h>
#include <LibWeb/WebAudio/Rendering/RenderGraph.h>

namespace Web::WebAudio::Rendering {

MediaStreamDestinationRenderNode::MediaStreamDestinationRenderNode(NodeID node_id, size_t quantum_size, size_t channel_count, NonnullRefPtr<MediaStreamDestinationSharedState> shared_state, Core::EventLoop& control_thread_event_loop)
    : RenderNode(node_id, 1, 1, quantum_size, channel_count)
    , m_shared_state(move(shared_state))
    , m_control_thread_event_loop(control_thread_event_loop)
    , m_mix_bus(channel_count, quantum_size)
{
    // NB: The control thread queues SetChannelConfig before AddNode and messages addressed to
    //     unknown nodes are dropped, so the constructor must establish the node's channel
    //     configuration itself.
    // https://webaudio.github.io/web-audio-api/#MediaStreamAudioDestinationNode
    set_channel_count(channel_count);
    set_channel_count_mode(Bindings::ChannelCountMode::Explicit);
    set_channel_interpretation(Bindings::ChannelInterpretation::Speakers);

    m_interleaved_scratch.resize(quantum_size * channel_count);
}

void MediaStreamDestinationRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    auto const& input = pull_input(graph, context, 0);

    // AD-HOC: The spec gives this node zero outputs, but RenderGraph only pulls nodes that
    //         have at least one, so the render node exposes its input as a passthrough
    //         output. This keeps the node processing every quantum and also makes it audible
    //         when a page connects it onward.
    output(0).copy_from(input);

    // Mix the input to the track's channel count and interleave it into the shared ring. If
    // the control thread has stalled and the ring is full, the newest frames are dropped; the
    // consumer resynchronizes when it drains.
    m_mix_bus.zero();
    m_mix_bus.sum_from(input, channel_interpretation());
    auto channel_count = m_mix_bus.channel_count();
    for (size_t frame = 0; frame < context.quantum_size; ++frame) {
        for (size_t channel_index = 0; channel_index < channel_count; ++channel_index)
            m_interleaved_scratch[frame * channel_count + channel_index] = m_mix_bus.channel(channel_index)[frame];
    }
    m_shared_state->ring->try_push(m_interleaved_scratch);

    // Coalesced render→control notification, following the renderer's ended-sources pattern:
    // at most one queued drain per rendered burst, so the control thread wakes up ~10 times
    // per second rather than once per quantum.
    if (!m_shared_state->drain_pending.exchange(true, AK::MemoryOrder::memory_order_acq_rel)) {
        m_control_thread_event_loop.deferred_invoke([shared_state = m_shared_state] {
            shared_state->drain_pending.store(false, AK::MemoryOrder::memory_order_release);
            if (shared_state->drain)
                shared_state->drain();
        });
    }
}

}
