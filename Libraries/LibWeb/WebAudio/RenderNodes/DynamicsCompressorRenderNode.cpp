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
    f32 const full_range_gain = apply_compression_curve(1.0f, threshold_db, knee_db, ratio);
    if (full_range_gain <= 0.0f)
        return 1.0f;
    // 2. Let full range makeup gain be the inverse of full range gain.
    f32 const full_range_makeup_gain = 1.0f / full_range_gain;
    // 3. Return the result of taking the 0.6 power of full range makeup gain.
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

    AudioBus const* mixed_input = nullptr;
    if (!inputs.is_empty() && !inputs[0].is_empty())
        mixed_input = inputs[0][0];

    size_t const input_channels = mixed_input ? mixed_input->channel_count() : 1;
    size_t const output_channels = min(input_channels, max_channel_count);

    ensure_delay_capacity(context, output_channels);

    size_t const delay_frames = static_cast<size_t>(static_cast<f64>(lookahead_seconds) * static_cast<f64>(context.sample_rate));
    bool const reading_from_unfilled_history = delay_frames > m_frames_written;
    size_t const output_channels_this_quantum = reading_from_unfilled_history ? 1 : output_channels;
    m_output.set_channel_count(output_channels_this_quantum);

    auto param_or_default = [&](size_t param_index, f32 fallback) {
        if (param_index >= param_inputs.size())
            return fallback;
        auto const& buses = param_inputs[param_index];
        if (buses.is_empty())
            return fallback;
        f32 const value = buses[0]->channel(0)[0];
        return __builtin_isfinite(value) ? value : fallback;
    };

    f32 threshold_db = clamp(param_or_default(DynamicsCompressorParamIndex::threshold, m_threshold_db), -100.0f, 0.0f);
    f32 knee_db = clamp(param_or_default(DynamicsCompressorParamIndex::knee, m_knee_db), 0.0f, 40.0f);
    f32 ratio = clamp(param_or_default(DynamicsCompressorParamIndex::ratio, m_ratio), 1.0f, 20.0f);
    f32 attack_seconds = clamp(param_or_default(DynamicsCompressorParamIndex::attack, m_attack_seconds), 0.0f, 1.0f);
    f32 release_seconds = clamp(param_or_default(DynamicsCompressorParamIndex::release, m_release_seconds), 0.0f, 1.0f);

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

    f32 const linear_threshold = db_to_linear(threshold_db);
    f32 const slope = 1.0f / ratio;
    f32 const knee_threshold_db = threshold_db + knee_db;
    f32 const knee_threshold = db_to_linear(knee_threshold_db);

    auto knee_curve = [&](f32 x, f32 k) -> f32 {
        if (x < linear_threshold)
            return x;
        return linear_threshold + ((1.0f - static_cast<f32>(AK::exp(-static_cast<f64>(k) * static_cast<f64>(x - linear_threshold)))) / k);
    };

    f32 knee_k = 0.0f;
    f32 yknee_threshold_db = knee_threshold_db;
    if (knee_db > 0.0f) {
        auto slope_at = [&](f32 x, f32 k) -> f32 {
            if (x < linear_threshold)
                return 1.0f;
            f32 const x2 = x * 1.001f;
            f32 const x_db = linear_to_db(x);
            f32 const x2_db = linear_to_db(x2);
            f32 const y_db = linear_to_db(knee_curve(x, k));
            f32 const y2_db = linear_to_db(knee_curve(x2, k));
            return (y2_db - y_db) / (x2_db - x_db);
        };

        f32 min_k = 0.1f;
        f32 max_k = 10000.0f;
        knee_k = 5.0f;
        f32 const x = db_to_linear(knee_threshold_db);

        for (int i = 0; i < 15; ++i) {
            f32 const current_slope = slope_at(x, knee_k);
            if (current_slope < slope)
                max_k = knee_k;
            else
                min_k = knee_k;
            knee_k = static_cast<f32>(AK::sqrt(static_cast<f64>(min_k * max_k)));
        }

        yknee_threshold_db = linear_to_db(knee_curve(knee_threshold, knee_k));
    }

    auto saturate = [&](f32 x) -> f32 {
        if (knee_db <= 0.0f) {
            if (x < linear_threshold)
                return x;
            f32 const x_db = linear_to_db(x);
            f32 const y_db = threshold_db + (slope * (x_db - threshold_db));
            return db_to_linear(y_db);
        }

        if (x < knee_threshold)
            return knee_curve(x, knee_k);

        f32 const x_db = linear_to_db(x);
        f32 const y_db = yknee_threshold_db + (slope * (x_db - knee_threshold_db));
        return db_to_linear(y_db);
    };

    f32 const full_range_gain = saturate(1.0f);
    f32 const makeup_gain = full_range_gain <= 0.0f
        ? 1.0f
        : static_cast<f32>(AK::pow(static_cast<f64>(1.0f / full_range_gain), 0.6));

    f32 detector_average = m_detector_average;
    f32 compressor_gain = m_compressor_gain;
    f32 max_attack_compression_diff_db = m_max_attack_compression_diff_db;
    f32 metering_db = m_metering_db;

    size_t const frames = m_output.frame_count();

    f32 const attack_frames = max(max(attack_seconds, 0.001f) * context.sample_rate, 1.0f);
    f32 const release_frames = max(release_seconds * context.sample_rate, 1.0f);

    f32 const sat_release_time_seconds = 0.0025f;
    f32 const sat_release_frames = max(sat_release_time_seconds * context.sample_rate, 1.0f);
    f32 const metering_release_time_seconds = 0.325f;
    f32 const metering_release_frames = max(metering_release_time_seconds * context.sample_rate, 1.0f);
    f32 const metering_release_coeff = 1.0f - static_cast<f32>(AK::exp(-1.0 / static_cast<f64>(metering_release_frames)));

    // These constants, and the adaptive envelope rate-logic, were lifted straight from Blink code:
    // https://raw.githubusercontent.com/chromium/chromium/main/third_party/blink/renderer/platform/audio/dynamics_compressor.cc
    // This is a 4th order polynomial curve fit to (0, y1) (1, y2) (2, y3) (3, y4)
    f32 const y1 = release_frames * 0.09f;
    f32 const y2 = release_frames * 0.16f;
    f32 const y3 = release_frames * 0.42f;
    f32 const y4 = release_frames * 0.98f;
    f32 const release_kA = (0.9999999999999998f * y1) + (1.8432219684323923e-16f * y2) - (1.9373394351676423e-16f * y3) + (8.824516011816245e-18f * y4);
    f32 const release_kB = (-1.5788320352845888f * y1) + (2.3305837032074286f * y2) - (0.9141194204840429f * y3) + (0.1623677525612032f * y4);
    f32 const release_kC = (0.5334142869106424f * y1) - (1.272736789213631f * y2) + (0.9258856042207512f * y3) - (0.18656310191776226f * y4);
    f32 const release_kD = (0.08783463138207234f * y1) - (0.1694162967925622f * y2) + (0.08588057951595272f * y3) - (0.00429891410546283f * y4);
    f32 const release_kE = (-0.042416883008123074f * y1) + (0.1115693827987602f * y2) - (0.09764676325265872f * y3) + (0.028494263462021576f * y4);

    f32 last_metering_db = 0.0f;
    size_t const effective_delay_frames = min(delay_frames, m_ring_size - 1);
    constexpr size_t division_frames = 32;
    size_t frame_index = 0;

    auto process_frame = [&](f32 scaled_desired_gain, f32 envelope_rate) {
        f32 input_magnitude = 0.0f;
        if (mixed_input) {
            for (size_t ch = 0; ch < output_channels; ++ch) {
                input_magnitude = max(input_magnitude, static_cast<f32>(AK::fabs(mixed_input->channel(ch)[frame_index])));
            }
        }

        // If absolute value of input is less than 0.0001, attenuation is 1.0.
        f32 attenuation = 1.0f;
        if (input_magnitude >= 0.0001f) {
            f32 const shaped_input = saturate(input_magnitude);
            attenuation = shaped_input / input_magnitude;
        }

        f32 const target_gain = clamp(attenuation, 0.0f, 1.0f);
        f32 const attenuation_db = max(2.0f, -linear_to_db(target_gain));
        f32 const sat_db_per_frame = attenuation_db / sat_release_frames;
        f32 const sat_release_rate = db_to_linear(sat_db_per_frame) - 1.0f;
        detector_average += (target_gain - detector_average) * (target_gain > detector_average ? sat_release_rate : 1.0f);
        detector_average = clamp(detector_average, 0.0f, 1.0f);
        if (!__builtin_isfinite(detector_average))
            detector_average = 1.0f;

        if (envelope_rate < 1.0f) {
            compressor_gain += (scaled_desired_gain - compressor_gain) * envelope_rate;
        } else {
            compressor_gain *= envelope_rate;
            compressor_gain = min(compressor_gain, 1.0f);
        }

        compressor_gain = clamp(compressor_gain, 0.0f, 1.0f);

        f32 const post_warp_compressor_gain = static_cast<f32>(AK::sin((AK::Pi<f64> * 0.5) * static_cast<f64>(compressor_gain)));
        f32 const reduction_gain = post_warp_compressor_gain * makeup_gain;

        // Metering follows warped compressor gain before makeup gain.
        f32 const db_real_gain = linear_to_db(post_warp_compressor_gain);
        if (db_real_gain < metering_db) {
            metering_db = db_real_gain;
        } else {
            metering_db += (db_real_gain - metering_db) * metering_release_coeff;
        }
        last_metering_db = min(0.0f, metering_db);

        size_t const delayed_index = (m_write_index + m_ring_size - effective_delay_frames) % m_ring_size;

        // Write current input sample into the delay line.
        for (size_t ch = 0; ch < output_channels; ++ch) {
            m_ring[ch][m_write_index] = (mixed_input && ch < input_channels) ? mixed_input->channel(ch)[frame_index] : 0.0f;
        }

        // Read delayed sample and apply reduction gain.
        for (size_t ch = 0; ch < m_output.channel_count(); ++ch) {
            f32 const delayed = m_ring[ch][delayed_index];
            f32 const reduced = delayed * reduction_gain;
            m_output.channel(ch)[frame_index] = reduced;
        }

        m_write_index = (m_write_index + 1) % m_ring_size;
        if (m_frames_written < m_ring_size)
            ++m_frames_written;
        ++frame_index;
    };

    auto compute_envelope_rate = [&](f32& out_scaled_desired_gain, f32& out_envelope_rate) {
        if (!__builtin_isfinite(detector_average))
            detector_average = 1.0f;

        f32 const desired_gain = detector_average;
        out_scaled_desired_gain = static_cast<f32>(AK::asin(desired_gain) / (AK::Pi<f64> * 0.5));
        bool const is_releasing = out_scaled_desired_gain > compressor_gain;

        f32 compression_diff_db = is_releasing ? -1.0f : 1.0f;
        if (out_scaled_desired_gain > 0.0f)
            compression_diff_db = linear_to_db(compressor_gain / out_scaled_desired_gain);

        out_envelope_rate = 0.0f;
        if (is_releasing) {
            max_attack_compression_diff_db = -1.0f;
            if (!__builtin_isfinite(compression_diff_db))
                compression_diff_db = -1.0f;

            f32 x = clamp(compression_diff_db, -12.0f, 0.0f);
            x = 0.25f * (x + 12.0f);
            f32 const x2 = x * x;
            f32 const x3 = x2 * x;
            f32 const x4 = x2 * x2;
            f32 const adaptive_release_frames = release_kA + (release_kB * x) + (release_kC * x2) + (release_kD * x3) + (release_kE * x4);

            f32 const db_per_frame = 5.0f / adaptive_release_frames;
            out_envelope_rate = db_to_linear(db_per_frame);
        } else {
            if (!__builtin_isfinite(compression_diff_db))
                compression_diff_db = 1.0f;
            if (max_attack_compression_diff_db < 0.0f || max_attack_compression_diff_db < compression_diff_db)
                max_attack_compression_diff_db = compression_diff_db;

            f32 const effective_attenuation_diff_db = max(0.5f, max_attack_compression_diff_db);
            f32 const x = 0.25f / effective_attenuation_diff_db;
            out_envelope_rate = 1.0f - static_cast<f32>(AK::pow(static_cast<f64>(x), 1.0 / static_cast<f64>(attack_frames)));
        }
    };

    while (frame_index < frames) {
        f32 scaled_desired_gain = 0.0f;
        f32 envelope_rate = 0.0f;
        compute_envelope_rate(scaled_desired_gain, envelope_rate);
        size_t const frames_this_division = min(division_frames, frames - frame_index);
        for (size_t j = 0; j < frames_this_division; ++j)
            process_frame(scaled_desired_gain, envelope_rate);
    }

    m_detector_average = detector_average;
    m_compressor_gain = compressor_gain;
    m_max_attack_compression_diff_db = max_attack_compression_diff_db;
    m_metering_db = metering_db;

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
