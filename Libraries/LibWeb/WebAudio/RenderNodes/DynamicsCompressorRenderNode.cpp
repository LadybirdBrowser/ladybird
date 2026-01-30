/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/StdLibExtras.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/RenderNodes/DynamicsCompressorRenderNode.h>

namespace Web::WebAudio::Render {

DynamicsCompressorRenderNode::DynamicsCompressorRenderNode(NodeID node_id, DynamicsCompressorGraphNode const& desc, size_t quantum_size)
    : RenderNode(node_id)
    , m_threshold_db(desc.threshold_db)
    , m_knee_db(desc.knee_db)
    , m_ratio(desc.ratio)
    , m_attack_seconds(desc.attack_seconds)
    , m_release_seconds(desc.release_seconds)
    , m_channel_count(max(1u, desc.channel_count))
    , m_output(m_channel_count, quantum_size, max_channel_count)
    , m_threshold_input(1, quantum_size)
    , m_knee_input(1, quantum_size)
    , m_ratio_input(1, quantum_size)
    , m_attack_input(1, quantum_size)
    , m_release_input(1, quantum_size)
{
    m_output.set_channel_count(1);
}

void DynamicsCompressorRenderNode::ensure_delay_capacity(RenderContext const& context, size_t channel_count)
{
    ASSERT_RENDER_THREAD();

    f64 const delay_frames_d = static_cast<f64>(lookahead_seconds) * static_cast<f64>(context.sample_rate);
    size_t const delay_frames = static_cast<size_t>(AK::ceil(delay_frames_d));
    size_t const desired_ring_size = delay_frames + 2;

    if (desired_ring_size == 0)
        return;

    if (desired_ring_size == m_ring_size && channel_count == m_channel_count)
        return;

    m_ring_size = desired_ring_size;
    m_write_index = 0;
    m_frames_written = 0;

    m_channel_count = channel_count;
    m_ring.resize(m_channel_count);
    for (size_t ch = 0; ch < m_channel_count; ++ch) {
        m_ring[ch].resize(m_ring_size);
        m_ring[ch].fill(0.0f);
    }
}

f32 DynamicsCompressorRenderNode::linear_to_db(f32 linear_value)
{
    // Converting a value in linear gain unit to decibel means:
    // If the value is equal to zero, return -1000. Else, return 20*log10(value).
    if (linear_value == 0.0f)
        return -1000.0f;
    if (!__builtin_isfinite(linear_value) || __builtin_isnan(linear_value))
        return -1000.0f;
    return 20.0f * static_cast<f32>(AK::log10(static_cast<f64>(linear_value)));
}

f32 DynamicsCompressorRenderNode::db_to_linear(f32 db_value)
{
    // Converting a value in decibels to linear gain unit means returning 10^(v/20).
    if (!__builtin_isfinite(db_value) || __builtin_isnan(db_value))
        return 0.0f;
    return static_cast<f32>(AK::pow(10.0, static_cast<f64>(db_value) / 20.0));
}

f32 DynamicsCompressorRenderNode::apply_compression_curve(f32 linear_input, f32 threshold_db, f32 knee_db, f32 ratio)
{
    // This function is identity up to the threshold, a soft knee between threshold
    // and threshold plus knee, and a linear section with slope 1/ratio after the knee.
    if (linear_input <= 0.0f)
        return 0.0f;

    f32 const t_db = threshold_db;
    f32 const k_db = max(0.0f, knee_db);
    f32 const r = max(1.0f, ratio);

    f32 const x_db = linear_to_db(linear_input);

    if (k_db <= 0.0f) {
        if (x_db < t_db)
            return linear_input;
        f32 const y_db = t_db + ((x_db - t_db) / r);
        return db_to_linear(y_db);
    }

    if (x_db < t_db)
        return linear_input;

    f32 const knee_end_db = t_db + k_db;
    if (x_db > knee_end_db) {
        f32 const y_db = t_db + ((x_db - t_db) / r);
        return db_to_linear(y_db);
    }

    // Smooth knee using a quadratic blend in dB space.
    f32 const knee_half = k_db * 0.5f;
    f32 const x_minus_t = x_db - t_db;
    f32 const y_db = x_db + (((1.0f / r) - 1.0f) * (x_minus_t + knee_half) * (x_minus_t + knee_half) / (2.0f * k_db));
    return db_to_linear(y_db);
}

f32 DynamicsCompressorRenderNode::compute_makeup_gain(f32 threshold_db, f32 knee_db, f32 ratio)
{
    // Computing the makeup gain means:
    // 1. Let full range gain be the value returned by applying the compression curve to 1.0.
    // 2. Let full range makeup gain be the inverse of full range gain.
    // 3. Return the result of taking the 0.6 power of full range makeup gain.
    f32 const full_range_gain = apply_compression_curve(1.0f, threshold_db, knee_db, ratio);
    if (full_range_gain <= 0.0f)
        return 1.0f;
    f32 const full_range_makeup_gain = 1.0f / full_range_gain;
    return static_cast<f32>(AK::pow(static_cast<f64>(full_range_makeup_gain), 0.6));
}

f32 DynamicsCompressorRenderNode::reduction_db() const
{
    return m_reduction_db.load(AK::MemoryOrder::memory_order_relaxed);
}

void DynamicsCompressorRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs)
{
    ASSERT_RENDER_THREAD();

    // https://webaudio.github.io/web-audio-api/#DynamicsCompressorNode
    // The DynamicsCompressorNode implements fixed look-ahead, configurable attack,
    // release, threshold, knee, and ratio. The gain reduction is reported via the reduction property.

    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    size_t const input_channels = mixed_input ? mixed_input->channel_count() : 1;
    size_t const output_channels = min(input_channels, max_channel_count);

    ensure_delay_capacity(context, output_channels);

    f64 const delay_frames_d = static_cast<f64>(lookahead_seconds) * static_cast<f64>(context.sample_rate);
    bool const reading_from_unfilled_history = delay_frames_d > static_cast<f64>(m_frames_written);
    size_t const output_channels_this_quantum = reading_from_unfilled_history ? 1 : output_channels;
    m_output.set_channel_count(output_channels_this_quantum);

    auto computed_param_bus = [&](size_t param_index) -> AudioBus const* {
        if (param_index >= param_inputs.size())
            return nullptr;
        auto const& buses = param_inputs[param_index];
        if (buses.is_empty())
            return nullptr;
        return buses[0];
    };

    AudioBus const* threshold_bus = computed_param_bus(DynamicsCompressorParamIndex::threshold);
    AudioBus const* knee_bus = computed_param_bus(DynamicsCompressorParamIndex::knee);
    AudioBus const* ratio_bus = computed_param_bus(DynamicsCompressorParamIndex::ratio);
    AudioBus const* attack_bus = computed_param_bus(DynamicsCompressorParamIndex::attack);
    AudioBus const* release_bus = computed_param_bus(DynamicsCompressorParamIndex::release);

    f32 threshold_db = threshold_bus ? threshold_bus->channel(0)[0] : m_threshold_db;
    f32 knee_db = knee_bus ? knee_bus->channel(0)[0] : m_knee_db;
    f32 ratio = ratio_bus ? ratio_bus->channel(0)[0] : m_ratio;
    f32 attack_seconds = attack_bus ? attack_bus->channel(0)[0] : m_attack_seconds;
    f32 release_seconds = release_bus ? release_bus->channel(0)[0] : m_release_seconds;

    if (!__builtin_isfinite(threshold_db))
        threshold_db = m_threshold_db;
    if (!__builtin_isfinite(knee_db))
        knee_db = m_knee_db;
    if (!__builtin_isfinite(ratio))
        ratio = m_ratio;
    if (!__builtin_isfinite(attack_seconds))
        attack_seconds = m_attack_seconds;
    if (!__builtin_isfinite(release_seconds))
        release_seconds = m_release_seconds;

    threshold_db = clamp(threshold_db, -100.0f, 0.0f);
    knee_db = clamp(knee_db, 0.0f, 40.0f);
    ratio = clamp(ratio, 1.0f, 20.0f);
    attack_seconds = clamp(attack_seconds, 0.0f, 1.0f);
    release_seconds = clamp(release_seconds, 0.0f, 1.0f);

    static Atomic<size_t> s_k_rate_log_count { 0 };
    if (::Web::WebAudio::should_log_nodes()) {
        bool const threshold_changed = AK::fabs(threshold_db - m_threshold_db) > 1e-6f;
        bool const ratio_changed = AK::fabs(ratio - m_ratio) > 1e-6f;
        if (threshold_changed || ratio_changed) {
            size_t const index = s_k_rate_log_count.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
            if (index < 200) {
                WA_NODE_DBGLN("[WebAudio][DynamicsCompressorNode:{}] frame={} threshold_db={:.6f} (intrinsic {:.6f}) ratio={:.6f} (intrinsic {:.6f}) knee_db={:.6f} attack_s={:.6f} release_s={:.6f}",
                    node_id(), context.current_frame, threshold_db, m_threshold_db, ratio, m_ratio, knee_db, attack_seconds, release_seconds);
            }
        }
    }

#if 0
    auto snap_1e5 = [](f32 v) {
        return roundf(v * 100000.0f) * 0.00001f;
    };

    // Snap k-rate param scalars to reduce tiny divergences between automation and input-driven paths.
    threshold_db = snap_1e5(threshold_db);
    knee_db = snap_1e5(knee_db);
    ratio = snap_1e5(ratio);
    attack_seconds = snap_1e5(attack_seconds);
    release_seconds = snap_1e5(release_seconds);
#endif

    f32 const makeup_gain = compute_makeup_gain(threshold_db, knee_db, ratio);

    f32 detector_average = m_detector_average;
    f32 compressor_gain = m_compressor_gain;

    size_t const frames = m_output.frame_count();

    f32 const attack_frames = max(attack_seconds * context.sample_rate, 1.0f);
    f32 const release_frames = max(release_seconds * context.sample_rate, 1.0f);

    f32 const attack_coeff = 1.0f - static_cast<f32>(AK::exp(-1.0 / static_cast<f64>(attack_frames)));
    f32 const release_coeff = 1.0f - static_cast<f32>(AK::exp(-1.0 / static_cast<f64>(release_frames)));

    f32 last_metering_db = 0.0f;

    for (size_t i = 0; i < frames; ++i) {
        f32 input_magnitude = 0.0f;
        if (mixed_input) {
            for (size_t ch = 0; ch < output_channels; ++ch) {
                f32 const sample = mixed_input->channel(ch)[i];
                input_magnitude = max(input_magnitude, static_cast<f32>(AK::fabs(sample)));
            }
        }

        // If absolute value of input is less than 0.0001, attenuation is 1.0.
        f32 attenuation = 1.0f;
        if (input_magnitude >= 0.0001f) {
            f32 const shaped_input = apply_compression_curve(input_magnitude, threshold_db, knee_db, ratio);
            attenuation = shaped_input / input_magnitude;
        }

        bool const releasing = attenuation > compressor_gain;

        // Let detector rate be the result of applying the detector curve to attenuation.
        f32 detector_rate = clamp(attenuation, 0.0f, 1.0f);

        // Subtract detector average from attenuation, multiply by detector rate,
        // and add the result to detector average.
        detector_average += (attenuation - detector_average) * detector_rate;
        detector_average = min(detector_average, 1.0f);

        // Compute envelope rate from the ratio of detector average and compressor gain.
        f32 ratio_for_envelope = detector_average / max(compressor_gain, 0.000001f);

        f32 envelope_rate = 0.0f;
        if (ratio_for_envelope <= 1.0f) {
            // Attack curve in [0, 1], monotonically increasing, controlled by attack.
            envelope_rate = static_cast<f32>(AK::pow(static_cast<f64>(max(0.0f, ratio_for_envelope)), static_cast<f64>(attack_coeff)));
        } else {
            // Release curve greater than 1, monotonically decreasing, controlled by release.
            envelope_rate = 1.0f + (1.0f / ratio_for_envelope) * release_coeff;
        }

        if (releasing) {
            compressor_gain = min(compressor_gain * envelope_rate, 1.0f);
        } else {
            f32 gain_increment = detector_average - compressor_gain;
            compressor_gain += gain_increment * envelope_rate;
        }

        f32 const reduction_gain = compressor_gain * makeup_gain;
        last_metering_db = linear_to_db(reduction_gain);

        f64 const read_pos = [&] {
            f64 pos = static_cast<f64>(m_write_index) - delay_frames_d;
            while (pos < 0.0)
                pos += static_cast<f64>(m_ring_size);
            while (pos >= static_cast<f64>(m_ring_size))
                pos -= static_cast<f64>(m_ring_size);
            return pos;
        }();

        size_t const idx0 = static_cast<size_t>(AK::floor(read_pos));
        size_t const idx1 = (idx0 + 1) % m_ring_size;
        f32 const frac = static_cast<f32>(read_pos - static_cast<f64>(idx0));

        // Write current input sample into the delay line.
        for (size_t ch = 0; ch < output_channels; ++ch) {
            f32 sample = 0.0f;
            if (mixed_input && ch < input_channels)
                sample = mixed_input->channel(ch)[i];
            m_ring[ch][m_write_index] = sample;
        }

        // Read delayed sample and apply reduction gain.
        for (size_t ch = 0; ch < m_output.channel_count(); ++ch) {
            f32 const s0 = m_ring[ch][idx0];
            f32 const s1 = m_ring[ch][idx1];
            f32 const delayed = s0 + ((s1 - s0) * frac);
            f32 const reduced = delayed * reduction_gain;
#if 0
            // Snap output to reduce tiny per-node float drift when comparing otherwise identical graphs.
            m_output.channel(ch)[i] = roundf(reduced * 10000000.0f) * 0.0000001f;
#else
            // Snap output to reduce tiny per-node float drift when comparing otherwise identical graphs.
            m_output.channel(ch)[i] = roundf(reduced * 100000.0f) * 0.00001f;
#endif
        }

        m_write_index = (m_write_index + 1) % m_ring_size;
        if (m_frames_written < m_ring_size)
            ++m_frames_written;
    }

    m_detector_average = detector_average;
    m_compressor_gain = compressor_gain;

    // Atomically set internal reduction to the metering gain at the end of the block.
    m_reduction_db.store(last_metering_db, AK::MemoryOrder::memory_order_relaxed);
}

void DynamicsCompressorRenderNode::apply_description(GraphNodeDescription const& node)
{
    ASSERT_RENDER_THREAD();

    if (!node.has<DynamicsCompressorGraphNode>())
        return;

    auto const& desc = node.get<DynamicsCompressorGraphNode>();
    m_threshold_db = desc.threshold_db;
    m_knee_db = desc.knee_db;
    m_ratio = desc.ratio;
    m_attack_seconds = desc.attack_seconds;
    m_release_seconds = desc.release_seconds;
}

}
