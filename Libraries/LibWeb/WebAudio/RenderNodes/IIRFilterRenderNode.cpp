/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/GraphNodes/IIRFilterGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/IIRFilterRenderNode.h>

namespace Web::WebAudio::Render {

IIRFilterRenderNode::IIRFilterRenderNode(NodeID node_id, IIRFilterGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_output(1, quantum_size, max_channel_count)
{
    set_coefficients(desc);
    m_output.set_channel_count(1);
    ensure_history_channels(1);
}

void IIRFilterRenderNode::set_coefficients(IIRFilterGraphNode const& desc)
{
    ASSERT_WEBAUDIO_THREAD();

    m_feedforward = desc.feedforward;
    m_feedback = desc.feedback;

    m_input_history_length = m_feedforward.size() > 0 ? m_feedforward.size() - 1 : 0;
    m_output_history_length = m_feedback.size() > 0 ? m_feedback.size() - 1 : 0;

    for (auto& history : m_input_history)
        history.clear();
    for (auto& history : m_output_history)
        history.clear();
}

void IIRFilterRenderNode::ensure_history_channels(size_t channels)
{
    ASSERT_WEBAUDIO_THREAD();

    size_t const target = min(channels, max_channel_count);
    m_input_history.resize(target);
    m_output_history.resize(target);

    for (size_t ch = 0; ch < target; ++ch) {
        if (m_input_history[ch].size() != m_input_history_length) {
            m_input_history[ch].resize(m_input_history_length);
            m_input_history[ch].fill(0.0);
        }
        if (m_output_history[ch].size() != m_output_history_length) {
            m_output_history[ch].resize(m_output_history_length);
            m_output_history[ch].fill(0.0);
        }
    }
}

void IIRFilterRenderNode::process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const&)
{
    ASSERT_RENDER_THREAD();

    // https://webaudio.github.io/web-audio-api/#IIRFilterNode

    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    size_t const input_channels = mixed_input ? mixed_input->channel_count() : 1;
    size_t const output_channels = min(input_channels, max_channel_count);
    m_output.set_channel_count(output_channels);
    ensure_history_channels(output_channels);

    size_t const frames = m_output.frame_count();

    for (size_t ch = 0; ch < m_output.channel_count(); ++ch) {
        auto out = m_output.channel(ch);
        auto& input_history = m_input_history[ch];
        auto& output_history = m_output_history[ch];

        for (size_t i = 0; i < frames; ++i) {
            f64 const x = static_cast<f64>(mixed_input ? mixed_input->channel(ch)[i] : 0.0f);

            f64 y = 0.0;
            if (!m_feedforward.is_empty())
                y = m_feedforward[0] * x;

            for (size_t k = 1; k < m_feedforward.size(); ++k)
                y += m_feedforward[k] * input_history[k - 1];

            for (size_t k = 1; k < m_feedback.size(); ++k)
                y -= m_feedback[k] * output_history[k - 1];

            out[i] = static_cast<f32>(y);

            if (m_input_history_length > 0) {
                for (size_t idx = m_input_history_length - 1; idx > 0; --idx)
                    input_history[idx] = input_history[idx - 1];
                input_history[0] = x;
            }

            if (m_output_history_length > 0) {
                for (size_t idx = m_output_history_length - 1; idx > 0; --idx)
                    output_history[idx] = output_history[idx - 1];
                output_history[0] = y;
            }
        }
    }
}

void IIRFilterRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();

    if (!node.has<IIRFilterGraphNode>())
        return;

    auto const& desc = node.get<IIRFilterGraphNode>();
    set_coefficients(desc);
    ensure_history_channels(m_output.channel_count());
}

}
