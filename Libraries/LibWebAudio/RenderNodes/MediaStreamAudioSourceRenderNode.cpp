/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebAudio/RenderNodes/MediaStreamAudioSourceRenderNode.h>

#include <AK/Math.h>
#include <LibWebAudio/Debug.h>

namespace Web::WebAudio::Render {

static constexpr i64 STALE_MEDIA_RESYNC_THRESHOLD_MS = 50;

MediaStreamAudioSourceRenderNode::MediaStreamAudioSourceRenderNode(
    NodeID node_id, NonnullRefPtr<MediaElementAudioSourceProvider> provider, size_t quantum_size)
    : RenderNode(node_id)
    , m_provider(move(provider))
    , m_output(1, quantum_size, max_channel_capacity)
{
}

void MediaStreamAudioSourceRenderNode::process(RenderContext& context,
    Vector<Vector<AudioBus const*>> const&,
    Vector<Vector<AudioBus const*>> const&)
{
    ASSERT_RENDER_THREAD();

    // https://webaudio.github.io/web-audio-api/#mediastreamaudiosourcenode
    // The output of this node corresponds to the audio from the selected MediaStreamTrack.

    u32 const provider_channels = m_provider->channel_count();
    size_t const desired_channel_count = max<size_t>(1, static_cast<size_t>(provider_channels));
    if (desired_channel_count > m_output.channel_capacity())
        m_output = m_output.clone_resized(desired_channel_count, m_output.frame_count(), desired_channel_count);

    size_t const output_channel_count = min(desired_channel_count, m_output.channel_capacity());
    m_output.set_channel_count(output_channel_count);
    m_output.zero();

    Vector<Span<f32>, 32> output_spans;
    output_spans.resize(output_channel_count);
    for (size_t channel = 0; channel < output_channel_count; ++channel)
        output_spans[channel] = m_output.channel(channel);

    size_t const quantum_frames = m_output.frame_count();
    u32 const context_sample_rate = AK::round_to<u32>(context.sample_rate);
    auto const context_time = AK::Duration::from_seconds_f64(static_cast<f64>(context.current_frame) / static_cast<f64>(context_sample_rate));
    auto peek_result = m_provider->peek_with_timing();
    if (m_last_timeline_generation != 0 && peek_result.timeline_generation != 0 && peek_result.timeline_generation != m_last_timeline_generation) {
        m_resample_input_start_frame = 0;
        m_resample_input_pending_frames = 0;
        m_resample_ratio_hold_quanta = 8;

        if (should_log_webaudio(LOG_MEDIA)) {
            WA_MEDIA_DBGLN("[WebAudio] media-stream source node: cid={} session={} provider={} discontinuity gen {} -> {}",
                m_provider->debug_client_id(), m_provider->debug_session_id(), m_provider->provider_id(),
                m_last_timeline_generation, peek_result.timeline_generation);
        }
    }
    m_last_timeline_generation = peek_result.timeline_generation;

    if (peek_result.available_frames == 0) {
        if (peek_result.end_of_stream)
            return;
        if (m_provider->wait_for_frames(1, 1))
            peek_result = m_provider->peek_with_timing();
        if (peek_result.available_frames == 0)
            return;
    }

    u32 const provider_sample_rate = m_provider->sample_rate();
    if (provider_sample_rate == 0)
        return;

    if (!m_media_to_context_offset.has_value() && peek_result.start_time.has_value())
        m_media_to_context_offset = context_time - peek_result.start_time.value();

    if (m_media_to_context_offset.has_value() && peek_result.start_time.has_value()) {
        auto expected_context_time = peek_result.start_time.value() + m_media_to_context_offset.value();
        auto delta = expected_context_time - context_time;
        i64 const delta_ms = delta.to_milliseconds();

        // https://webaudio.github.io/web-audio-api/#dom-audiocontext-suspend
        // While an AudioContext is suspended, MediaStreams will have their output ignored; that is, data will be lost by the real time nature of media streams.
        if (delta_ms <= -STALE_MEDIA_RESYNC_THRESHOLD_MS) {
            i64 const stale_ms = -delta_ms;
            size_t const stale_frames = static_cast<size_t>(AK::ceil((static_cast<double>(stale_ms) * static_cast<double>(provider_sample_rate)) / 1000.0));
            size_t const skipped_frames = m_provider->skip_frames(stale_frames);
            if (skipped_frames > 0) {
                m_resample_input_start_frame = 0;
                m_resample_input_pending_frames = 0;
                m_resample_ratio_hold_quanta = 8;
                peek_result = m_provider->peek_with_timing();

                if (should_log_webaudio(LOG_MEDIA)) {
                    WA_MEDIA_DBGLN("[WebAudio] media-stream source node: cid={} session={} provider={} skipped stale "
                                   "frames={} stale_ms={} ctx_time_ms={} gen={}",
                        m_provider->debug_client_id(), m_provider->debug_session_id(),
                        m_provider->provider_id(), skipped_frames, stale_ms, context_time.to_milliseconds(),
                        peek_result.timeline_generation);
                }
            }

            if (peek_result.start_time.has_value())
                m_media_to_context_offset = context_time - peek_result.start_time.value();
        }
    }

    if (!m_resampler_initialized || m_resample_last_provider_sample_rate != provider_sample_rate || m_resample_last_channel_count != output_channel_count) {
        m_resampler_initialized = true;
        m_resample_last_provider_sample_rate = provider_sample_rate;
        m_resample_last_channel_count = output_channel_count;
        m_resample_input_channels.clear();
        m_resample_input_channels.ensure_capacity(output_channel_count);
        for (size_t channel = 0; channel < output_channel_count; ++channel)
            m_resample_input_channels.unchecked_append(Vector<f32> {});
        m_resample_input_start_frame = 0;
        m_resample_input_pending_frames = 0;
        m_resample_ratio_smoothed = 1.0;

        f64 const base_ratio = static_cast<f64>(provider_sample_rate) / static_cast<f64>(context_sample_rate);
        sample_rate_converter_init(m_resampler, output_channel_count, base_ratio);
    }

    f64 const base_ratio = static_cast<f64>(provider_sample_rate) / static_cast<f64>(context_sample_rate);
    f64 ratio_target = base_ratio;
    if (m_resample_ratio_hold_quanta > 0) {
        --m_resample_ratio_hold_quanta;
    } else if (m_provider->capacity_frames() > 0) {
        size_t const capacity_frames = m_provider->capacity_frames();
        size_t const fill_frames = peek_result.available_frames;
        f64 const target_fill = static_cast<f64>(capacity_frames) * 0.5;
        f64 const fill_delta = static_cast<f64>(fill_frames) - target_fill;

        constexpr f64 k_p = 0.000003;
        ratio_target += fill_delta * k_p;
        ratio_target = clamp(ratio_target, base_ratio * 0.9975, base_ratio * 1.0025);
    }

    if (AK::fabs(m_resample_ratio_smoothed - ratio_target) > 0.0001)
        m_resample_ratio_smoothed += (ratio_target - m_resample_ratio_smoothed) * 0.1;
    else
        m_resample_ratio_smoothed = ratio_target;

    sample_rate_converter_set_ratio(m_resampler, m_resample_ratio_smoothed);

    size_t output_frames_produced = 0;
    while (output_frames_produced < quantum_frames) {
        if (m_resample_input_pending_frames == 0) {
            Vector<Span<f32>, 8> pop_spans;
            pop_spans.resize(output_channel_count);
            for (size_t channel = 0; channel < output_channel_count; ++channel) {
                m_resample_input_channels[channel].resize(quantum_frames);
                pop_spans[channel] = m_resample_input_channels[channel].span();
            }

            size_t frames_read = m_provider->pop_planar(pop_spans.span(), quantum_frames, output_channel_count);
            if (frames_read == 0 && m_provider->wait_for_frames(1, 1))
                frames_read = m_provider->pop_planar(pop_spans.span(), quantum_frames, output_channel_count);

            if (frames_read == 0) {
                for (size_t channel = 0; channel < output_channel_count; ++channel)
                    output_spans[channel].slice(output_frames_produced).fill(0.0f);
                return;
            }

            m_resample_input_start_frame = 0;
            m_resample_input_pending_frames = frames_read;
        }

        Vector<ReadonlySpan<f32>, 8> input_spans;
        input_spans.resize(output_channel_count);
        for (size_t channel = 0; channel < output_channel_count; ++channel) {
            input_spans[channel] = m_resample_input_channels[channel]
                                       .span()
                                       .slice(m_resample_input_start_frame, m_resample_input_pending_frames);
        }

        Vector<Span<f32>, 8> output_chunk_spans;
        output_chunk_spans.resize(output_channel_count);
        size_t const output_frames_remaining = quantum_frames - output_frames_produced;
        for (size_t channel = 0; channel < output_channel_count; ++channel)
            output_chunk_spans[channel] = output_spans[channel].slice(output_frames_produced, output_frames_remaining);

        auto const resample_result = sample_rate_converter_process(m_resampler, input_spans.span(), output_chunk_spans.span(), false);
        if (resample_result.input_frames_consumed == 0 && resample_result.output_frames_produced == 0) {
            for (size_t channel = 0; channel < output_channel_count; ++channel)
                output_spans[channel].slice(output_frames_produced).fill(0.0f);
            return;
        }

        m_resample_input_start_frame += resample_result.input_frames_consumed;
        m_resample_input_pending_frames -= resample_result.input_frames_consumed;
        output_frames_produced += resample_result.output_frames_produced;
    }
}

AudioBus const& MediaStreamAudioSourceRenderNode::output(size_t) const
{
    ASSERT_RENDER_THREAD();
    return m_output;
}

} // namespace Web::WebAudio::Render