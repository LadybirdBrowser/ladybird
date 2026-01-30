/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Engine/StreamTransport.h>

namespace Web::WebAudio::Render {

class RingStreamConsumer;
class RingStreamProducer;

}

namespace Web::WebAudio {

// A simple SPSC ring buffer for interleaved f32 frames
class WEB_API MediaElementAudioSourceProvider final : public AtomicRefCounted<MediaElementAudioSourceProvider> {
public:
    static NonnullRefPtr<MediaElementAudioSourceProvider> create(size_t channel_capacity, size_t capacity_frames);
    static NonnullRefPtr<MediaElementAudioSourceProvider> create_for_remote_consumer(u64 provider_id, Render::RingStreamView view, Core::AnonymousBuffer shared_memory, int notify_read_fd = -1);

    u64 provider_id() const { return m_provider_id; }

    // Optional debug context so AudioServer logs can be correlated back to a particular client
    // connection and WebAudio session.
    void set_debug_connection_info(int client_id, u64 session_id);
    int debug_client_id() const { return m_debug_client_id; }
    u64 debug_session_id() const { return m_debug_session_id; }

    // Attach a shared RingStream producer. When attached, push_interleaved() also writes into
    // the shared ring and signals notify_write_fd (eventfd or pipe) after successful writes.
    void set_stream_transport_producer(Render::RingStreamView view, Render::StreamOverflowPolicy overflow_policy, int notify_write_fd);
    void clear_stream_transport_producer();

    void set_target_sample_rate(u32 sample_rate) { m_target_sample_rate = sample_rate; }
    Optional<u32> target_sample_rate() const { return m_target_sample_rate; }

    // Declare a hard discontinuity (seek/flush). Clears buffered audio, invalidates the timeline
    // until the next timestamped push, and increments timeline_generation.
    void declare_discontinuity();

    // Declare end-of-stream. Consumers can distinguish true EOS from a transient underrun.
    void declare_end_of_stream();

    void push_interleaved(ReadonlySpan<float> interleaved_samples, u32 sample_rate, u32 channel_count);
    void push_interleaved(ReadonlySpan<float> interleaved_samples, u32 sample_rate, u32 channel_count, AK::Duration media_time);

    struct PeekResult {
        size_t available_frames { 0 };
        Optional<AK::Duration> start_time;
        u64 timeline_generation { 0 };
        bool end_of_stream { false };
    };

    PeekResult peek_with_timing();

    // Best-effort bounded wait for producer progress.
    // Only meaningful for remote (shared-memory) consumers where a notify fd is available.
    bool wait_for_frames(size_t min_frames, int timeout_ms);

    // Advances the read cursor without copying any samples. Returns the number of frames skipped.
    size_t skip_frames(size_t requested_frames);

    struct PopResult {
        size_t frames_read { 0 };
        Optional<AK::Duration> start_time;
        u64 timeline_generation { 0 };
        bool end_of_stream { false };
    };

    PopResult pop_planar_with_timing(Span<Span<f32>> out_channels, size_t requested_frames, u32 expected_channel_count);

    // Pops up to `requested_frames` frames into planar output channels.
    // - `out_channels.size()` must be >= expected_channel_count
    // - each out channel span must have size >= requested_frames
    // Returns the number of frames actually popped.
    size_t pop_planar(Span<Span<f32>> out_channels, size_t requested_frames, u32 expected_channel_count);

    u32 sample_rate() const
    {
        return m_consumer.visit(
            [&](LocalConsumerPtr const& local) -> u32 {
                return AK::atomic_load(&local->header.sample_rate_hz, AK::MemoryOrder::memory_order_relaxed);
            },
            [&](TransportConsumerState const& transport) -> u32 {
                if (!transport.view.header)
                    return 0;
                return AK::atomic_load(&transport.view.header->sample_rate_hz, AK::MemoryOrder::memory_order_relaxed);
            });
    }

    u32 channel_count() const
    {
        return m_consumer.visit(
            [&](LocalConsumerPtr const& local) -> u32 {
                return AK::atomic_load(&local->header.channel_count, AK::MemoryOrder::memory_order_relaxed);
            },
            [&](TransportConsumerState const& transport) -> u32 {
                if (!transport.view.header)
                    return 0;
                return AK::atomic_load(&transport.view.header->channel_count, AK::MemoryOrder::memory_order_relaxed);
            });
    }

    u64 debug_total_frames_pushed() const { return m_total_frames_pushed.load(AK::MemoryOrder::memory_order_relaxed); }
    u64 debug_total_frames_popped() const { return m_total_frames_popped.load(AK::MemoryOrder::memory_order_relaxed); }
    u64 debug_read_frame() const
    {
        return m_consumer.visit(
            [&](LocalConsumerPtr const& local) -> u64 {
                return Render::ring_stream_load_read_frame(local->header);
            },
            [&](TransportConsumerState const& transport) -> u64 {
                if (!transport.view.header)
                    return 0;
                return Render::ring_stream_load_read_frame(*transport.view.header);
            });
    }

    u64 debug_write_frame() const
    {
        return m_consumer.visit(
            [&](LocalConsumerPtr const& local) -> u64 {
                return Render::ring_stream_load_write_frame(local->header);
            },
            [&](TransportConsumerState const& transport) -> u64 {
                if (!transport.view.header)
                    return 0;
                return Render::ring_stream_load_write_frame(*transport.view.header);
            });
    }

    size_t channel_capacity() const
    {
        return m_consumer.visit(
            [&](LocalConsumerPtr const& local) -> size_t {
                return static_cast<size_t>(local->header.channel_capacity);
            },
            [&](TransportConsumerState const& transport) -> size_t {
                if (!transport.view.header)
                    return 0;
                return static_cast<size_t>(transport.view.header->channel_capacity);
            });
    }

    size_t capacity_frames() const
    {
        return m_consumer.visit(
            [&](LocalConsumerPtr const& local) -> size_t {
                return static_cast<size_t>(local->header.capacity_frames);
            },
            [&](TransportConsumerState const& transport) -> size_t {
                if (!transport.view.header)
                    return 0;
                return static_cast<size_t>(transport.view.header->capacity_frames);
            });
    }

    ~MediaElementAudioSourceProvider();

private:
    struct LocalConsumerState {
        Render::RingStreamHeader header {};
        Vector<float> ring;
    };

    struct TransportConsumerState {
        Render::RingStreamView view;
        Core::AnonymousBuffer shared_memory;
        int notify_read_fd { -1 };
    };

    using LocalConsumerPtr = NonnullOwnPtr<LocalConsumerState>;

    MediaElementAudioSourceProvider(LocalConsumerPtr state);
    MediaElementAudioSourceProvider(u64 provider_id, TransportConsumerState state);

    u64 m_provider_id { 0 };

    // Consumer backing selection:
    // - LocalConsumerState: in-memory SPSC ring (single-process)
    // - TransportConsumerState: shared-memory RingStream (cross-process)
    AK::Variant<LocalConsumerPtr, TransportConsumerState> m_consumer;

    Atomic<u64> m_total_frames_pushed { 0 };
    Atomic<u64> m_total_frames_popped { 0 };

    // Throttle consumer-side underrun/short-read logging.
    Atomic<i64> m_last_short_read_log_ms { 0 };

    // Throttle additional state-transition logging.
    Atomic<i64> m_last_empty_log_ms { 0 };
    Atomic<i64> m_last_refill_log_ms { 0 };
    Atomic<i64> m_last_eos_log_ms { 0 };
    Atomic<i64> m_last_discontinuity_log_ms { 0 };

    Atomic<bool> m_has_debug_connection_info { false };
    Optional<int> m_debug_client_pid;
    int m_debug_client_id { -1 };
    u64 m_debug_session_id { 0 };

    // Optional shared stream transport.
    Optional<Render::RingStreamView> m_stream_transport_producer_view;
    Render::StreamOverflowPolicy m_stream_transport_overflow_policy { Render::StreamOverflowPolicy::DropOldest };
    int m_stream_transport_notify_write_fd { -1 };

    Optional<u32> m_target_sample_rate;
};

}
