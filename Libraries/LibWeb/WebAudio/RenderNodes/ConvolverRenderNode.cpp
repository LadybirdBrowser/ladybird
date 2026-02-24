/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Math.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/FrequencyAnalysis.h>
#include <LibWeb/WebAudio/Engine/Policy.h>
#include <LibWeb/WebAudio/GraphNodes/ConvolverGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/ConvolverRenderNode.h>
#include <cmath>

namespace Web::WebAudio::Render {

static f32 compute_normalization_gain(Vector<Vector<f32>> const& impulse, size_t channel_count)
{
    f64 energy = 0.0;
    size_t const channels_to_consider = min(channel_count, impulse.size());
    for (size_t ch = 0; ch < channels_to_consider; ++ch) {
        auto const& channel = impulse[ch];
        for (auto sample : channel)
            energy += static_cast<f64>(sample) * static_cast<f64>(sample);
    }

    if (energy <= 0.0 || !__builtin_isfinite(energy) || __builtin_isnan(energy))
        return 1.0f;

    f64 const gain = 1.0 / AK::sqrt(energy);
    if (!__builtin_isfinite(gain) || __builtin_isnan(gain))
        return 1.0f;

    return static_cast<f32>(gain);
}

ConvolverRenderNode::ConvolverRenderNode(NodeID node_id, ConvolverGraphNode const& desc, RefPtr<SharedAudioBuffer> impulse_buffer, size_t quantum_size)
    : RenderNode(node_id)
    , m_normalize(desc.normalize)
    , m_channel_interpretation(desc.channel_interpretation)
    , m_channel_count(desc.channel_count)
    , m_impulse_buffer(move(impulse_buffer))
    , m_output(1, quantum_size, max_channels)
{
    load_impulse_from_buffer(m_impulse_buffer.ptr());
}

void ConvolverRenderNode::load_impulse_from_buffer(SharedAudioBuffer const* buffer)
{
    ASSERT_WEBAUDIO_THREAD();

    m_impulse.clear();
    m_impulse_length = 0;
    m_impulse_buffer_channel_count = 0;
    m_partition_size = 0;
    m_fft_size = 0;
    m_partition_count = 0;
    m_impulse_partitions.clear();
    m_input_fft_history.clear();
    m_overlap_tail.clear();
    m_fft_accum_real.clear();
    m_fft_accum_imag.clear();
    m_fft_time_real.clear();
    m_fft_time_imag.clear();
    m_fft_history_write_index = 0;
    m_output_channel_hold_frames = 0;
    m_tail_frames_remaining = 0;

    if (!buffer) {
        m_output.set_channel_count(1);
        return;
    }

    size_t const channels = min(buffer->channel_count(), max_channels);
    m_impulse_buffer_channel_count = channels;
    m_impulse.resize(channels);

    m_impulse_length = buffer->length_in_sample_frames();
    for (size_t ch = 0; ch < channels; ++ch) {
        auto samples = buffer->channel(ch);
        m_impulse[ch].resize(m_impulse_length);
        for (size_t i = 0; i < m_impulse_length; ++i)
            m_impulse[ch][i] = i < samples.size() ? samples[i] : 0.0f;
    }

    renormalize_impulse();
    rebuild_partitioned_impulse();
    m_output.set_channel_count(max(static_cast<size_t>(1), m_impulse.size()));
    m_last_output_channels = m_output.channel_count();
}

void ConvolverRenderNode::renormalize_impulse()
{
    ASSERT_WEBAUDIO_THREAD();

    if (m_impulse.is_empty() || m_impulse_length == 0)
        return;

    // Reload the unscaled impulse so toggling normalize produces the correct gain.
    if (m_impulse_buffer) {
        size_t const channels = min(m_impulse_buffer->channel_count(), m_impulse.size());
        size_t const length = min(m_impulse_buffer->length_in_sample_frames(), m_impulse_length);
        for (size_t ch = 0; ch < channels; ++ch) {
            auto samples = m_impulse_buffer->channel(ch);
            auto& dest = m_impulse[ch];
            for (size_t i = 0; i < length; ++i)
                dest[i] = i < samples.size() ? samples[i] : 0.0f;
            for (size_t i = length; i < dest.size(); ++i)
                dest[i] = 0.0f;
        }
    }

    f32 gain = m_normalize ? compute_normalization_gain(m_impulse, max<size_t>(1, m_impulse_buffer_channel_count)) : 1.0f;
    if (gain == 1.0f)
        return;

    for (auto& channel : m_impulse) {
        for (auto& sample : channel)
            sample *= gain;
    }

    // If the buffer is mono but we've expanded the impulse for stereo output,
    // keep the duplicated channels identical.
    if (m_impulse_buffer_channel_count == 1 && m_impulse.size() > 1) {
        for (size_t ch = 1; ch < m_impulse.size(); ++ch)
            m_impulse[ch] = m_impulse[0];
    }
}

void ConvolverRenderNode::ensure_impulse_channels(size_t channels)
{
    ASSERT_WEBAUDIO_THREAD();

    if (channels <= m_impulse.size())
        return;

    auto old_input_fft_history = move(m_input_fft_history);
    auto old_overlap_tail = move(m_overlap_tail);
    size_t const old_partition_count = m_partition_count;
    size_t const old_fft_size = m_fft_size;
    size_t const old_fft_history_write_index = m_fft_history_write_index;

    size_t const target = min(channels, max_channels);
    size_t const existing = m_impulse.size();
    m_impulse.resize(target);

    for (size_t ch = existing; ch < target; ++ch) {
        m_impulse[ch].resize(m_impulse_length);
        auto const& source = m_impulse.is_empty() ? Vector<f32> {} : m_impulse[0];
        for (size_t i = 0; i < m_impulse_length; ++i)
            m_impulse[ch][i] = source.is_empty() ? 0.0f : source[i];
    }

    rebuild_partitioned_impulse();

    // Preserve history only when the partitioning layout is unchanged, since
    // any size change invalidates the ring buffer indexing and block sizes.
    if (old_partition_count == m_partition_count && old_fft_size == m_fft_size) {
        if (!old_input_fft_history.is_empty() && !m_input_fft_history.is_empty()) {
            size_t const copy_channels = min(old_input_fft_history.size(), m_input_fft_history.size());
            for (size_t ch = 0; ch < copy_channels; ++ch) {
                if (old_input_fft_history[ch].size() != m_input_fft_history[ch].size())
                    continue;
                for (size_t part = 0; part < m_partition_count; ++part) {
                    m_input_fft_history[ch][part].real = move(old_input_fft_history[ch][part].real);
                    m_input_fft_history[ch][part].imag = move(old_input_fft_history[ch][part].imag);
                }
            }
            m_fft_history_write_index = old_fft_history_write_index;
        }
        if (!old_overlap_tail.is_empty() && !m_overlap_tail.is_empty())
            m_overlap_tail[0] = move(old_overlap_tail[0]);
    }
}

void ConvolverRenderNode::rebuild_partitioned_impulse()
{
    ASSERT_WEBAUDIO_THREAD();

    if (m_impulse_length == 0 || m_impulse.is_empty())
        return;

    m_partition_size = m_output.frame_count();
    m_fft_size = m_partition_size * 2;
    m_partition_count = (m_impulse_length + m_partition_size - 1) / m_partition_size;
    if (m_partition_count == 0)
        m_partition_count = 1;

    m_impulse_partitions.resize(m_impulse.size());
    for (size_t ch = 0; ch < m_impulse.size(); ++ch) {
        m_impulse_partitions[ch].resize(m_partition_count);
        for (size_t part = 0; part < m_partition_count; ++part) {
            auto& block = m_impulse_partitions[ch][part];
            block.real.resize(m_fft_size);
            block.imag.resize(m_fft_size);
            block.real.fill(0.0);
            block.imag.fill(0.0);

            size_t const base_index = part * m_partition_size;
            for (size_t i = 0; i < m_partition_size; ++i) {
                size_t const impulse_index = base_index + i;
                if (impulse_index >= m_impulse_length)
                    break;
                block.real[i] = static_cast<f64>(m_impulse[ch][impulse_index]);
            }

            apply_fft_in_place(block.real.span(), block.imag.span(), FFTDirection::Forward);
        }
    }

    size_t const input_channels = 2;
    m_input_fft_history.resize(input_channels);
    for (size_t ch = 0; ch < input_channels; ++ch) {
        m_input_fft_history[ch].resize(m_partition_count);
        for (size_t part = 0; part < m_partition_count; ++part) {
            auto& block = m_input_fft_history[ch][part];
            block.real.resize(m_fft_size);
            block.imag.resize(m_fft_size);
            block.real.fill(0.0);
            block.imag.fill(0.0);
        }
    }

    m_overlap_tail.resize(2);
    for (auto& tail : m_overlap_tail) {
        tail.resize(m_partition_size);
        tail.fill(0.0f);
    }

    m_fft_accum_real.resize(m_fft_size);
    m_fft_accum_imag.resize(m_fft_size);
    m_fft_time_real.resize(m_fft_size);
    m_fft_time_imag.resize(m_fft_size);

    m_fft_history_write_index = 0;
    m_output_channel_hold_frames = 0;
}

// https://webaudio.github.io/web-audio-api/#ConvolverNode
void ConvolverRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const&)
{
    ASSERT_RENDER_THREAD();

    (void)context;

    m_output.zero();

    if (m_impulse_length == 0 || m_impulse.is_empty())
        return;

    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    size_t const input_channel_count = mixed_input ? mixed_input->channel_count() : 0;
    bool const has_input_bus = input_channel_count > 0;

    if (has_input_bus) {
        m_tail_frames_remaining = m_impulse_length;
    } else if (m_tail_frames_remaining == 0) {
        m_output.set_channel_count(0);
        m_last_output_channels = 0;
        return;
    }

    size_t output_channels = m_last_output_channels;
    if (has_input_bus) {
        // https://webaudio.github.io/web-audio-api/#ConvolverNode-buffer
        // The ConvolverNode output is mono only when the input is mono and the
        // impulse response buffer has one channel. Otherwise the output is stereo.
        bool const mono_impulse = m_impulse_buffer_channel_count == 1;
        bool const mono_input = input_channel_count == 1;
        output_channels = (mono_impulse && mono_input) ? 1 : 2;
    }
    if (output_channels == 0)
        output_channels = 1;

    if (has_input_bus && m_impulse_buffer_channel_count == 1) {
        if (output_channels == 2) {
            m_output_channel_hold_frames = 0;
        } else if (m_last_output_channels == 2) {
            m_output_channel_hold_frames = m_impulse_length;
        }

        if (m_output_channel_hold_frames > 0) {
            // https://webaudio.github.io/web-audio-api/#channels-tail-time
            // When input channels decrease for a node with tail-time, the output
            // channel count changes only after the earlier higher-channel input no longer
            // affects the output. For ConvolverNode this rule applies only when the impulse
            // response is mono, since a multi-channel impulse forces stereo output.
            output_channels = 2;
            size_t const frames = m_output.frame_count();
            if (m_output_channel_hold_frames > frames)
                m_output_channel_hold_frames -= frames;
            else
                m_output_channel_hold_frames = 0;
        }
    }

    ensure_impulse_channels(output_channels);
    m_output.set_channel_count(output_channels);

    bool const mono_to_stereo_transition = m_last_output_channels == 1 && output_channels == 2;
    if (mono_to_stereo_transition && m_channel_interpretation == ChannelInterpretation::Speakers) {
        // https://webaudio.github.io/web-audio-api/#UpMix-sub
        // Speaker up-mixing from mono to stereo duplicates the mono channel into
        // left and right. Copy FFT history so the tail mixes consistently with the new layout.
        if (m_input_fft_history.size() >= 2) {
            for (size_t part = 0; part < m_partition_count; ++part) {
                m_input_fft_history[1][part].real = m_input_fft_history[0][part].real;
                m_input_fft_history[1][part].imag = m_input_fft_history[0][part].imag;
            }
        }
        if (m_overlap_tail.size() >= 2)
            m_overlap_tail[1] = m_overlap_tail[0];
    }

    m_last_output_channels = output_channels;

    if (m_partition_size != m_output.frame_count())
        rebuild_partitioned_impulse();

    if (m_partition_count == 0 || m_fft_size == 0 || m_partition_size == 0)
        return;

    size_t const input_channels = has_input_bus ? mixed_input->channel_count() : 0;
    size_t const convolution_channels = output_channels == 2 ? 2 : 1;
    size_t const history_channels = min(convolution_channels, m_input_fft_history.size());
    for (size_t ch = 0; ch < history_channels; ++ch) {
        auto& block = m_input_fft_history[ch][m_fft_history_write_index];
        block.real.fill(0.0);
        block.imag.fill(0.0);

        if (has_input_bus) {
            bool const mono_to_stereo_discrete = input_channels == 1 && output_channels == 2 && m_channel_interpretation == ChannelInterpretation::Discrete;
            bool const fill_channel = !mono_to_stereo_discrete || ch == 0;
            if (fill_channel) {
                size_t const source_channel = input_channels > 1 ? min(ch, input_channels - 1) : 0u;
                auto const& channel = mixed_input->channel(source_channel);
                for (size_t i = 0; i < m_partition_size; ++i) {
                    block.real[i] = static_cast<f64>(channel[i]);
                }
            }
        }

        apply_fft_in_place(block.real.span(), block.imag.span(), FFTDirection::Forward);
    }

    auto accumulate_partitioned_convolution = [&](size_t impulse_channel, size_t input_channel) {
        if (impulse_channel >= m_impulse_partitions.size())
            return;
        if (input_channel >= m_input_fft_history.size())
            return;

        for (size_t part = 0; part < m_partition_count; ++part) {
            size_t const input_index = (m_fft_history_write_index + m_partition_count - part) % m_partition_count;
            auto const& input_block = m_input_fft_history[input_channel][input_index];
            auto const& impulse_block = m_impulse_partitions[impulse_channel][part];

            for (size_t i = 0; i < m_fft_size; ++i) {
                f64 const a_real = input_block.real[i];
                f64 const a_imag = input_block.imag[i];
                f64 const b_real = impulse_block.real[i];
                f64 const b_imag = impulse_block.imag[i];
                m_fft_accum_real[i] += (a_real * b_real) - (a_imag * b_imag);
                m_fft_accum_imag[i] += (a_real * b_imag) + (a_imag * b_real);
            }
        }
    };

    for (size_t ch = 0; ch < output_channels; ++ch) {
        m_fft_accum_real.fill(0.0);
        m_fft_accum_imag.fill(0.0);

        if (m_impulse_partitions.size() >= 4 && output_channels == 2) {
            accumulate_partitioned_convolution(ch, 0);
            accumulate_partitioned_convolution(ch + 2, 1);
        } else {
            size_t input_channel = ch;
            if (input_channel >= history_channels)
                input_channel = history_channels > 0 ? history_channels - 1 : 0;
            accumulate_partitioned_convolution(ch, input_channel);
        }

        for (size_t i = 0; i < m_fft_size; ++i) {
            if (!__builtin_isfinite(m_fft_accum_real[i]))
                m_fft_accum_real[i] = 0.0;
            if (!__builtin_isfinite(m_fft_accum_imag[i]))
                m_fft_accum_imag[i] = 0.0;
        }

        m_fft_time_real = m_fft_accum_real;
        m_fft_time_imag = m_fft_accum_imag;
        apply_fft_in_place(m_fft_time_real.span(), m_fft_time_imag.span(), FFTDirection::Inverse);

        auto& overlap = m_overlap_tail[ch];
        auto output = m_output.channel(ch);
        for (size_t i = 0; i < m_partition_size; ++i) {
            f32 overlap_sample = overlap[i];
            if (!__builtin_isfinite(overlap_sample))
                overlap_sample = 0.0f;
            f32 value = static_cast<f32>(m_fft_time_real[i]);
            if (!__builtin_isfinite(value))
                value = 0.0f;
            value += overlap_sample;
            if (!__builtin_isfinite(value))
                value = 0.0f;
            output[i] = value;

            f32 tail = static_cast<f32>(m_fft_time_real[i + m_partition_size]);
            if (!__builtin_isfinite(tail))
                tail = 0.0f;
            overlap[i] = tail;
        }
    }

    m_fft_history_write_index = (m_fft_history_write_index + 1) % m_partition_count;

    if (!has_input_bus && m_tail_frames_remaining > 0) {
        size_t const frames = m_output.frame_count();
        if (m_tail_frames_remaining > frames)
            m_tail_frames_remaining -= frames;
        else
            m_tail_frames_remaining = 0;
    }
}

void ConvolverRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();

    if (!node.has<ConvolverGraphNode>())
        return;

    auto const& desc = node.get<ConvolverGraphNode>();
    bool normalize_changed = m_normalize != desc.normalize;
    m_normalize = desc.normalize;
    m_channel_interpretation = desc.channel_interpretation;
    m_channel_count = desc.channel_count;

    // Buffer changes are classified as rebuild-required, so we only need to handle
    // normalization toggles here.
    if (normalize_changed) {
        renormalize_impulse();
        rebuild_partitioned_impulse();
    }
}

}
