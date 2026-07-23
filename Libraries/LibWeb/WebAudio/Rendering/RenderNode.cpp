/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/WebAudio/Rendering/RenderGraph.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>

namespace Web::WebAudio::Rendering {

RenderAudioParam::RenderAudioParam(NonnullRefPtr<AudioParamTimeline> timeline, float min_value, float max_value, Bindings::AutomationRate automation_rate)
    : m_timeline(move(timeline))
    , m_min_value(min_value)
    , m_max_value(max_value)
    , m_automation_rate(automation_rate)
{
}

// https://webaudio.github.io/web-audio-api/#computation-of-value
void RenderAudioParam::compute(RenderGraph& graph, RenderContext const& context, Span<float> output)
{
    // 1. paramIntrinsicValue will be calculated at each time, which is either the value set directly to the value
    //    attribute, or, if there are any automation events with times before or at this time, the value as calculated
    //    from these events.
    // NB: An a-rate parameter takes the exact value of the signal at each sample frame of the render quantum, while a
    //     k-rate parameter uses the value of the very first sample frame for the entire quantum.
    auto rate = automation_rate();
    if (rate == Bindings::AutomationRate::KRate) {
        auto value = m_timeline->value_at_time(context.quantum_start_time());
        output.fill(value);
    } else {
        m_timeline->sample(context.quantum_start_time(), context.sample_period(), output);
    }

    // 2. Set [[current value]] to the value of paramIntrinsicValue at the beginning of this render quantum.
    // NB: The render param keeps no [[current value]] slot of its own; the AudioParam's value getter reads the shared
    //     automation timeline directly on the control thread.

    // 3. paramComputedValue is the sum of the paramIntrinsicValue value and the value of the input AudioParam
    //    buffer. If the sum is NaN, replace the sum with the defaultValue.
    for (auto const& input : m_inputs) {
        auto const* bus = graph.pull_output(input.source, input.output_index, context);
        if (!bus)
            continue;

        // The AudioParam will take the rendered audio data from any AudioNode output connected to it and convert it to
        // mono by down-mixing if it is not already mono.
        if (!m_input_scratch || m_input_scratch->frame_count() != context.quantum_size)
            m_input_scratch = make<AudioBus>(1, context.quantum_size);
        m_input_scratch->zero();
        m_input_scratch->sum_from(*bus, Bindings::ChannelInterpretation::Speakers);

        auto input_samples = m_input_scratch->channel(0);
        if (rate == Bindings::AutomationRate::KRate) {
            for (auto& value : output)
                value += input_samples[0];
        } else {
            for (size_t frame = 0; frame < output.size(); ++frame)
                output[frame] += input_samples[frame];
        }
    }
    auto default_value = m_timeline->default_value();
    for (auto& value : output) {
        if (isnan(value))
            value = default_value;
    }

    // 4. If this AudioParam is a compound parameter, compute its final value with other AudioParams.
    // NB: None of the AudioParams backed by a render node are compound parameters.

    // 5. Set computedValue to paramComputedValue.

    // NB: The computedValue is clamped to the simple nominal range for this parameter.
    // https://webaudio.github.io/web-audio-api/#simple-nominal-range
    for (auto& value : output)
        value = clamp(value, m_min_value, m_max_value);
}

RenderNode::RenderNode(NodeID node_id, size_t input_count, size_t output_count, size_t quantum_size, size_t output_channel_count)
    : m_node_id(node_id)
{
    m_input_connections.resize(input_count);
    m_input_buses.ensure_capacity(input_count);
    for (size_t input_index = 0; input_index < input_count; ++input_index)
        m_input_buses.unchecked_append(AudioBus { 1, quantum_size });
    m_outputs.ensure_capacity(output_count);
    for (size_t output_index = 0; output_index < output_count; ++output_index)
        m_outputs.unchecked_append(AudioBus { output_channel_count, quantum_size });
}

void RenderNode::set_outgoing_connections(Vector<OutgoingNodeConnection> node_connections, Vector<OutgoingParamConnection> param_connections)
{
    m_outgoing_node_connections = move(node_connections);
    m_outgoing_param_connections = move(param_connections);
}

// Pulls and mixes all connections to the given input into a single bus with the input's computed number of channels.
// Unconnected inputs are mixed as a single channel of silence.
// https://webaudio.github.io/web-audio-api/#computednumberofchannels
AudioBus const& RenderNode::pull_input(RenderGraph& graph, RenderContext const& context, size_t input_index)
{
    auto& connections = m_input_connections[input_index];
    auto& input_bus = m_input_buses[input_index];

    auto& source_buses = m_pulled_source_buses;
    source_buses.clear_with_capacity();
    for (auto const& connection : connections) {
        if (auto const* bus = graph.pull_output(connection.source, connection.output_index, context))
            source_buses.append(bus);
    }

    if (source_buses.is_empty()) {
        input_bus.set_channel_count(1);
        input_bus.zero();
        return input_bus;
    }

    size_t computed_number_of_channels = 0;
    switch (m_channel_count_mode) {
    case Bindings::ChannelCountMode::Max:
    case Bindings::ChannelCountMode::ClampedMax:
        // "max": computedNumberOfChannels is the maximum of the number of channels of all connections to an input.
        //        In this mode channelCount is ignored.
        // "clamped-max": computedNumberOfChannels is determined as for "max" and then clamped to a maximum value of
        //                the given channelCount.
        for (auto const* bus : source_buses)
            computed_number_of_channels = max(computed_number_of_channels, bus->channel_count());
        if (m_channel_count_mode == Bindings::ChannelCountMode::ClampedMax)
            computed_number_of_channels = min(computed_number_of_channels, m_channel_count);
        break;
    case Bindings::ChannelCountMode::Explicit:
        // "explicit": computedNumberOfChannels is the exact value as specified by the channelCount.
        computed_number_of_channels = m_channel_count;
        break;
    }

    input_bus.set_channel_count(computed_number_of_channels);
    input_bus.zero();
    for (auto const* bus : source_buses)
        input_bus.sum_from(*bus, m_channel_interpretation);
    return input_bus;
}

}
