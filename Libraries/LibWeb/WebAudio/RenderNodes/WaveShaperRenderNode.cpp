/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Math.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/GraphNodes/WaveShaperGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/WaveShaperRenderNode.h>

namespace Web::WebAudio::Render {

WaveShaperRenderNode::WaveShaperRenderNode(NodeID node_id, WaveShaperGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_oversample(desc.oversample)
    , m_curve(desc.curve)
    , m_output(1, quantum_size, max_channel_count)
    , m_oversampled(1, quantum_size, max_channel_count)
{
    m_output.set_channel_count(1);
    m_oversampled.set_channel_count(1);
}

size_t WaveShaperRenderNode::oversample_factor() const
{
    switch (m_oversample) {
    case OverSampleType::X2:
        return 2;
    case OverSampleType::X4:
        return 4;
    case OverSampleType::None:
    default:
        return 1;
    }
}

f32 WaveShaperRenderNode::shape_sample(f32 input) const
{
    if (m_curve.is_empty())
        return input;

    f32 x = input;
    if (!__builtin_isfinite(x) || __builtin_isnan(x))
        x = 0.0f;

    x = clamp(x, -1.0f, 1.0f);

    size_t const curve_size = m_curve.size();
    if (curve_size == 1)
        return m_curve[0];

    f32 const index = (x + 1.0f) * 0.5f * static_cast<f32>(curve_size - 1);
    size_t const index_lower = static_cast<size_t>(AK::floor(index));
    size_t const index_upper = min(index_lower + 1, curve_size - 1);
    f32 const fraction = index - static_cast<f32>(index_lower);

    f32 const lower_value = m_curve[index_lower];
    f32 const upper_value = m_curve[index_upper];
    return lower_value + (fraction * (upper_value - lower_value));
}

void WaveShaperRenderNode::ensure_oversample_storage(size_t channel_count, size_t oversampled_frames, size_t factor)
{
    ASSERT_RENDER_THREAD();

    if (channel_count == 0)
        channel_count = 1;

    if (m_oversampled.channel_capacity() < channel_count || m_oversampled.frame_count() != oversampled_frames) {
        m_oversampled = m_oversampled.clone_resized(channel_count, oversampled_frames, max_channel_count);
    } else {
        m_oversampled.set_channel_count(channel_count);
    }

    if (!m_resampler_initialized || m_resampler_channel_count != channel_count || m_resampler_factor != factor) {
        sample_rate_converter_init(m_upsampler, channel_count, 1.0 / static_cast<f64>(factor));
        sample_rate_converter_init(m_downsampler, channel_count, static_cast<f64>(factor));
        m_resampler_initialized = true;
        m_resampler_channel_count = channel_count;
        m_resampler_factor = factor;
    }
}

void WaveShaperRenderNode::process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const&)
{
    ASSERT_RENDER_THREAD();

    // https://webaudio.github.io/web-audio-api/#WaveShaperNode

    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    size_t const input_channels = mixed_input ? mixed_input->channel_count() : 1;
    size_t const output_channels = min(input_channels, max_channel_count);
    m_output.set_channel_count(output_channels);

    size_t const frames = m_output.frame_count();
    bool input_is_silent = mixed_input == nullptr;
    if (mixed_input) {
        for (size_t ch = 0; ch < m_output.channel_count(); ++ch) {
            auto in = mixed_input->channel(ch);
            for (size_t i = 0; i < frames; ++i) {
                if (in[i] != 0.0f) {
                    input_is_silent = false;
                    break;
                }
            }
            if (!input_is_silent)
                break;
        }
    }

    if (input_is_silent) {
        if (m_curve.is_empty()) {
            m_output.zero();
            return;
        }

        f32 const silent_value = shape_sample(0.0f);
        for (size_t ch = 0; ch < m_output.channel_count(); ++ch)
            m_output.channel(ch).fill(silent_value);
        return;
    }
    if (m_curve.is_empty()) {
        for (size_t ch = 0; ch < m_output.channel_count(); ++ch) {
            auto in = mixed_input->channel(ch);
            auto out = m_output.channel(ch);
            for (size_t i = 0; i < frames; ++i)
                out[i] = in[i];
        }
        return;
    }

    size_t const factor = oversample_factor();
    if (factor == 1) {
        for (size_t ch = 0; ch < m_output.channel_count(); ++ch) {
            auto in = mixed_input->channel(ch);
            auto out = m_output.channel(ch);
            for (size_t i = 0; i < frames; ++i)
                out[i] = shape_sample(in[i]);
        }
        return;
    }

    size_t const oversampled_frames = frames * factor;
    ensure_oversample_storage(output_channels, oversampled_frames, factor);

    AK::Array<ReadonlySpan<f32>, max_channel_count> input_spans_storage;
    AK::Array<Span<f32>, max_channel_count> oversampled_spans_storage;

    auto input_spans = input_spans_storage.span().slice(0, output_channels);
    auto oversampled_spans = oversampled_spans_storage.span().slice(0, output_channels);

    for (size_t ch = 0; ch < output_channels; ++ch) {
        input_spans[ch] = mixed_input->channel(ch);
        oversampled_spans[ch] = m_oversampled.channel(ch);
    }

    auto upsample_result = sample_rate_converter_process(m_upsampler, input_spans, oversampled_spans, false);
    if (upsample_result.output_frames_produced < oversampled_frames) {
        for (size_t ch = 0; ch < output_channels; ++ch) {
            auto tail = oversampled_spans[ch].slice(upsample_result.output_frames_produced);
            tail.fill(0.0f);
        }
    }

    for (size_t ch = 0; ch < output_channels; ++ch) {
        auto bus = m_oversampled.channel(ch);
        for (size_t i = 0; i < oversampled_frames; ++i)
            bus[i] = shape_sample(bus[i]);
    }

    AK::Array<ReadonlySpan<f32>, max_channel_count> shaped_spans_storage;
    AK::Array<Span<f32>, max_channel_count> output_spans_storage;
    auto shaped_spans = shaped_spans_storage.span().slice(0, output_channels);
    auto output_spans = output_spans_storage.span().slice(0, output_channels);

    for (size_t ch = 0; ch < output_channels; ++ch) {
        shaped_spans[ch] = m_oversampled.channel(ch);
        output_spans[ch] = m_output.channel(ch);
    }

    auto downsample_result = sample_rate_converter_process(m_downsampler, shaped_spans, output_spans, false);
    if (downsample_result.output_frames_produced < frames) {
        for (size_t ch = 0; ch < output_channels; ++ch) {
            auto tail = output_spans[ch].slice(downsample_result.output_frames_produced);
            tail.fill(0.0f);
        }
    }
}

void WaveShaperRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();

    if (!node.has<WaveShaperGraphNode>())
        return;

    auto const& desc = node.get<WaveShaperGraphNode>();
    m_oversample = desc.oversample;
}

void WaveShaperRenderNode::apply_description_offline(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();

    if (!node.has<WaveShaperGraphNode>())
        return;

    auto const& desc = node.get<WaveShaperGraphNode>();
    m_curve = desc.curve;
    m_oversample = desc.oversample;
}

}
