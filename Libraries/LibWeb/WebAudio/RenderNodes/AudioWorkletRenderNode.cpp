/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/RenderNodes/AudioWorkletRenderNode.h>
#include <LibWeb/WebAudio/Worklet/AudioWorkletProcessorHost.h>

namespace Web::WebAudio::Render {

AudioWorkletRenderNode::AudioWorkletRenderNode(NodeID node_id, AudioWorkletGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_number_of_inputs(desc.number_of_inputs)
    , m_number_of_outputs(desc.number_of_outputs)
    , m_processor_name(desc.processor_name)
    , m_output_channel_count(desc.output_channel_count)
    , m_channel_count(max<size_t>(1, desc.channel_count))
    , m_channel_count_mode(desc.channel_count_mode)
    , m_parameter_names(desc.parameter_names)
{
    if (m_output_channel_count.has_value() && m_output_channel_count->size() != m_number_of_outputs)
        m_output_channel_count.clear();

    m_outputs.ensure_capacity(m_number_of_outputs);
    for (size_t i = 0; i < m_number_of_outputs; ++i) {
        auto bus = make<AudioBus>(1, quantum_size, max_channel_count);
        m_outputs.unchecked_append(move(bus));
    }
    if (m_number_of_outputs == 0) {
        m_silent_output = make<AudioBus>(1, quantum_size, max_channel_count);
        m_silent_output->zero();
    }

    m_parameters_for_invocation.resize(m_parameter_names.size());
    m_inputs_for_invocation.resize(m_number_of_inputs);
}

void AudioWorkletRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs)
{
    ASSERT_RENDER_THREAD();

    size_t computed_output_channels = 1;
    if (!m_output_channel_count.has_value()) {
        size_t max_input_channels = 1;
        for (auto const& input_group : inputs) {
            if (input_group.is_empty())
                continue;
            if (auto const* mixed = input_group[0])
                max_input_channels = max(max_input_channels, mixed->channel_count());
        }

        size_t const safe_channel_count = max<size_t>(1, m_channel_count);
        size_t const safe_max_input_channels = max<size_t>(1, max_input_channels);
        switch (m_channel_count_mode) {
        case ChannelCountMode::Max:
            computed_output_channels = safe_max_input_channels;
            break;
        case ChannelCountMode::ClampedMax:
            computed_output_channels = min(safe_max_input_channels, safe_channel_count);
            break;
        case ChannelCountMode::Explicit:
            computed_output_channels = safe_channel_count;
            break;
        }
    }

    // Ensure output channel count is stable for this quantum.
    for (size_t output_index = 0; output_index < m_number_of_outputs; ++output_index) {
        auto& bus = *m_outputs[output_index];
        size_t desired_channels = 1;
        if (m_output_channel_count.has_value()) {
            if (output_index < m_output_channel_count->size())
                desired_channels = (*m_output_channel_count)[output_index];
        } else {
            desired_channels = computed_output_channels;
        }

        desired_channels = clamp(desired_channels, 1ul, max_channel_count);
        desired_channels = min(desired_channels, bus.channel_capacity());
        bus.set_channel_count(desired_channels);
        bus.zero();
    }

    if (!context.worklet_processor_host) {
        for (auto& bus : m_outputs)
            bus->set_channel_count(0);
        return;
    }

    if (!m_keep_processing)
        return;

    Vector<AudioBus*> output_buses;
    output_buses.ensure_capacity(m_number_of_outputs);
    for (size_t i = 0; i < m_number_of_outputs; ++i)
        output_buses.unchecked_append(m_outputs[i].ptr());

    // Build AudioWorkletProcessor "parameters" object from computed param buses.
    // Each param input provides a mono bus in slot 0 (computedValue) for this quantum.
    for (size_t param_index = 0; param_index < m_parameter_names.size(); ++param_index) {
        auto& entry = m_parameters_for_invocation[param_index];
        entry.name = m_parameter_names[param_index].bytes_as_string_view();

        if (param_index >= param_inputs.size() || param_inputs[param_index].is_empty() || !param_inputs[param_index][0]) {
            // Should not happen: executor always supplies a computed bus for each param.
            static f32 s_zero = 0.0f;
            entry.values = ReadonlySpan<f32>(&s_zero, 1);
            continue;
        }

        AudioBus const& computed = *param_inputs[param_index][0];
        auto values = computed.channel(0);
        if (values.is_empty()) {
            static f32 s_zero = 0.0f;
            entry.values = ReadonlySpan<f32>(&s_zero, 1);
            continue;
        }

        // Reduce constant vectors to length 1 to match spec behavior and WPT expectations.
        bool constant = true;
        f32 const first = values[0];
        for (size_t i = 1; i < values.size(); ++i) {
            if (values[i] != first) {
                constant = false;
                break;
            }
        }

        if (constant)
            entry.values = ReadonlySpan<f32>(values.data(), 1);
        else
            entry.values = values;
    }

    m_inputs_for_invocation.resize(m_number_of_inputs);
    for (size_t input_index = 0; input_index < m_number_of_inputs; ++input_index) {
        auto& input_group = m_inputs_for_invocation[input_index];
        input_group.clear();
        if (input_index >= inputs.size())
            continue;

        auto const& source_group = inputs[input_index];
        input_group.ensure_capacity(source_group.size());
        for (auto const* bus : source_group)
            input_group.unchecked_append(bus);
    }

    m_keep_processing = context.worklet_processor_host->process_audio_worklet(
        node_id(),
        context,
        m_processor_name,
        m_number_of_inputs,
        m_number_of_outputs,
        m_output_channel_count.has_value() ? m_output_channel_count->span() : ReadonlySpan<size_t> {},
        m_inputs_for_invocation,
        output_buses.span(),
        m_parameters_for_invocation.span());
}

AudioBus const& AudioWorkletRenderNode::output(size_t output_index) const
{
    ASSERT_RENDER_THREAD();

    if (m_number_of_outputs == 0) {
        VERIFY(m_silent_output);
        return *m_silent_output;
    }
    if (output_index >= m_number_of_outputs)
        return *m_outputs[0];
    return *m_outputs[output_index];
}

}
