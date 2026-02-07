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
    // https://webaudio.github.io/web-audio-api/#mediaelementaudiosourcenode
    // The output of this node is the audio from the associated HTMLMediaElement.

    // Channel count is derived from the tapped media stream.
    // Keep at least 1 channel to avoid a 0-channel AudioBus.
    u32 const provider_channels = m_provider->channel_count();
    size_t const desired_channel_count = max<size_t>(1, static_cast<size_t>(provider_channels));

    // If the media element has more channels than we expected, grow our preallocated storage.
    // NOTE: This is allowed to allocate on the render thread, but should be very rare in practice.
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

    auto const quantum_frames = m_output.frame_count();
    // NOTE: `RenderContext::sample_rate` is a float and may not be an exact integer.
    // Use a rounded integer rate for time/frame conversions and equality checks so we don't
    // accidentally engage SRC at a nominal 1:1 ratio.
    u32 const context_sample_rate = AK::round_to<u32>(context.sample_rate);
    auto const context_time = AK::Duration::from_seconds_f64(static_cast<f64>(context.current_frame) / static_cast<f64>(context_sample_rate));

    auto peek_result = m_provider->peek_with_timing();
    if (m_last_timeline_generation != 0 && peek_result.timeline_generation != 0 && peek_result.timeline_generation != m_last_timeline_generation) {
        m_media_to_context_offset.clear();
        // The provider has declared a discontinuity (seek/flush/retime). Reset the SRC
        // state so we do not filter across unrelated audio history.
        m_resampler_initialized = false;
        m_resample_input_start_frame = 0;
        m_resample_input_pending_frames = 0;
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

    size_t leading_silence_frames = 0;
    if (m_media_to_context_offset.has_value() && peek_result.start_time.has_value()) {
        auto expected_media_time = context_time - m_media_to_context_offset.value();
        auto delta = peek_result.start_time.value() - expected_media_time;

        f64 const delta_seconds = delta.to_seconds_f64();
        f64 const deadband_seconds = 0.005;

        // For small drift, avoid hard discontinuities (skip/silence) and just retune the
        // mapping between media time and context time.
        f64 const snap_seconds = 0.02;
        if (AK::fabs(delta_seconds) <= snap_seconds || delta_seconds < -deadband_seconds) {
            m_media_to_context_offset = m_media_to_context_offset.value() - delta;
        } else if (delta_seconds > deadband_seconds) {
            f64 silence_frames_f64 = delta_seconds * static_cast<f64>(context_sample_rate);
            leading_silence_frames = static_cast<size_t>(min<f64>(static_cast<f64>(quantum_frames), ceil(silence_frames_f64)));
        }
    }

    if (leading_silence_frames > 0 && should_log_media_element_bridge()) {
        static Atomic<i64> s_last_silence_log_ms { 0 };
        i64 now_ms = AK::MonotonicTime::now().milliseconds();
        i64 last_ms = s_last_silence_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
        if ((now_ms - last_ms) >= 250 && s_last_silence_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
            WA_MEDIA_DBGLN("[WebAudio] media-source node: cid={} session={} provider={} leading silence frames={} quantum={} provider_sr={} gen={} start_time_ms={}",
                m_provider->debug_client_id(),
                m_provider->debug_session_id(),
                m_provider->provider_id(),
                leading_silence_frames,
                quantum_frames,
                m_provider->sample_rate(),
                peek_result.timeline_generation,
                peek_result.start_time.has_value() ? peek_result.start_time->to_milliseconds() : -1);

            WA_MEDIA_DBGLN("[WebAudio] media-source node: cid={} session={} provider={} leading silence ctx_frame={} ctx_time_ms={} avail={} eos={}",
                m_provider->debug_client_id(),
                m_provider->debug_session_id(),
                m_provider->provider_id(),
                context.current_frame,
                context_time.to_milliseconds(),
                peek_result.available_frames,
                peek_result.end_of_stream);
        }
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

    size_t const output_frames_to_fill = quantum_frames - leading_silence_frames;
    if (output_frames_to_fill == 0)
        return;

    // If the media stream already matches the context's sample rate, avoid running it through
    // the windowed-sinc SRC. Even at a nominal ratio of 1.0, filtering can introduce tiny
    // numeric differences that are observable in strict sample-compare tests.
    if (provider_sample_rate == context_sample_rate) {
        Vector<Span<f32>, 8> output_pop_spans;
        output_pop_spans.resize(output_channel_count);
        for (size_t ch = 0; ch < output_channel_count; ++ch)
            output_pop_spans[ch] = output_spans[ch].slice(leading_silence_frames, output_frames_to_fill);

        auto pop_result = m_provider->pop_planar_with_timing(output_pop_spans.span(), output_frames_to_fill, output_channel_count);
        if (pop_result.frames_read < output_frames_to_fill) {
            if (should_log_media_element_bridge()) {
                static Atomic<i64> s_last_underrun_log_ms { 0 };
                i64 now_ms = AK::MonotonicTime::now().milliseconds();
                i64 last_ms = s_last_underrun_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
                if ((now_ms - last_ms) >= 250 && s_last_underrun_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
                    WA_MEDIA_DBGLN("[WebAudio] media-source node: cid={} session={} provider={} UNDERRUN (no-SRC) want_frames={} got_frames={} padded_frames={} eos={} gen={} pushed={} popped={}",
                        m_provider->debug_client_id(),
                        m_provider->debug_session_id(),
                        m_provider->provider_id(),
                        output_frames_to_fill,
                        pop_result.frames_read,
                        output_frames_to_fill - pop_result.frames_read,
                        pop_result.end_of_stream,
                        pop_result.timeline_generation,
                        m_provider->debug_total_frames_pushed(),
                        m_provider->debug_total_frames_popped());

                    WA_MEDIA_DBGLN("[WebAudio] media-source node: cid={} session={} provider={} UNDERRUN (no-SRC) ctx_frame={} ctx_time_ms={} avail_at_peek={} provider_sr={} out_ch={}",
                        m_provider->debug_client_id(),
                        m_provider->debug_session_id(),
                        m_provider->provider_id(),
                        context.current_frame,
                        context_time.to_milliseconds(),
                        peek_result.available_frames,
                        provider_sample_rate,
                        output_channel_count);
                }
            }
            for (size_t ch = 0; ch < output_channel_count; ++ch) {
                auto tail = output_pop_spans[ch].slice(pop_result.frames_read);
                tail.fill(0.0f);
            }
        }
        return;
    }

    // Target a stable buffer fill level by gently adjusting the SRC ratio.
    // This handles long-term clock drift without audible discontinuities.
    f64 const base_ratio = static_cast<f64>(provider_sample_rate) / static_cast<f64>(context_sample_rate);
    f64 ratio_target = base_ratio;

    size_t const capacity_frames = m_provider->capacity_frames();
    if (capacity_frames > 0) {
        f64 const target_fill = static_cast<f64>(capacity_frames) * 0.5;
        f64 const available = static_cast<f64>(peek_result.available_frames);
        f64 const error = (available - target_fill) / target_fill;

        // Keep the adjustment very small (ppm-scale).
        f64 const max_ppm = 0.002;
        f64 const k_p = 0.001;
        f64 drift = clamp(error * k_p, -max_ppm, max_ppm);
        ratio_target = base_ratio * (1.0 + drift);
    }

    if (!m_resampler_initialized
        || m_resample_last_provider_sample_rate != provider_sample_rate
        || m_resample_last_channel_count != output_channel_count) {
        // Ring size is chosen to avoid frequent wraparound and keep tap windows available.
        sample_rate_converter_init(m_resampler, output_channel_count, ratio_target, 4096);
        m_resampler_initialized = true;
        m_resample_ratio_smoothed = ratio_target;
        m_resample_last_provider_sample_rate = provider_sample_rate;
        m_resample_last_channel_count = output_channel_count;

        size_t const resample_input_capacity_frames = quantum_frames * 64;
        m_resample_input_channels.resize(output_channel_count);
        for (auto& channel : m_resample_input_channels)
            channel.resize(resample_input_capacity_frames);
        m_resample_input_start_frame = 0;
        m_resample_input_pending_frames = 0;
    }

    // Smooth ratio updates to avoid adding jitter.
    f64 const alpha = 0.01;
    m_resample_ratio_smoothed += (ratio_target - m_resample_ratio_smoothed) * alpha;
    sample_rate_converter_set_ratio(m_resampler, m_resample_ratio_smoothed);

    size_t const resample_input_capacity_frames = m_resample_input_channels.is_empty() ? (quantum_frames * 64) : m_resample_input_channels[0].size();
    size_t const taps = SincResamplerKernel::tap_count;
    size_t const estimated_input_frames = static_cast<size_t>(ceil(static_cast<f64>(output_frames_to_fill) * m_resample_ratio_smoothed)) + taps;
    size_t required_input_frames = m_resample_input_pending_frames + estimated_input_frames;
    required_input_frames = min(required_input_frames, resample_input_capacity_frames);

    if (m_resample_input_start_frame + required_input_frames > resample_input_capacity_frames) {
        if (m_resample_input_pending_frames > 0) {
            for (auto& channel : m_resample_input_channels)
                memmove(channel.data(), channel.data() + m_resample_input_start_frame, m_resample_input_pending_frames * sizeof(f32));
        }
        m_resample_input_start_frame = 0;
    }

    size_t const write_offset = m_resample_input_start_frame + m_resample_input_pending_frames;
    size_t frames_to_fetch = required_input_frames > m_resample_input_pending_frames ? (required_input_frames - m_resample_input_pending_frames) : 0;
    if (write_offset + frames_to_fetch > resample_input_capacity_frames)
        frames_to_fetch = resample_input_capacity_frames - write_offset;

    Optional<AK::Duration> pop_start_time;
    u64 pop_generation = 0;
    if (frames_to_fetch > 0) {
        Vector<Span<f32>, 8> input_pop_spans;
        input_pop_spans.resize(output_channel_count);
        for (size_t ch = 0; ch < output_channel_count; ++ch)
            input_pop_spans[ch] = Span<f32> { m_resample_input_channels[ch].data() + write_offset, frames_to_fetch };

        auto pop_result = m_provider->pop_planar_with_timing(input_pop_spans.span(), frames_to_fetch, output_channel_count);
        pop_start_time = pop_result.start_time;
        pop_generation = pop_result.timeline_generation;

        size_t frames_read = pop_result.frames_read;
        if (frames_read < frames_to_fetch) {
            if (should_log_media_element_bridge()) {
                static Atomic<i64> s_last_underrun_log_ms { 0 };
                i64 now_ms = AK::MonotonicTime::now().milliseconds();
                i64 last_ms = s_last_underrun_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
                if ((now_ms - last_ms) >= 250 && s_last_underrun_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
                    WA_MEDIA_DBGLN("[WebAudio] media-source node: cid={} session={} provider={} UNDERRUN (SRC) want_in_frames={} got_in_frames={} padded_in_frames={} eos={} gen={} avail_at_peek={}",
                        m_provider->debug_client_id(),
                        m_provider->debug_session_id(),
                        m_provider->provider_id(),
                        frames_to_fetch,
                        frames_read,
                        frames_to_fetch - frames_read,
                        pop_result.end_of_stream,
                        pop_result.timeline_generation,
                        peek_result.available_frames);

                    WA_MEDIA_DBGLN("[WebAudio] media-source node: cid={} session={} provider={} UNDERRUN (SRC) ctx_frame={} ctx_time_ms={} provider_sr={} out_ch={} ratio={}",
                        m_provider->debug_client_id(),
                        m_provider->debug_session_id(),
                        m_provider->provider_id(),
                        context.current_frame,
                        context_time.to_milliseconds(),
                        provider_sample_rate,
                        output_channel_count,
                        m_resample_ratio_smoothed);
                }
            }
            for (size_t ch = 0; ch < output_channel_count; ++ch) {
                auto tail = input_pop_spans[ch].slice(frames_read);
                tail.fill(0.0f);
            }
        }

        // FIXME: The resampler needs a contiguous input window to produce output.
        // When the provider underruns, we still appended silence into our input buffer above.
        // Count those silent frames as pending so processing can make forward progress.
        m_resample_input_pending_frames += frames_to_fetch;
    }

    Vector<ReadonlySpan<f32>, 8> input_spans;
    input_spans.resize(output_channel_count);
    for (size_t ch = 0; ch < output_channel_count; ++ch)
        input_spans[ch] = ReadonlySpan<f32> { m_resample_input_channels[ch].data() + m_resample_input_start_frame, m_resample_input_pending_frames };

    Vector<Span<f32>, 8> output_resample_spans;
    output_resample_spans.resize(output_channel_count);
    for (size_t ch = 0; ch < output_channel_count; ++ch)
        output_resample_spans[ch] = output_spans[ch].slice(leading_silence_frames, output_frames_to_fill);

    auto resample_result = sample_rate_converter_process(m_resampler, input_spans.span(), output_resample_spans.span(), false);

    if (resample_result.input_frames_consumed > 0) {
        m_resample_input_start_frame += resample_result.input_frames_consumed;
        m_resample_input_pending_frames = m_resample_input_pending_frames >= resample_result.input_frames_consumed
            ? (m_resample_input_pending_frames - resample_result.input_frames_consumed)
            : 0;
    }

    if (resample_result.output_frames_produced == 0)
        return;

    if (should_log_media_element_bridge()) {
        static Atomic<i64> s_last_nonempty_log_ms { 0 };
        i64 now_ms = AK::MonotonicTime::now().milliseconds();
        i64 last_ms = s_last_nonempty_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
        if ((now_ms - last_ms) >= 1000 && s_last_nonempty_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
            WA_MEDIA_DBGLN("[WebAudio] media-source node: cid={} session={} provider={} frames_read={} silence={} out_ch={} provider_sr={} provider_ch={} start_time_ms={} gen={} pushed={} popped={}",
                m_provider->debug_client_id(),
                m_provider->debug_session_id(),
                m_provider->provider_id(),
                resample_result.output_frames_produced + leading_silence_frames,
                leading_silence_frames,
                output_channel_count,
                m_provider->sample_rate(),
                m_provider->channel_count(),
                pop_start_time.has_value() ? pop_start_time->to_milliseconds() : -1,
                pop_generation,
                m_provider->debug_total_frames_pushed(),
                m_provider->debug_total_frames_popped());
        }
    }
}

AudioBus const& MediaElementAudioSourceRenderNode::output(size_t) const
{
    return m_output;
}

}
