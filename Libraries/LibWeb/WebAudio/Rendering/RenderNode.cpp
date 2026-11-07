/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/RustFFI.h>
#include <LibWeb/WebAudio/Rendering/RenderGraph.h>
#include <LibWeb/WebAudio/Rendering/RenderNode.h>

namespace Web::WebAudio::Rendering {

RenderAudioParam::RenderAudioParam(NonnullRefPtr<AudioParamTimeline> timeline, float min_value, float max_value, Bindings::AutomationRate automation_rate)
    : m_param(RustFFI::web_audio_render_param_create(timeline->rust_timeline(), min_value, max_value, static_cast<RustFFI::AutomationRate>(automation_rate)))
{
    static_assert(to_underlying(Bindings::AutomationRate::ARate) == to_underlying(RustFFI::AutomationRate::ARate));
    static_assert(to_underlying(Bindings::AutomationRate::KRate) == to_underlying(RustFFI::AutomationRate::KRate));
}

RenderAudioParam::~RenderAudioParam()
{
    RustFFI::web_audio_render_param_destroy(m_param);
}

void RenderAudioParam::set_automation_rate(Bindings::AutomationRate rate)
{
    RustFFI::web_audio_render_param_set_automation_rate(m_param, static_cast<RustFFI::AutomationRate>(rate));
}

Bindings::AutomationRate RenderAudioParam::automation_rate() const
{
    return static_cast<Bindings::AutomationRate>(RustFFI::web_audio_render_param_automation_rate(m_param));
}

void RenderAudioParam::clear_inputs()
{
    RustFFI::web_audio_render_param_clear_inputs(m_param);
}

void RenderAudioParam::add_input(NodeID source, size_t output_index)
{
    RustFFI::web_audio_render_param_add_input(m_param, source.value(), output_index);
}

void RenderAudioParam::compute(RenderGraph& graph, RenderContext const& context, Span<float> output)
{
    RustFFI::RenderContext rust_context { context.sample_rate, context.quantum_size, context.quantum_start_frame };
    RustFFI::web_audio_render_param_compute(m_param, &graph, [](void* graph, u64 source, size_t output_index, RustFFI::RenderContext const* context) -> void const* {
        RenderContext render_context { context->sample_rate, context->quantum_size, context->quantum_start_frame };
        auto const* bus = static_cast<RenderGraph*>(graph)->pull_output(source, output_index, render_context);
        return bus ? bus->rust_bus() : nullptr;
    }, &rust_context, output);
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
