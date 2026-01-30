/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/RenderNodes/MediaElementAudioSourceRenderNode.h>

#include <AK/Math.h>
#include <AK/Time.h>
#include <LibWeb/WebAudio/Debug.h>
#include <string.h>

namespace Web::WebAudio::Render {

MediaElementAudioSourceRenderNode::MediaElementAudioSourceRenderNode(NodeID node_id, NonnullRefPtr<MediaElementAudioSourceProvider> provider, size_t quantum_size)
    : RenderNode(node_id)
    , m_provider(move(provider))
    , m_output(1, quantum_size, max_channel_capacity)
{
}

void MediaElementAudioSourceRenderNode::process(RenderContext& context, Vector<Vector<AudioBus const*>> const&, Vector<Vector<AudioBus const*>> const&)
{
    ASSERT_RENDER_THREAD();

    // https://webaudio.github.io/web-audio-api/#mediaelementaudiosourcenode
    // The output of this node is the audio from the associated HTMLMediaElement.

    // Channel count is derived from the tapped media stream.
    // Keep at least 1 channel to avoid a 0-channel AudioBus.
    u32 const provider_channels = m_provider->channel_count();
    size_t const desired_channel_count = max<size_t>(1, static_cast<size_t>(provider_channels));

    // If the media element has more channels than we expected, grow our preallocated storage.
    // This render thread allocation should be very rare.
    if (desired_channel_count > m_output.channel_capacity()) {
        m_output = m_output.clone_resized(desired_channel_count, m_output.frame_count(), desired_channel_count);
    }

    size_t const output_channel_count = min(desired_channel_count, m_output.channel_capacity());

    m_output.set_channel_count(output_channel_count);
    m_output.zero();

    Vector<Span<f32>, 32> output_spans;
    output_spans.resize(output_channel_count);
    for (size_t ch = 0; ch < output_channel_count; ++ch)
        output_spans[ch] = m_output.channel(ch);

    size_t const quantum_frames = m_output.frame_count();
    u32 const context_sample_rate = AK::round_to<u32>(context.sample_rate);
    auto const context_time = AK::Duration::from_seconds_f64(static_cast<f64>(context.current_frame) / static_cast<f64>(context_sample_rate));

    auto peek_result = m_provider->peek_with_timing();
    if (m_last_timeline_generation != 0 && peek_result.timeline_generation != 0 && peek_result.timeline_generation != m_last_timeline_generation) {
        m_media_to_context_offset.clear();
        if (should_log_media_element_bridge())
            WA_MEDIA_DBGLN("[WebAudio] media-source node: cid={} session={} provider={} discontinuity gen {} -> {}",
                m_provider->debug_client_id(),
                m_provider->debug_session_id(),
                m_provider->provider_id(),
                m_last_timeline_generation,
                peek_result.timeline_generation);
    }
    m_last_timeline_generation = peek_result.timeline_generation;

    if (peek_result.available_frames == 0) {
        if (peek_result.end_of_stream)
            return;

        // The media element tap may deliver audio in bursts. On strict sample-compare tests,
        // a brief underrun (momentary empty ring) can inject mid-stream silence.
        // If a transport notify fd is available, wait briefly for more frames.
        if (m_provider->wait_for_frames(1, 1))
            peek_result = m_provider->peek_with_timing();

        if (peek_result.available_frames == 0) {
            if (peek_result.end_of_stream)
                return;

            if (should_log_media_element_bridge()) {
                static Atomic<i64> s_last_empty_log_ms { 0 };
                i64 now_ms = AK::MonotonicTime::now().milliseconds();
                i64 last_ms = s_last_empty_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
                if ((now_ms - last_ms) >= 500 && s_last_empty_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
                    WA_MEDIA_DBGLN("[WebAudio] media-source node: cid={} session={} provider={} EMPTY want_frames={} out_ch={} provider_sr={} provider_ch={} pushed={} popped={}",
                        m_provider->debug_client_id(),
                        m_provider->debug_session_id(),
                        m_provider->provider_id(),
                        quantum_frames,
                        output_channel_count,
                        m_provider->sample_rate(),
                        m_provider->channel_count(),
                        m_provider->debug_total_frames_pushed(),
                        m_provider->debug_total_frames_popped());

                    WA_MEDIA_DBGLN("[WebAudio] media-source node: cid={} session={} provider={} EMPTY ctx_frame={} ctx_time_ms={} gen={} avail={} eos={}",
                        m_provider->debug_client_id(),
                        m_provider->debug_session_id(),
                        m_provider->provider_id(),
                        context.current_frame,
                        context_time.to_milliseconds(),
                        peek_result.timeline_generation,
                        peek_result.available_frames,
                        peek_result.end_of_stream);
                }
            }
            return;
        }
    }

    if (!m_media_to_context_offset.has_value() && peek_result.start_time.has_value())
        m_media_to_context_offset = context_time - peek_result.start_time.value();

    if (m_media_to_context_offset.has_value() && peek_result.start_time.has_value()) {
        auto expected_context_time = peek_result.start_time.value() + m_media_to_context_offset.value();
        auto delta = expected_context_time - context_time;
        f64 delta_ms = static_cast<f64>(delta.to_milliseconds());

        // If the delta is too large, snap the offset to avoid adding large latency.
        if (AK::fabs(delta_ms) > 200.0 || delta_ms < -50.0)
            m_media_to_context_offset = m_media_to_context_offset.value() - delta;
    }

    u32 const provider_sample_rate = m_provider->sample_rate();
    if (provider_sample_rate == 0) {
        if (should_log_media_element_bridge()) {
            static Atomic<i64> s_last_sr0_log_ms { 0 };
            i64 now_ms = AK::MonotonicTime::now().milliseconds();
            i64 last_ms = s_last_sr0_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
            if ((now_ms - last_ms) >= 500 && s_last_sr0_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
                WA_MEDIA_DBGLN("[WebAudio] media-source node: cid={} session={} provider={} provider sample_rate=0 (avail={} eos={} gen={} pushed={} popped={})",
                    m_provider->debug_client_id(),
                    m_provider->debug_session_id(),
                    m_provider->provider_id(),
                    peek_result.available_frames,
                    peek_result.end_of_stream,
                    peek_result.timeline_generation,
                    m_provider->debug_total_frames_pushed(),
                    m_provider->debug_total_frames_popped());

                WA_MEDIA_DBGLN("[WebAudio] media-source node: cid={} session={} provider={} provider sample_rate=0 ctx_frame={} ctx_time_ms={}",
                    m_provider->debug_client_id(),
                    m_provider->debug_session_id(),
                    m_provider->provider_id(),
                    context.current_frame,
                    context_time.to_milliseconds());
            }
        }
        return;
    }

    size_t const output_frames_to_fill = quantum_frames;
    if (!m_resampler_initialized || m_resample_last_provider_sample_rate != provider_sample_rate || m_resample_last_channel_count != output_channel_count) {
        m_resampler_initialized = true;
        m_resample_last_provider_sample_rate = provider_sample_rate;
        m_resample_last_channel_count = output_channel_count;
        m_resample_input_channels.clear();
        m_resample_input_channels.ensure_capacity(output_channel_count);
        for (size_t ch = 0; ch < output_channel_count; ++ch)
            m_resample_input_channels.unchecked_append(Vector<f32> {});
        m_resample_input_start_frame = 0;
        m_resample_input_pending_frames = 0;
        m_resample_ratio_smoothed = 1.0;

        f64 const base_ratio = static_cast<f64>(provider_sample_rate) / static_cast<f64>(context_sample_rate);
        sample_rate_converter_init(m_resampler, output_channel_count, base_ratio);
    }

    f64 base_ratio = static_cast<f64>(provider_sample_rate) / static_cast<f64>(context_sample_rate);
    f64 ratio_target = base_ratio;
    if (m_provider->capacity_frames() > 0) {
        size_t capacity_frames = m_provider->capacity_frames();
        size_t fill_frames = peek_result.available_frames;
        f64 target_fill = static_cast<f64>(capacity_frames) * 0.5;
        f64 fill_delta = static_cast<f64>(fill_frames) - target_fill;
        f64 k_p = 0.00002;
        ratio_target += fill_delta * k_p;
        ratio_target = clamp(ratio_target, base_ratio * 0.98, base_ratio * 1.02);
    }

    if (AK::fabs(m_resample_ratio_smoothed - ratio_target) > 0.0001)
        m_resample_ratio_smoothed += (ratio_target - m_resample_ratio_smoothed) * 0.1;
    else
        m_resample_ratio_smoothed = ratio_target;

    sample_rate_converter_set_ratio(m_resampler, m_resample_ratio_smoothed);

    size_t output_frames_produced = 0;
    while (output_frames_produced < output_frames_to_fill) {
        if (m_resample_input_pending_frames == 0) {
            size_t max_input_frames = output_frames_to_fill;
            Vector<Span<f32>, 8> pop_spans;
            pop_spans.resize(output_channel_count);
            for (size_t ch = 0; ch < output_channel_count; ++ch) {
                m_resample_input_channels[ch].resize(max_input_frames);
                pop_spans[ch] = m_resample_input_channels[ch].span();
            }

            size_t frames_read = m_provider->pop_planar(pop_spans.span(), max_input_frames, output_channel_count);
            if (frames_read == 0) {
                for (size_t ch = 0; ch < output_channel_count; ++ch)
                    output_spans[ch].slice(output_frames_produced).fill(0.0f);
                return;
            }

            m_resample_input_start_frame = 0;
            m_resample_input_pending_frames = frames_read;
        }

        Vector<ReadonlySpan<f32>, 8> input_spans;
        input_spans.resize(output_channel_count);
        for (size_t ch = 0; ch < output_channel_count; ++ch)
            input_spans[ch] = m_resample_input_channels[ch].span().slice(m_resample_input_start_frame, m_resample_input_pending_frames);

        Vector<Span<f32>, 8> output_chunk_spans;
        output_chunk_spans.resize(output_channel_count);
        size_t output_frames_remaining = output_frames_to_fill - output_frames_produced;
        for (size_t ch = 0; ch < output_channel_count; ++ch)
            output_chunk_spans[ch] = output_spans[ch].slice(output_frames_produced, output_frames_remaining);

        auto resample_result = sample_rate_converter_process(m_resampler, input_spans.span(), output_chunk_spans.span(), false);
        if (resample_result.input_frames_consumed == 0 && resample_result.output_frames_produced == 0) {
            for (size_t ch = 0; ch < output_channel_count; ++ch)
                output_spans[ch].slice(output_frames_produced).fill(0.0f);
            return;
        }

        m_resample_input_start_frame += resample_result.input_frames_consumed;
        m_resample_input_pending_frames -= resample_result.input_frames_consumed;
        output_frames_produced += resample_result.output_frames_produced;
    }
}

AudioBus const& MediaElementAudioSourceRenderNode::output(size_t) const
{
    ASSERT_RENDER_THREAD();
    return m_output;
}

}
