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
#include <AK/Vector.h>
#include <LibCore/SharedSingleProducerCircularBuffer.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibThreading/Mutex.h>

namespace Core {

class ThreadEventQueue;

}

namespace AudioServer {

class AudioOutputDevice {
public:
    static AudioOutputDevice& the();

    // Must be called from the AudioServer control thread.
    void ensure_started(Core::ThreadEventQueue& control_event_queue, u32 target_latency_ms);

    bool has_sample_specification() const;
    u32 device_sample_rate_hz() const;
    u32 device_channel_count() const;

    // Called on the AudioServer control thread.
    void when_ready(Function<void()>);

    // Called on the AudioServer control thread.
    void register_producer(u64 producer_id, Core::SharedSingleProducerCircularBuffer ring, size_t bytes_per_frame);
    void unregister_producer(u64 producer_id);

    // Called on the AudioServer control thread.
    void set_producer_muted(u64 producer_id, bool muted);

private:
    AudioOutputDevice() = default;
    ~AudioOutputDevice();

    struct Producer {
        Core::SharedSingleProducerCircularBuffer ring;
        size_t bytes_per_frame { 0 };
        bool muted { false };
    };

    struct ProducerSnapshot : public AtomicRefCounted<ProducerSnapshot> {
        Vector<Producer> producers;
    };

    void notify_ready();
    void update_producer_snapshot();

    Threading::Mutex m_mutex;
    Threading::Mutex m_start_mutex;
    Core::ThreadEventQueue* m_control_event_queue { nullptr };

    RefPtr<Audio::PlaybackStream> m_stream;

    Atomic<ProducerSnapshot*> m_producer_snapshot { nullptr };

    HashMap<u64, Producer> m_producers;
    Vector<Function<void()>> m_when_ready;

    Atomic<bool> m_has_sample_specification { false };
    Atomic<u32> m_device_sample_rate_hz { 0 };
    Atomic<u32> m_device_channel_count { 0 };
};

}
