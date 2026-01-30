/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/Time.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/StreamTransportNotify.h>
#include <LibWeb/WebAudio/Engine/StreamTransportRing.h>
#include <LibWeb/WebAudio/MediaElementAudioSourceProvider.h>
#include <poll.h>

namespace Web::WebAudio {

MediaElementAudioSourceProvider::~MediaElementAudioSourceProvider()
{
    if (!m_consumer.has<TransportConsumerState>())
        return;
    TransportConsumerState& transport = m_consumer.get<TransportConsumerState>();
    if (transport.notify_read_fd >= 0) {
        (void)::close(transport.notify_read_fd);
        transport.notify_read_fd = -1;
    }
}

void MediaElementAudioSourceProvider::set_debug_connection_info(int client_id, u64 session_id)
{
    ASSERT_CONTROL_THREAD();
    m_debug_client_id = client_id;
    m_debug_session_id = session_id;
    m_has_debug_connection_info.store(true, AK::MemoryOrder::memory_order_release);
}

void MediaElementAudioSourceProvider::declare_discontinuity()
{
    if (m_consumer.has<TransportConsumerState>())
        return;

    LocalConsumerState& local = *m_consumer.get<LocalConsumerPtr>();
    u64 write_frame = Render::ring_stream_load_write_frame(local.header);
    Render::ring_stream_store_read_frame(local.header, write_frame);
    Render::ring_stream_store_write_frame(local.header, write_frame);

    AK::atomic_store(&local.header.timeline_sample_rate, 0u, AK::MemoryOrder::memory_order_relaxed);
    (void)AK::atomic_fetch_add(&local.header.timeline_generation, static_cast<u64>(1), AK::MemoryOrder::memory_order_release);

    Render::ring_stream_clear_flag(local.header, Render::ring_stream_flag_end_of_stream);
    Render::ring_stream_clear_producer_timestamp_anchor(local.header);

    if (should_log_media_element_bridge()) {
        i64 now_ms = AK::MonotonicTime::now().milliseconds();
        i64 last_ms = m_last_discontinuity_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
        if ((now_ms - last_ms) >= 250 && m_last_discontinuity_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
            u64 const flags = Render::ring_stream_load_flags(local.header);
            u32 const timeline_sample_rate = AK::atomic_load(&local.header.timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
            u64 const generation = AK::atomic_load(&local.header.timeline_generation, AK::MemoryOrder::memory_order_relaxed);
            u64 const read_frame = Render::ring_stream_load_read_frame(local.header);
            u64 const write_frame = Render::ring_stream_load_write_frame(local.header);
            WA_MEDIA_DBGLN("[WebAudio] media-tap discontinuity: cid={} session={} provider={} read={} write={} gen={} flags={} timeline_sr={}",
                debug_client_id(),
                debug_session_id(),
                m_provider_id,
                read_frame,
                write_frame,
                generation,
                flags,
                timeline_sample_rate);
        }
    }

    if (m_stream_transport_producer_view.has_value()) {
        auto& view = *m_stream_transport_producer_view;
        Render::RingStreamHeader& header = *view.header;
        u64 stream_write_frame = Render::ring_stream_load_write_frame(header);
        Render::ring_stream_store_read_frame(header, stream_write_frame);
        Render::ring_stream_store_write_frame(header, stream_write_frame);

        AK::atomic_store(&header.timeline_sample_rate, 0u, AK::MemoryOrder::memory_order_relaxed);
        (void)AK::atomic_fetch_add(&header.timeline_generation, static_cast<u64>(1), AK::MemoryOrder::memory_order_release);

        Render::ring_stream_clear_flag(header, Render::ring_stream_flag_end_of_stream);
        Render::ring_stream_clear_producer_timestamp_anchor(header);

        if (m_stream_transport_notify_write_fd >= 0)
            (void)Render::try_signal_stream_notify_fd(m_stream_transport_notify_write_fd);
    }
}

void MediaElementAudioSourceProvider::declare_end_of_stream()
{
    if (m_consumer.has<TransportConsumerState>())
        return;

    LocalConsumerState& local = *m_consumer.get<LocalConsumerPtr>();

    if (should_log_media_element_bridge()) {
        u64 const flags_before = Render::ring_stream_load_flags(local.header);
        bool const was_eos = (flags_before & Render::ring_stream_flag_end_of_stream) != 0;
        if (!was_eos) {
            i64 now_ms = AK::MonotonicTime::now().milliseconds();
            i64 last_ms = m_last_eos_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
            if ((now_ms - last_ms) >= 250 && m_last_eos_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
                u64 const read_frame = Render::ring_stream_load_read_frame(local.header);
                u64 const write_frame = Render::ring_stream_load_write_frame(local.header);
                u64 const generation = AK::atomic_load(&local.header.timeline_generation, AK::MemoryOrder::memory_order_relaxed);
                u32 const timeline_sample_rate = AK::atomic_load(&local.header.timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
                Render::RingStreamProducerTimestampAnchor const anchor = Render::ring_stream_load_producer_timestamp_anchor(local.header);
                WA_MEDIA_DBGLN("[WebAudio] media-tap declare-eos: cid={} session={} provider={} read={} write={} gen={} flags_before={} timeline_sr={} anchor_gen={} anchor_media={} anchor_ring={}",
                    debug_client_id(),
                    debug_session_id(),
                    m_provider_id,
                    read_frame,
                    write_frame,
                    generation,
                    flags_before,
                    timeline_sample_rate,
                    anchor.generation,
                    anchor.media_start_frame,
                    anchor.media_start_at_ring_frame);
            }
        }
    }

    Render::ring_stream_set_flag(local.header, Render::ring_stream_flag_end_of_stream);

    if (m_stream_transport_producer_view.has_value()) {
        auto& view = *m_stream_transport_producer_view;
        Render::RingStreamHeader& header = *view.header;

        if (should_log_media_element_bridge()) {
            u64 const flags_before = Render::ring_stream_load_flags(header);
            bool const was_eos = (flags_before & Render::ring_stream_flag_end_of_stream) != 0;
            if (!was_eos) {
                i64 now_ms = AK::MonotonicTime::now().milliseconds();
                i64 last_ms = m_last_eos_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
                if ((now_ms - last_ms) >= 250 && m_last_eos_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
                    u64 const read_frame = Render::ring_stream_load_read_frame(header);
                    u64 const write_frame = Render::ring_stream_load_write_frame(header);
                    u64 const generation = AK::atomic_load(&header.timeline_generation, AK::MemoryOrder::memory_order_relaxed);
                    u32 const timeline_sample_rate = AK::atomic_load(&header.timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
                    Render::RingStreamProducerTimestampAnchor const anchor = Render::ring_stream_load_producer_timestamp_anchor(header);
                    WA_MEDIA_DBGLN("[WebAudio] media-tap declare-eos (transport): cid={} session={} provider={} read={} write={} gen={} flags_before={} timeline_sr={} anchor_gen={} anchor_media={} anchor_ring={}",
                        debug_client_id(),
                        debug_session_id(),
                        m_provider_id,
                        read_frame,
                        write_frame,
                        generation,
                        flags_before,
                        timeline_sample_rate,
                        anchor.generation,
                        anchor.media_start_frame,
                        anchor.media_start_at_ring_frame);
                }
            }
        }

        Render::ring_stream_set_flag(header, Render::ring_stream_flag_end_of_stream);
        if (m_stream_transport_notify_write_fd >= 0)
            (void)Render::try_signal_stream_notify_fd(m_stream_transport_notify_write_fd);
    }
}

NonnullRefPtr<MediaElementAudioSourceProvider> MediaElementAudioSourceProvider::create(size_t channel_capacity, size_t capacity_frames)
{
    ASSERT_CONTROL_THREAD();
    LocalConsumerPtr state = make<LocalConsumerState>();
    VERIFY(channel_capacity > 0);
    VERIFY(capacity_frames > 0);
    state->header.channel_capacity = static_cast<u32>(channel_capacity);
    state->header.capacity_frames = static_cast<u64>(capacity_frames);
    return adopt_ref(*new MediaElementAudioSourceProvider(move(state)));
}

NonnullRefPtr<MediaElementAudioSourceProvider> MediaElementAudioSourceProvider::create_for_remote_consumer(u64 provider_id, Render::RingStreamView view, Core::AnonymousBuffer shared_memory, int notify_read_fd)
{
    ASSERT_CONTROL_THREAD();
    TransportConsumerState state { .view = view, .shared_memory = move(shared_memory), .notify_read_fd = notify_read_fd };
    return adopt_ref(*new MediaElementAudioSourceProvider(provider_id, state));
}

MediaElementAudioSourceProvider::MediaElementAudioSourceProvider(LocalConsumerPtr state)
    : m_consumer(move(state))
{
    static Atomic<u64> s_next_provider_id { 1 };
    m_provider_id = s_next_provider_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);

    LocalConsumerState& local = *m_consumer.get<LocalConsumerPtr>();

    VERIFY(local.header.channel_capacity > 0);
    VERIFY(local.header.capacity_frames > 0);
    if (local.ring.is_empty()) {
        local.ring.resize(static_cast<size_t>(local.header.channel_capacity) * static_cast<size_t>(local.header.capacity_frames));
        local.ring.fill(0.0f);
    }

    local.header.version = Render::ring_stream_version;
    local.header.sample_rate_hz = 0;
    local.header.channel_count = 1;
    local.header.overrun_frames_total = 0;
    Render::ring_stream_store_read_frame(local.header, 0);
    Render::ring_stream_store_write_frame(local.header, 0);
    Render::ring_stream_clear_producer_timestamp_anchor(local.header);

    AK::atomic_store(&local.header.timeline_generation, static_cast<u64>(1), AK::MemoryOrder::memory_order_release);
    AK::atomic_store(&local.header.timeline_sample_rate, 0u, AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&local.header.timeline_media_start_frame, static_cast<u64>(0), AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&local.header.timeline_media_start_at_ring_frame, static_cast<u64>(0), AK::MemoryOrder::memory_order_relaxed);
}

MediaElementAudioSourceProvider::MediaElementAudioSourceProvider(u64 provider_id, TransportConsumerState state)
    : m_provider_id(provider_id)
    , m_consumer(move(state))
{
    TransportConsumerState& transport = m_consumer.get<TransportConsumerState>();
    VERIFY(transport.view.header);
    VERIFY(transport.view.header->channel_capacity > 0);
    VERIFY(transport.view.header->capacity_frames > 0);
    VERIFY(!transport.view.interleaved_frames.is_empty());
}

bool MediaElementAudioSourceProvider::wait_for_frames(size_t min_frames, int timeout_ms)
{
    ASSERT_RENDER_THREAD();
    if (min_frames == 0)
        return true;
    if (timeout_ms <= 0)
        return false;
    if (!m_consumer.has<TransportConsumerState>())
        return false;

    TransportConsumerState& transport = m_consumer.get<TransportConsumerState>();
    if (!transport.view.header)
        return false;
    int fd = transport.notify_read_fd;
    if (fd < 0)
        return false;

    auto peek_before = peek_with_timing();
    if (peek_before.available_frames >= min_frames)
        return true;

    pollfd pfd { .fd = fd, .events = POLLIN, .revents = 0 };
    int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc <= 0)
        return false;

    if (pfd.revents & POLLIN)
        Render::drain_stream_notify_fd(fd);

    auto peek_after = peek_with_timing();
    return peek_after.available_frames >= min_frames;
}

void MediaElementAudioSourceProvider::set_stream_transport_producer(Render::RingStreamView view, Render::StreamOverflowPolicy overflow_policy, int notify_write_fd)
{
    ASSERT_CONTROL_THREAD();
    if (!view.header)
        return;
    if (view.header->capacity_frames == 0 || view.header->channel_capacity == 0)
        return;
    if (view.interleaved_frames.is_empty())
        return;

    m_stream_transport_producer_view = view;
    m_stream_transport_overflow_policy = overflow_policy;
    m_stream_transport_notify_write_fd = notify_write_fd;

    if (!m_consumer.has<LocalConsumerPtr>())
        return;

    LocalConsumerState& local = *m_consumer.get<LocalConsumerPtr>();
    Render::RingStreamHeader& local_header = local.header;
    Render::RingStreamHeader& transport_header = *view.header;

    size_t const samples_to_copy = min(local.ring.size(), view.interleaved_frames.size());
    if (samples_to_copy > 0)
        __builtin_memcpy(view.interleaved_frames.data(), local.ring.data(), samples_to_copy * sizeof(float));

    transport_header.version = local_header.version;
    transport_header.sample_rate_hz = AK::atomic_load(&local_header.sample_rate_hz, AK::MemoryOrder::memory_order_relaxed);
    transport_header.channel_count = AK::atomic_load(&local_header.channel_count, AK::MemoryOrder::memory_order_relaxed);
    transport_header.channel_capacity = local_header.channel_capacity;
    transport_header.capacity_frames = local_header.capacity_frames;
    transport_header.overrun_frames_total = local_header.overrun_frames_total;

    u64 const local_read_frame = Render::ring_stream_load_read_frame(local_header);
    u64 const local_write_frame = Render::ring_stream_load_write_frame(local_header);
    Render::ring_stream_store_read_frame(transport_header, local_read_frame);
    Render::ring_stream_store_write_frame(transport_header, local_write_frame);

    Render::ring_stream_store_flags(transport_header, Render::ring_stream_load_flags(local_header));
    Render::ring_stream_store_producer_timestamp_anchor(transport_header, Render::ring_stream_load_producer_timestamp_anchor(local_header));

    AK::atomic_store(&transport_header.timeline_generation, AK::atomic_load(&local_header.timeline_generation, AK::MemoryOrder::memory_order_relaxed), AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&transport_header.timeline_sample_rate, AK::atomic_load(&local_header.timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed), AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&transport_header.timeline_media_start_frame, AK::atomic_load(&local_header.timeline_media_start_frame, AK::MemoryOrder::memory_order_relaxed), AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&transport_header.timeline_media_start_at_ring_frame, AK::atomic_load(&local_header.timeline_media_start_at_ring_frame, AK::MemoryOrder::memory_order_relaxed), AK::MemoryOrder::memory_order_relaxed);

    if (m_stream_transport_notify_write_fd >= 0)
        (void)Render::try_signal_stream_notify_fd(m_stream_transport_notify_write_fd);
}

void MediaElementAudioSourceProvider::clear_stream_transport_producer()
{
    ASSERT_CONTROL_THREAD();
    m_stream_transport_producer_view.clear();
    m_stream_transport_notify_write_fd = -1;
}

MediaElementAudioSourceProvider::PeekResult MediaElementAudioSourceProvider::peek_with_timing()
{
    ASSERT_RENDER_THREAD();
    if (m_consumer.has<TransportConsumerState>()) {
        TransportConsumerState& transport = m_consumer.get<TransportConsumerState>();
        auto& view = transport.view;
        PeekResult result;

        Render::RingStreamHeader& header = *view.header;
        result.end_of_stream = (Render::ring_stream_load_flags(header) & Render::ring_stream_flag_end_of_stream) != 0;
        result.timeline_generation = AK::atomic_load(&header.timeline_generation, AK::MemoryOrder::memory_order_acquire);

        u64 read_frame = Render::ring_stream_load_read_frame(header);
        u64 write_frame = Render::ring_stream_load_write_frame(header);

        (void)Render::ring_stream_consumer_detect_and_fix_overrun(header, read_frame, write_frame);

        result.available_frames = Render::ring_stream_available_frames(header, read_frame, write_frame);

        u32 const timeline_sample_rate = AK::atomic_load(&header.timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
        if (timeline_sample_rate != 0 && result.available_frames > 0) {
            u64 const timeline_media_start_frame = AK::atomic_load(&header.timeline_media_start_frame, AK::MemoryOrder::memory_order_relaxed);
            u64 const timeline_media_start_at_ring_frame = AK::atomic_load(&header.timeline_media_start_at_ring_frame, AK::MemoryOrder::memory_order_relaxed);
            u64 const media_frame_at_read_u64 = timeline_media_start_frame + (read_frame - timeline_media_start_at_ring_frame);
            i64 const media_frame_at_read = media_frame_at_read_u64 > static_cast<u64>(NumericLimits<i64>::max())
                ? NumericLimits<i64>::max()
                : static_cast<i64>(media_frame_at_read_u64);
            result.start_time = AK::Duration::from_time_units(media_frame_at_read, 1, timeline_sample_rate);
        }

        return result;
    }

    LocalConsumerState& local = *m_consumer.get<LocalConsumerPtr>();

    PeekResult result;

    Render::RingStreamView view { .header = &local.header, .interleaved_frames = { local.ring.data(), local.ring.size() } };
    Render::RingStreamConsumer consumer(view);
    Render::RingStreamPeekResult peek = consumer.peek_with_timing();
    result.timeline_generation = peek.timeline_generation;
    result.available_frames = peek.available_frames;
    result.start_time = peek.start_time;
    result.end_of_stream = (Render::ring_stream_load_flags(local.header) & Render::ring_stream_flag_end_of_stream) != 0;
    return result;
}

size_t MediaElementAudioSourceProvider::skip_frames(size_t requested_frames)
{
    ASSERT_RENDER_THREAD();
    if (m_consumer.has<TransportConsumerState>()) {
        if (requested_frames == 0)
            return 0;

        TransportConsumerState& transport = m_consumer.get<TransportConsumerState>();
        auto& view = transport.view;
        Render::RingStreamHeader& header = *view.header;

        u64 read_frame = Render::ring_stream_load_read_frame(header);
        u64 write_frame = Render::ring_stream_load_write_frame(header);

        (void)Render::ring_stream_consumer_detect_and_fix_overrun(header, read_frame, write_frame);

        size_t const available = Render::ring_stream_available_frames(header, read_frame, write_frame);
        size_t const frames_to_skip = min(available, requested_frames);
        if (frames_to_skip == 0)
            return 0;

        Render::ring_stream_store_read_frame(header, read_frame + frames_to_skip);
        m_total_frames_popped.fetch_add(frames_to_skip, AK::MemoryOrder::memory_order_relaxed);
        return frames_to_skip;
    }

    LocalConsumerState& local = *m_consumer.get<LocalConsumerPtr>();

    if (requested_frames == 0)
        return 0;

    Render::RingStreamView view { .header = &local.header, .interleaved_frames = { local.ring.data(), local.ring.size() } };
    Render::RingStreamConsumer consumer(view);
    size_t const frames_skipped = consumer.skip_frames(requested_frames);
    m_total_frames_popped.fetch_add(frames_skipped, AK::MemoryOrder::memory_order_relaxed);
    return frames_skipped;
}

void MediaElementAudioSourceProvider::push_interleaved(ReadonlySpan<float> interleaved_samples, u32 sample_rate, u32 channel_count)
{
    if (m_consumer.has<TransportConsumerState>())
        return;

    LocalConsumerState& local = *m_consumer.get<LocalConsumerPtr>();

    // No timing metadata provided.
    // Keep accepting audio for backwards compatibility, but do not attempt to maintain a media timeline.
    if (channel_count == 0 || sample_rate == 0)
        return;

    // Best-effort: clamp to our channel capacity. This avoids allocations on the audio callback thread.
    u32 const clamped_channels = min<u32>(channel_count, local.header.channel_capacity);
    size_t const input_frame_count = interleaved_samples.size() / channel_count;

    if (input_frame_count == 0)
        return;

    u32 const previous_sample_rate = AK::atomic_load(&local.header.sample_rate_hz, AK::MemoryOrder::memory_order_relaxed);
    u32 const previous_channel_count = AK::atomic_load(&local.header.channel_count, AK::MemoryOrder::memory_order_relaxed);

    AK::atomic_store(&local.header.sample_rate_hz, sample_rate, AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&local.header.channel_count, clamped_channels, AK::MemoryOrder::memory_order_relaxed);

    Render::RingStreamView local_view { .header = &local.header, .interleaved_frames = { local.ring.data(), local.ring.size() } };
    Render::RingStreamProducer local_producer(local_view, Render::StreamOverflowPolicy::DropOldest);
    size_t const frames_written = local_producer.try_push_interleaved({ interleaved_samples.data(), interleaved_samples.size() }, channel_count);

    if (frames_written > 0)
        Render::ring_stream_clear_flag(local.header, Render::ring_stream_flag_end_of_stream);

    m_total_frames_pushed.fetch_add(frames_written, AK::MemoryOrder::memory_order_relaxed);

    AK::atomic_store(&local.header.timeline_sample_rate, 0u, AK::MemoryOrder::memory_order_relaxed);

    if (m_stream_transport_producer_view.has_value()) {
        auto& view = *m_stream_transport_producer_view;
        Render::RingStreamHeader& header = *view.header;

        AK::atomic_store(&header.sample_rate_hz, sample_rate, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&header.channel_count, clamped_channels, AK::MemoryOrder::memory_order_relaxed);

        AK::atomic_store(&header.timeline_sample_rate, 0u, AK::MemoryOrder::memory_order_relaxed);

        Render::RingStreamProducer transport_producer(view, m_stream_transport_overflow_policy);
        size_t const transport_frames_written = transport_producer.try_push_interleaved({ interleaved_samples.data(), interleaved_samples.size() }, channel_count);
        if (transport_frames_written > 0)
            Render::ring_stream_clear_flag(header, Render::ring_stream_flag_end_of_stream);
        if (transport_frames_written > 0 && m_stream_transport_notify_write_fd >= 0)
            (void)Render::try_signal_stream_notify_fd(m_stream_transport_notify_write_fd);
    }

    if (should_log_media_element_bridge()) {
        static Atomic<i64> s_last_log_ms { 0 };
        i64 now_ms = AK::MonotonicTime::now().milliseconds();
        i64 last_ms = s_last_log_ms.load(AK::MemoryOrder::memory_order_relaxed);

        bool format_changed = previous_sample_rate != sample_rate || previous_channel_count != clamped_channels;
        bool should_log = format_changed || (now_ms - last_ms) >= 1000;
        if (should_log && s_last_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
            u64 const read_frame = Render::ring_stream_load_read_frame(local.header);
            u64 const write_frame = Render::ring_stream_load_write_frame(local.header);
            WA_MEDIA_DBGLN("[WebAudio] media-tap push: cid={} session={} provider={} frames={} sr={} ch_in={} ch_store={} read={} write={} total_pushed={}",
                debug_client_id(),
                debug_session_id(),
                m_provider_id,
                input_frame_count,
                sample_rate,
                channel_count,
                clamped_channels,
                read_frame,
                write_frame,
                m_total_frames_pushed.load(AK::MemoryOrder::memory_order_relaxed));
        }
    }
}

void MediaElementAudioSourceProvider::push_interleaved(ReadonlySpan<float> interleaved_samples, u32 sample_rate, u32 channel_count, AK::Duration media_time)
{
    if (m_consumer.has<TransportConsumerState>())
        return;

    LocalConsumerState& local = *m_consumer.get<LocalConsumerPtr>();

    if (channel_count == 0 || sample_rate == 0)
        return;

    // Best-effort: clamp to our channel capacity. This avoids allocations on the audio callback thread.
    u32 const clamped_channels = min<u32>(channel_count, local.header.channel_capacity);
    size_t const input_frame_count = interleaved_samples.size() / channel_count;
    if (input_frame_count == 0)
        return;

    u32 const previous_sample_rate = AK::atomic_load(&local.header.sample_rate_hz, AK::MemoryOrder::memory_order_relaxed);
    u32 const previous_channel_count = AK::atomic_load(&local.header.channel_count, AK::MemoryOrder::memory_order_relaxed);

    AK::atomic_store(&local.header.sample_rate_hz, sample_rate, AK::MemoryOrder::memory_order_relaxed);
    AK::atomic_store(&local.header.channel_count, clamped_channels, AK::MemoryOrder::memory_order_relaxed);

    u64 read_frame = Render::ring_stream_load_read_frame(local.header);
    u64 write_frame = Render::ring_stream_load_write_frame(local.header);
    bool const ring_was_empty = read_frame == write_frame;
    u64 const local_anchor_ring_frame = write_frame;

    u64 const start_media_frame = media_time.to_time_units(1, sample_rate);
    u32 const timeline_sample_rate = AK::atomic_load(&local.header.timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);

    bool const format_changed = previous_sample_rate != sample_rate || previous_channel_count != clamped_channels;

    auto reset_timeline = [&] {
        Render::ring_stream_store_read_frame(local.header, write_frame);
        Render::ring_stream_store_write_frame(local.header, write_frame);

        AK::atomic_store(&local.header.timeline_media_start_frame, start_media_frame, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&local.header.timeline_media_start_at_ring_frame, write_frame, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&local.header.timeline_sample_rate, sample_rate, AK::MemoryOrder::memory_order_relaxed);
        (void)AK::atomic_fetch_add(&local.header.timeline_generation, static_cast<u64>(1), AK::MemoryOrder::memory_order_release);
    };

    auto update_timeline_mapping = [&](u64 start_media_frame, u64 ring_frame) {
        AK::atomic_store(&local.header.timeline_media_start_frame, start_media_frame, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&local.header.timeline_media_start_at_ring_frame, ring_frame, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&local.header.timeline_sample_rate, sample_rate, AK::MemoryOrder::memory_order_relaxed);
    };

    auto reset_stream_transport_timeline = [&](Render::RingStreamHeader& header, u64 start_media_frame, u64 stream_write_frame) {
        // Clear any buffered data so the consumer observes a clean discontinuity.
        Render::ring_stream_store_read_frame(header, stream_write_frame);
        Render::ring_stream_store_write_frame(header, stream_write_frame);

        AK::atomic_store(&header.timeline_media_start_frame, start_media_frame, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&header.timeline_media_start_at_ring_frame, stream_write_frame, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&header.timeline_sample_rate, sample_rate, AK::MemoryOrder::memory_order_relaxed);
        (void)AK::atomic_fetch_add(&header.timeline_generation, static_cast<u64>(1), AK::MemoryOrder::memory_order_release);
    };

    auto update_stream_transport_timeline_mapping = [&](Render::RingStreamHeader& header, u64 start_media_frame, u64 stream_write_frame) {
        AK::atomic_store(&header.timeline_media_start_frame, start_media_frame, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&header.timeline_media_start_at_ring_frame, stream_write_frame, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&header.timeline_sample_rate, sample_rate, AK::MemoryOrder::memory_order_relaxed);
    };

    if (format_changed || timeline_sample_rate != sample_rate) {
        reset_timeline();
        read_frame = write_frame;

        if (m_stream_transport_producer_view.has_value()) {
            auto& view = *m_stream_transport_producer_view;
            Render::RingStreamHeader& header = *view.header;
            u64 stream_write_frame = Render::ring_stream_load_write_frame(header);
            reset_stream_transport_timeline(header, start_media_frame, stream_write_frame);
        }
    } else if (ring_was_empty) {
        // Establish an anchor when transitioning from empty -> non-empty.
        // Timestamp jitter while buffered audio exists should not rewrite the timeline.
        update_timeline_mapping(start_media_frame, write_frame);

        if (m_stream_transport_producer_view.has_value()) {
            auto& view = *m_stream_transport_producer_view;
            Render::RingStreamHeader& header = *view.header;
            u64 const stream_read_frame = Render::ring_stream_load_read_frame(header);
            u64 const stream_write_frame = Render::ring_stream_load_write_frame(header);
            if (stream_read_frame == stream_write_frame)
                update_stream_transport_timeline_mapping(header, start_media_frame, stream_write_frame);
        }
    }

    Render::RingStreamView local_view { .header = &local.header, .interleaved_frames = { local.ring.data(), local.ring.size() } };
    Render::RingStreamProducer local_producer(local_view, Render::StreamOverflowPolicy::DropOldest);
    size_t const frames_written = local_producer.try_push_interleaved({ interleaved_samples.data(), interleaved_samples.size() }, channel_count);

    if (should_log_media_element_bridge() && ring_was_empty && frames_written > 0) {
        i64 now_ms = AK::MonotonicTime::now().milliseconds();
        i64 last_ms = m_last_refill_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
        if ((now_ms - last_ms) >= 250 && m_last_refill_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
            u64 const generation = AK::atomic_load(&local.header.timeline_generation, AK::MemoryOrder::memory_order_relaxed);
            u64 const new_write_frame = Render::ring_stream_load_write_frame(local.header);
            WA_MEDIA_DBGLN("[WebAudio] media-tap refill: cid={} session={} provider={} frames={} sr={} ch_in={} ch_store={} media_time_ms={} read={} write_before={} write_after={} gen={}",
                debug_client_id(),
                debug_session_id(),
                m_provider_id,
                frames_written,
                sample_rate,
                channel_count,
                clamped_channels,
                media_time.to_milliseconds(),
                read_frame,
                local_anchor_ring_frame,
                new_write_frame,
                generation);
        }
    }

    if (frames_written > 0)
        Render::ring_stream_clear_flag(local.header, Render::ring_stream_flag_end_of_stream);

    if (frames_written > 0) {
        u64 const generation = AK::atomic_load(&local.header.timeline_generation, AK::MemoryOrder::memory_order_relaxed);
        Render::ring_stream_store_producer_timestamp_anchor(local.header, {
                                                                              .generation = generation,
                                                                              .media_start_frame = start_media_frame,
                                                                              .media_start_at_ring_frame = local_anchor_ring_frame,
                                                                          });
    }

    m_total_frames_pushed.fetch_add(frames_written, AK::MemoryOrder::memory_order_relaxed);

    if (m_stream_transport_producer_view.has_value()) {
        auto& view = *m_stream_transport_producer_view;
        Render::RingStreamHeader& header = *view.header;

        u64 const transport_anchor_ring_frame = Render::ring_stream_load_write_frame(header);

        AK::atomic_store(&header.sample_rate_hz, sample_rate, AK::MemoryOrder::memory_order_relaxed);
        AK::atomic_store(&header.channel_count, clamped_channels, AK::MemoryOrder::memory_order_relaxed);

        Render::RingStreamProducer transport_producer(view, m_stream_transport_overflow_policy);
        size_t const transport_frames_written = transport_producer.try_push_interleaved({ interleaved_samples.data(), interleaved_samples.size() }, channel_count);
        if (transport_frames_written > 0)
            Render::ring_stream_clear_flag(header, Render::ring_stream_flag_end_of_stream);

        if (transport_frames_written > 0) {
            u64 const generation = AK::atomic_load(&header.timeline_generation, AK::MemoryOrder::memory_order_relaxed);
            Render::ring_stream_store_producer_timestamp_anchor(header, {
                                                                            .generation = generation,
                                                                            .media_start_frame = start_media_frame,
                                                                            .media_start_at_ring_frame = transport_anchor_ring_frame,
                                                                        });
        }
        if (transport_frames_written > 0 && m_stream_transport_notify_write_fd >= 0)
            (void)Render::try_signal_stream_notify_fd(m_stream_transport_notify_write_fd);
    }

    if (should_log_media_element_bridge()) {
        static Atomic<i64> s_last_log_ms { 0 };
        i64 now_ms = AK::MonotonicTime::now().milliseconds();
        i64 last_ms = s_last_log_ms.load(AK::MemoryOrder::memory_order_relaxed);

        bool should_log = format_changed || (now_ms - last_ms) >= 1000;
        if (should_log && s_last_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
            WA_MEDIA_DBGLN("[WebAudio] media-tap push: cid={} session={} provider={} frames={} sr={} ch_in={} ch_store={} media_time_ms={} read={} write={} gen={} total_pushed={}",
                debug_client_id(),
                debug_session_id(),
                m_provider_id,
                input_frame_count,
                sample_rate,
                channel_count,
                clamped_channels,
                media_time.to_milliseconds(),
                read_frame,
                Render::ring_stream_load_write_frame(local.header),
                AK::atomic_load(&local.header.timeline_generation, AK::MemoryOrder::memory_order_relaxed),
                m_total_frames_pushed.load(AK::MemoryOrder::memory_order_relaxed));
        }
    }
}

MediaElementAudioSourceProvider::PopResult MediaElementAudioSourceProvider::pop_planar_with_timing(Span<Span<f32>> out_channels, size_t requested_frames, u32 expected_channel_count)
{
    ASSERT_RENDER_THREAD();
    PopResult result;

    if (m_consumer.has<TransportConsumerState>()) {
        if (requested_frames == 0 || expected_channel_count == 0)
            return result;
        if (out_channels.size() < expected_channel_count)
            return result;
        for (u32 ch = 0; ch < expected_channel_count; ++ch) {
            if (out_channels[ch].size() < requested_frames)
                return result;
        }

        TransportConsumerState& transport = m_consumer.get<TransportConsumerState>();
        auto& view = transport.view;
        Render::RingStreamHeader& header = *view.header;

        result.end_of_stream = (Render::ring_stream_load_flags(header) & Render::ring_stream_flag_end_of_stream) != 0;

        result.timeline_generation = AK::atomic_load(&header.timeline_generation, AK::MemoryOrder::memory_order_acquire);

        u64 read_frame = Render::ring_stream_load_read_frame(header);
        u64 write_frame = Render::ring_stream_load_write_frame(header);

        (void)Render::ring_stream_consumer_detect_and_fix_overrun(header, read_frame, write_frame);

        size_t const available = Render::ring_stream_available_frames(header, read_frame, write_frame);
        size_t const frames_to_read = min(available, requested_frames);
        if (frames_to_read == 0) {
            if (should_log_media_element_bridge()) {
                i64 now_ms = AK::MonotonicTime::now().milliseconds();
                i64 last_ms = m_last_empty_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
                if ((now_ms - last_ms) >= 250 && m_last_empty_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
                    u64 const flags = Render::ring_stream_load_flags(header);
                    u32 const timeline_sample_rate = AK::atomic_load(&header.timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
                    Render::RingStreamProducerTimestampAnchor const anchor = Render::ring_stream_load_producer_timestamp_anchor(header);
                    WA_MEDIA_DBGLN("[WebAudio] media-tap empty (remote): cid={} session={} provider={} want={} avail={} read={} write={} gen={} eos={} flags={} header_sr={} header_ch={} timeline_sr={} anchor_gen={} anchor_media={} anchor_ring={}",
                        debug_client_id(),
                        debug_session_id(),
                        m_provider_id,
                        requested_frames,
                        available,
                        read_frame,
                        write_frame,
                        result.timeline_generation,
                        result.end_of_stream,
                        flags,
                        header.sample_rate_hz,
                        header.channel_count,
                        timeline_sample_rate,
                        anchor.generation,
                        anchor.media_start_frame,
                        anchor.media_start_at_ring_frame);
                }
            }
            return result;
        }

        if (should_log_media_element_bridge() && frames_to_read < requested_frames) {
            i64 now_ms = AK::MonotonicTime::now().milliseconds();
            i64 last_ms = m_last_short_read_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
            if ((now_ms - last_ms) >= 250 && m_last_short_read_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
                u64 const flags = Render::ring_stream_load_flags(header);
                u32 const timeline_sample_rate = AK::atomic_load(&header.timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
                Render::RingStreamProducerTimestampAnchor const anchor = Render::ring_stream_load_producer_timestamp_anchor(header);
                WA_MEDIA_DBGLN("[WebAudio] media-tap short-read (remote): cid={} session={} provider={} need={} got={} avail={} read={} write={} gen={} eos={} flags={} header_sr={} header_ch={} timeline_sr={} anchor_gen={} anchor_media={} anchor_ring={}",
                    debug_client_id(),
                    debug_session_id(),
                    m_provider_id,
                    requested_frames,
                    frames_to_read,
                    available,
                    read_frame,
                    write_frame,
                    result.timeline_generation,
                    result.end_of_stream,
                    flags,
                    header.sample_rate_hz,
                    header.channel_count,
                    timeline_sample_rate,
                    anchor.generation,
                    anchor.media_start_frame,
                    anchor.media_start_at_ring_frame);
            }
        }

        u32 const timeline_sample_rate = AK::atomic_load(&header.timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
        if (timeline_sample_rate != 0) {
            u64 const timeline_media_start_frame = AK::atomic_load(&header.timeline_media_start_frame, AK::MemoryOrder::memory_order_relaxed);
            u64 const timeline_media_start_at_ring_frame = AK::atomic_load(&header.timeline_media_start_at_ring_frame, AK::MemoryOrder::memory_order_relaxed);
            u64 const media_frame_at_read_u64 = timeline_media_start_frame + (read_frame - timeline_media_start_at_ring_frame);
            i64 const media_frame_at_read = media_frame_at_read_u64 > static_cast<u64>(NumericLimits<i64>::max())
                ? NumericLimits<i64>::max()
                : static_cast<i64>(media_frame_at_read_u64);
            result.start_time = AK::Duration::from_time_units(media_frame_at_read, 1, timeline_sample_rate);
        }

        (void)Render::ring_stream_pop_planar_from_read_frame(view, read_frame, frames_to_read, out_channels, expected_channel_count);
        m_total_frames_popped.fetch_add(frames_to_read, AK::MemoryOrder::memory_order_relaxed);
        result.frames_read = frames_to_read;
        return result;
    }

    LocalConsumerState& local = *m_consumer.get<LocalConsumerPtr>();

    if (requested_frames == 0 || expected_channel_count == 0)
        return result;

    if (out_channels.size() < expected_channel_count)
        return result;
    for (u32 ch = 0; ch < expected_channel_count; ++ch) {
        if (out_channels[ch].size() < requested_frames)
            return result;
    }

    Render::RingStreamView view { .header = &local.header, .interleaved_frames = { local.ring.data(), local.ring.size() } };
    Render::RingStreamHeader& header = local.header;

    result.end_of_stream = (Render::ring_stream_load_flags(header) & Render::ring_stream_flag_end_of_stream) != 0;

    result.timeline_generation = AK::atomic_load(&header.timeline_generation, AK::MemoryOrder::memory_order_acquire);

    u64 read_frame = Render::ring_stream_load_read_frame(header);
    u64 write_frame = Render::ring_stream_load_write_frame(header);

    (void)Render::ring_stream_consumer_detect_and_fix_overrun(header, read_frame, write_frame);

    size_t const available = Render::ring_stream_available_frames(header, read_frame, write_frame);
    size_t const frames_to_read = min(available, requested_frames);
    if (frames_to_read == 0) {
        if (should_log_media_element_bridge()) {
            i64 now_ms = AK::MonotonicTime::now().milliseconds();
            i64 last_ms = m_last_empty_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
            if ((now_ms - last_ms) >= 250 && m_last_empty_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
                u64 const flags = Render::ring_stream_load_flags(header);
                u32 const timeline_sample_rate = AK::atomic_load(&header.timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
                Render::RingStreamProducerTimestampAnchor const anchor = Render::ring_stream_load_producer_timestamp_anchor(header);
                WA_MEDIA_DBGLN("[WebAudio] media-tap empty (local): cid={} session={} provider={} want={} avail={} read={} write={} gen={} eos={} flags={} timeline_sr={} anchor_gen={} anchor_media={} anchor_ring={}",
                    debug_client_id(),
                    debug_session_id(),
                    m_provider_id,
                    requested_frames,
                    available,
                    read_frame,
                    write_frame,
                    result.timeline_generation,
                    result.end_of_stream,
                    flags,
                    timeline_sample_rate,
                    anchor.generation,
                    anchor.media_start_frame,
                    anchor.media_start_at_ring_frame);
            }
        }
        return result;
    }

    if (should_log_media_element_bridge() && frames_to_read < requested_frames) {
        i64 now_ms = AK::MonotonicTime::now().milliseconds();
        i64 last_ms = m_last_short_read_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
        if ((now_ms - last_ms) >= 250 && m_last_short_read_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed)) {
            u64 const flags = Render::ring_stream_load_flags(header);
            u32 const timeline_sample_rate = AK::atomic_load(&header.timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
            Render::RingStreamProducerTimestampAnchor const anchor = Render::ring_stream_load_producer_timestamp_anchor(header);
            WA_MEDIA_DBGLN("[WebAudio] media-tap short-read (local): cid={} session={} provider={} need={} got={} avail={} read={} write={} gen={} eos={} flags={} timeline_sr={} anchor_gen={} anchor_media={} anchor_ring={}",
                debug_client_id(),
                debug_session_id(),
                m_provider_id,
                requested_frames,
                frames_to_read,
                available,
                read_frame,
                write_frame,
                result.timeline_generation,
                result.end_of_stream,
                flags,
                timeline_sample_rate,
                anchor.generation,
                anchor.media_start_frame,
                anchor.media_start_at_ring_frame);
        }
    }

    u32 const timeline_sample_rate = AK::atomic_load(&header.timeline_sample_rate, AK::MemoryOrder::memory_order_relaxed);
    if (timeline_sample_rate != 0) {
        u64 const timeline_media_start_frame = AK::atomic_load(&header.timeline_media_start_frame, AK::MemoryOrder::memory_order_relaxed);
        u64 const timeline_media_start_at_ring_frame = AK::atomic_load(&header.timeline_media_start_at_ring_frame, AK::MemoryOrder::memory_order_relaxed);
        u64 const media_frame_at_read_u64 = timeline_media_start_frame + (read_frame - timeline_media_start_at_ring_frame);
        i64 const media_frame_at_read = media_frame_at_read_u64 > static_cast<u64>(NumericLimits<i64>::max())
            ? NumericLimits<i64>::max()
            : static_cast<i64>(media_frame_at_read_u64);
        result.start_time = AK::Duration::from_time_units(media_frame_at_read, 1, timeline_sample_rate);
    }

    (void)Render::ring_stream_pop_planar_from_read_frame(view, read_frame, frames_to_read, out_channels, expected_channel_count);
    m_total_frames_popped.fetch_add(frames_to_read, AK::MemoryOrder::memory_order_relaxed);
    result.frames_read = frames_to_read;
    return result;
}

size_t MediaElementAudioSourceProvider::pop_planar(Span<Span<f32>> out_channels, size_t requested_frames, u32 expected_channel_count)
{
    ASSERT_RENDER_THREAD();
    return pop_planar_with_timing(out_channels, requested_frames, expected_channel_count).frames_read;
}

}
