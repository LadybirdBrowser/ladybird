/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/RenderNodes/AnalyserRenderNode.h>

namespace Web::WebAudio::Render {

static constexpr size_t MIN_FFT_SIZE { 32 };
static constexpr size_t MAX_FFT_SIZE { 32768 };

AnalyserRenderNode::AnalyserRenderNode(NodeID node_id, AnalyserGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_fft_size(desc.fft_size)
    , m_smoothing_time_constant(desc.smoothing_time_constant)
    , m_output(1, quantum_size, max_channel_capacity)
    , m_analysis_mono(1, quantum_size)
{
    ASSERT_CONTROL_THREAD();
    initialize_storage();
    reset_runtime_state();
}

void AnalyserRenderNode::process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const&)
{
    ASSERT_RENDER_THREAD();
    // https://webaudio.github.io/web-audio-api/#the-analysernode-interface
    // The output of the AnalyserNode is the same as its input.
    // For analysis (time-domain/frequency-domain data), the input signal is down-mixed to mono.

    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    if (!mixed_input) {
        m_output.set_channel_count(1);
        m_output.zero();
        m_analysis_mono.zero();
        return;
    }

    // Audio inputs are mixed at the graph edge. Slot 0 contains the pre-mixed input for this node input.
    size_t const output_channel_count = min(mixed_input->channel_count(), m_output.channel_capacity());
    m_output.set_channel_count(output_channel_count);
    size_t const frames = m_output.frame_count();
    for (size_t ch = 0; ch < output_channel_count; ++ch) {
        auto in = mixed_input->channel(ch);
        auto out = m_output.channel(ch);
        in.slice(0, frames).copy_to(out.slice(0, frames));
    }

    // Analysis input is always mono.
    m_analysis_mono.set_channel_count(1);
    AudioBus const* analysis_inputs[] = { mixed_input };
    mix_inputs_into(m_analysis_mono, ReadonlySpan<AudioBus const*> { analysis_inputs });

    size_t const analysis_frames = m_analysis_mono.frame_count();
    auto out = m_analysis_mono.channel(0);

    size_t const samples_until_wrap = min(analysis_frames, MAX_FFT_SIZE - m_ring_write_index);
    out.slice(0, samples_until_wrap).copy_to(m_ring_buffer.span().slice(m_ring_write_index, samples_until_wrap));

    size_t const samples_after_wrap = analysis_frames - samples_until_wrap;
    if (samples_after_wrap != 0)
        out.slice(samples_until_wrap, samples_after_wrap).copy_to(m_ring_buffer.span().slice(0, samples_after_wrap));

    m_ring_write_index = samples_after_wrap != 0 ? samples_after_wrap : (m_ring_write_index + analysis_frames);
    m_ring_filled_samples = min(MAX_FFT_SIZE, m_ring_filled_samples + analysis_frames);

    // Frequency data (including smoothing) is defined over consecutive analysis frames.
    // Keep it continuously updated per render quantum.
    update_time_domain_snapshot_buffer();
}

void AnalyserRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();
    if (!node.has<AnalyserGraphNode>())
        return;

    auto const& desc = node.get<AnalyserGraphNode>();
    size_t const new_fft_size = min(max(desc.fft_size, MIN_FFT_SIZE), MAX_FFT_SIZE);
    if (new_fft_size != m_fft_size.load(AK::MemoryOrder::memory_order_relaxed)) {
        m_fft_size.store(new_fft_size, AK::MemoryOrder::memory_order_release);
        m_render_frequency_smoothing_needs_reset = true;
    }

    m_smoothing_time_constant.store(desc.smoothing_time_constant, AK::MemoryOrder::memory_order_release);
}

bool AnalyserRenderNode::copy_analyser_time_domain_data(Span<f32> output) const
{
    ASSERT_RENDER_THREAD();
    auto const fft_size = m_fft_size.load(AK::MemoryOrder::memory_order_acquire);
    if (fft_size == 0 || output.size() != fft_size)
        return false;
    auto const index = m_active_snapshot_index.load(AK::MemoryOrder::memory_order_acquire);
    auto const& data = m_time_domain_cache[index];

    VERIFY(data.size() >= fft_size);
    data.span().slice(0, fft_size).copy_to(output);

    return true;
}

bool AnalyserRenderNode::copy_analyser_frequency_data_db(Span<f32> output) const
{
    ASSERT_RENDER_THREAD();
    auto const fft_size = m_fft_size.load(AK::MemoryOrder::memory_order_acquire);
    size_t const bin_count = bin_count_for_fft_size(fft_size);
    if (fft_size == 0 || output.size() != bin_count)
        return false;
    auto const snapshot_index = m_active_snapshot_index.load(AK::MemoryOrder::memory_order_acquire);

    // Render thread continuously computes frequency output for the current snapshot.
    auto const& data = m_frequency_data_db[snapshot_index];
    VERIFY(data.size() >= bin_count);
    data.span().slice(0, bin_count).copy_to(output);
    return true;
}

void AnalyserRenderNode::update_time_domain_snapshot_buffer()
{
    ASSERT_RENDER_THREAD();
    auto const fft_size = m_fft_size.load(AK::MemoryOrder::memory_order_acquire);
    if (fft_size == 0)
        return;

    auto const active_index = m_active_snapshot_index.load(AK::MemoryOrder::memory_order_relaxed);
    auto const write_index = static_cast<u8>(active_index ^ 1u);

    auto& time_domain_out = m_time_domain_cache[write_index];
    VERIFY(time_domain_out.size() >= fft_size);

    // If we have fewer than fft_size samples, left-pad with zeros.
    size_t const copy_count = min(m_ring_filled_samples, fft_size);
    size_t const zero_prefix = fft_size - copy_count;

    time_domain_out.span().slice(0, zero_prefix).fill(0.0f);

    size_t const start_index = (m_ring_write_index + MAX_FFT_SIZE - copy_count) % MAX_FFT_SIZE;
    size_t const samples_until_wrap = min(copy_count, MAX_FFT_SIZE - start_index);
    m_ring_buffer.span()
        .slice(start_index, samples_until_wrap)
        .copy_to(time_domain_out.span().slice(zero_prefix, samples_until_wrap));

    size_t const samples_after_wrap = copy_count - samples_until_wrap;
    if (samples_after_wrap != 0)
        m_ring_buffer.span()
            .slice(0, samples_after_wrap)
            .copy_to(time_domain_out.span().slice(zero_prefix + samples_until_wrap, samples_after_wrap));

    auto const smoothing_time_constant = m_smoothing_time_constant.load(AK::MemoryOrder::memory_order_acquire);
    size_t const bin_count = bin_count_for_fft_size(fft_size);

    // The smoothing state depends on the fft size.
    if (m_render_frequency_smoothing_needs_reset) {
        m_render_frequency_smoothing_needs_reset = false;
        VERIFY(m_previous_block_render.size() >= bin_count);
        m_previous_block_render.span().slice(0, bin_count).fill(0.0f);
    }

    auto& frequency_out = m_frequency_data_db[write_index];
    VERIFY(frequency_out.size() >= bin_count);
    compute_frequency_data_db_in_place(
        time_domain_out.span().slice(0, fft_size),
        fft_size,
        smoothing_time_constant,
        m_previous_block_render,
        frequency_out,
        m_frequency_scratch_render);

    m_active_snapshot_index.store(write_index, AK::MemoryOrder::memory_order_release);
}

void AnalyserRenderNode::initialize_storage()
{
    ASSERT_CONTROL_THREAD();
    m_fft_size.store(min(max(m_fft_size.load(AK::MemoryOrder::memory_order_relaxed), MIN_FFT_SIZE), MAX_FFT_SIZE), AK::MemoryOrder::memory_order_relaxed);

    m_ring_buffer.resize(MAX_FFT_SIZE);
    m_ring_buffer.fill(0.0f);

    for (auto& buffer : m_time_domain_cache) {
        buffer.resize(MAX_FFT_SIZE);
        buffer.fill(0.0f);
    }

    size_t constexpr max_bin_count = MAX_FFT_SIZE / 2;
    m_previous_block_render.resize(max_bin_count);
    m_previous_block_render.fill(0.0f);

    for (auto& buffer : m_frequency_data_db) {
        buffer.resize(max_bin_count);
        buffer.fill(-AK::Infinity<f32>);
    }

    m_frequency_scratch_render.windowed.resize(MAX_FFT_SIZE);
    m_frequency_scratch_render.real.resize(MAX_FFT_SIZE);
    m_frequency_scratch_render.imaginary.resize(MAX_FFT_SIZE);
}

void AnalyserRenderNode::reset_runtime_state()
{
    ASSERT_CONTROL_THREAD();
    m_ring_write_index = 0;
    m_ring_filled_samples = 0;

    m_active_snapshot_index.store(0, AK::MemoryOrder::memory_order_relaxed);

    m_render_frequency_smoothing_needs_reset = true;
}

}
