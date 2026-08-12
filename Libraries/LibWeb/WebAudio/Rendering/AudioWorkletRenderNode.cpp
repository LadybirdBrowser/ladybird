/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/AudioWorkletRenderNode.h>
#include <LibWeb/WebAudio/Rendering/RenderGraph.h>

namespace Web::WebAudio::Rendering {

AudioWorkletRenderNode::AudioWorkletRenderNode(NodeID node_id, size_t input_count, size_t output_count, size_t quantum_size,
    Vector<u32> output_channel_capacities,
    size_t channel_count, Bindings::ChannelCountMode channel_count_mode, Bindings::ChannelInterpretation channel_interpretation,
    NonnullRefPtr<AudioWorkletPipe> pipe,
    Vector<NonnullRefPtr<RenderAudioParam>> params)
    : RenderNode(node_id, input_count, output_count, quantum_size, /* output_channel_count */ 1)
    , m_pipe(move(pipe))
    , m_params(move(params))
    , m_output_channel_capacities(move(output_channel_capacities))
{
    // Channel configuration must be set here: the SetChannelConfig messages queued during node
    // construction arrive before our AddNode and are dropped by the graph.
    set_channel_count(channel_count);
    set_channel_count_mode(channel_count_mode);
    set_channel_interpretation(channel_interpretation);
    m_param_scratch.resize(quantum_size);
}

AudioWorkletRenderNode::~AudioWorkletRenderNode() = default;

void AudioWorkletRenderNode::for_each_param(Function<void(RenderAudioParam&)> const& callback)
{
    for (auto& param : m_params)
        callback(*param);
}

void AudioWorkletRenderNode::process(RenderGraph& graph, RenderContext const& context)
{
    // A failed (processor threw) or shut-down (node finalized / context closed) worklet renders
    // silence forever; the pump side owns the state transitions.
    if (m_pipe->state() != AudioWorkletPipe::State::Running) {
        for (size_t output = 0; output < output_count(); ++output)
            this->output(output).zero();
        return;
    }

    (void)m_pipe->try_push_input([&](AudioWorkletPipe::InputSlotWriter& slot) {
        slot.start_frame() = context.quantum_start_frame;
        for (size_t input = 0; input < input_count(); ++input) {
            // Unlike the render graph's silent mono bus, an unconnected worklet input
            // is represented by an empty channel array in process().
            if (input_connections(input).is_empty()) {
                slot.actual_channel_count(input) = 0;
                continue;
            }
            auto const& input_bus = pull_input(graph, context, input);
            auto capacity_channels = m_pipe->config().input_channel_capacity[input];
            auto channels_to_copy = min(input_bus.channel_count(), static_cast<size_t>(capacity_channels));
            slot.actual_channel_count(input) = channels_to_copy;
            for (size_t channel = 0; channel < channels_to_copy; ++channel) {
                auto destination = slot.input_channel(input, channel);
                auto source = input_bus.channel(channel);
                VERIFY(destination.size() == source.size());
                __builtin_memcpy(destination.data(), source.data(), source.size() * sizeof(float));
            }
        }
        for (size_t param_index = 0; param_index < m_params.size(); ++param_index) {
            auto block = slot.param_block(param_index);
            // a-rate params fill the whole block; k-rate blocks are a single value. compute() handles
            // both through the shared timeline + any audio-rate param connections.
            m_param_scratch.resize(block.size());
            m_params[param_index]->compute(graph, context, m_param_scratch.span().slice(0, block.size()));
            __builtin_memcpy(block.data(), m_param_scratch.data(), block.size() * sizeof(float));
        }
    });
    m_pipe->request_wakeup();

    bool popped = m_pipe->try_pop_output([&](AudioWorkletPipe::OutputSlotReader const& slot) {
        for (size_t output = 0; output < output_count(); ++output) {
            auto& output_bus = this->output(output);
            auto channel_count = min(static_cast<size_t>(slot.actual_channel_count(output)), static_cast<size_t>(m_output_channel_capacities[output]));
            output_bus.set_channel_count(max(channel_count, static_cast<size_t>(1)));
            output_bus.zero();
            for (size_t channel = 0; channel < channel_count; ++channel) {
                auto source = slot.output_channel(output, channel);
                auto destination = output_bus.channel(channel);
                auto frames = min(source.size(), destination.size());
                __builtin_memcpy(destination.data(), source.data(), frames * sizeof(float));
            }
        }
    });
    if (!popped) {
        for (size_t output = 0; output < output_count(); ++output)
            this->output(output).zero();
        return;
    }

    // Latency clamp: after a main-thread stall, the pump refills the output ring past the configured
    // lookahead (the audio side emitted silence meanwhile). Discard the excess so the stall does not
    // permanently ratchet up this node's latency.
    if (m_pipe->output_occupancy() > m_pipe->config().prime_level)
        m_pipe->discard_outputs_down_to(m_pipe->config().prime_level);
}

}
