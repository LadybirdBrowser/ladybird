/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibAudioServer/LibAudioServer.h>
#include <LibAudioServer/SharedCircularBuffer.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ThreadedPromise.h>
#include <LibThreading/Mutex.h>

namespace Core {

class ThreadEventQueue;

}

namespace AudioServer {

class OutputDriver;
struct TimingInfo;

class OutputStream {
public:
    explicit OutputStream(DeviceHandle device_handle)
        : m_device_handle(device_handle)
    {
    }
    ~OutputStream();

    // Must be called from the AudioServer control thread.
    void ensure_started(Core::ThreadEventQueue& control_event_queue, u32 target_latency_ms);

    void set_underrun_callback(Function<void()>);
    NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> resume();
    NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend();
    NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend();
    AK::Duration device_time_played() const;
    NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double volume);

    static ErrorOr<Core::AnonymousBuffer> create_timing_buffer();

    // Called on the AudioServer control thread.
    void when_ready(Function<void()>);

    // Called on the AudioServer control thread.
    void register_producer(u64 producer_id, AudioServer::SharedCircularBuffer ring, Core::AnonymousBuffer timing_buffer, size_t bytes_per_frame);
    void unregister_producer(u64 producer_id);

    // Called on the AudioServer control thread.
    void set_producer_muted(u64 producer_id, bool muted);

private:
    struct OutputDriverDeleter {
        void operator()(OutputDriver*);
    };

    struct Producer {
        AudioServer::SharedCircularBuffer ring;
        Core::AnonymousBuffer timing_buffer;
        u64 device_played_frame_base { 0 };
        size_t bytes_per_frame { 0 };
        bool muted { false };
    };

    struct ProducerSnapshot : public AtomicRefCounted<ProducerSnapshot> {
        Vector<Producer> producers;
    };

    void notify_ready();
    void update_producer_snapshot();
    u64 current_device_played_frames() const;
    static TimingInfo* timing_storage_from_buffer(Core::AnonymousBuffer& timing_buffer);
    static void publish_timing(TimingInfo& storage, u64 device_played_frames, u64 server_monotonic_ns, u64 additional_ring_read_frames, u64 additional_underruns);

    Threading::Mutex m_mutex;
    Threading::Mutex m_start_mutex;
    Core::ThreadEventQueue* m_control_event_queue { nullptr };
    DeviceHandle m_device_handle { 0 };

    OwnPtr<OutputDriver, OutputDriverDeleter> m_stream;

    Atomic<ProducerSnapshot*> m_producer_snapshot { nullptr };

    HashMap<u64, Producer> m_producers;
    Vector<Function<void()>> m_when_ready;

    Atomic<u32> m_device_sample_rate_hz { 0 };
    Atomic<u32> m_device_channel_count { 0 };
    AK::Duration m_last_debug_log_time { AK::Duration::zero() };
    Vector<float> m_scratch;
};

}
