/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AudioServer/AudioOutputDevice.h>
#include <AudioServer/AudioServerConnection.h>
#include <AudioServer/Debug.h>
#include <LibCore/Event.h>
#include <LibCore/ThreadEventQueue.h>

namespace AudioServer {

AudioOutputDevice::~AudioOutputDevice()
{
    ProducerSnapshot* snapshot = m_producer_snapshot.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    if (snapshot)
        snapshot->unref();
}

AudioOutputDevice& AudioOutputDevice::the()
{
    static AudioOutputDevice device;
    return device;
}

void AudioOutputDevice::ensure_started(Core::ThreadEventQueue& control_event_queue, u32 target_latency_ms)
{
    Threading::MutexLocker start_locker(m_start_mutex);
    if (m_stream)
        return;

    if (should_log_audio_server())
        dbgln("AudioServer: starting output device (target_latency_ms={})", target_latency_ms);

    m_control_event_queue = &control_event_queue;
    Core::ThreadEventQueue* control_event_queue_ptr = &control_event_queue;

    // Start in Playing state so the backend selects a device format and calls
    // SampleSpecificationCallback promptly. We output silence until producers start writing.
    auto initial_output_state = Audio::OutputState::Playing;

    auto sample_specification_callback = [this, control_event_queue_ptr](Audio::SampleSpecification spec) {
        m_device_sample_rate_hz.store(spec.sample_rate(), AK::MemoryOrder::memory_order_release);
        m_device_channel_count.store(spec.channel_count(), AK::MemoryOrder::memory_order_release);
        m_has_sample_specification.store(true, AK::MemoryOrder::memory_order_release);

        // This callback may run on a backend thread (e.g. PulseAudio). Post to the AudioServer control thread.
        if (control_event_queue_ptr)
            control_event_queue_ptr->deferred_invoke([this] { notify_ready(); });
    };

    auto audio_data_request_callback = [this](Span<float> buffer) {
        buffer.fill(0.0f);

        u32 output_channel_count = m_device_channel_count.load(AK::MemoryOrder::memory_order_acquire);
        if (output_channel_count == 0)
            return ReadonlySpan<float> { buffer };

        size_t output_channel_count_sz = static_cast<size_t>(output_channel_count);
        size_t aligned_sample_count = (buffer.size() / output_channel_count_sz) * output_channel_count_sz;
        if (aligned_sample_count == 0)
            return ReadonlySpan<float> { buffer };

        Span<float> aligned_buffer = buffer.slice(0, aligned_sample_count);

        static thread_local AK::Duration last_debug_log_time = AK::Duration::zero();

        ProducerSnapshot* snapshot = m_producer_snapshot.load(AK::MemoryOrder::memory_order_acquire);
        if (!snapshot)
            return ReadonlySpan<float> { buffer };

        snapshot->ref();

        auto out_bytes = Bytes { reinterpret_cast<u8*>(aligned_buffer.data()), aligned_buffer.size() * sizeof(float) };

        bool have_written_anything = false;
        size_t producers_with_data = 0;
        size_t total_bytes_read = 0;

        // Scratch space for mixing additional producer buffers.
        static thread_local Vector<float> scratch;
        if (scratch.size() < buffer.size())
            scratch.resize(buffer.size());
        Span<float> scratch_span { scratch.data(), buffer.size() };
        auto scratch_bytes = Bytes { reinterpret_cast<u8*>(scratch_span.data()), scratch_span.size() * sizeof(float) };

        for (auto& producer : snapshot->producers) {
            if (producer.bytes_per_frame == 0)
                continue;

            if (producer.muted) {
                size_t bytes_read = producer.ring.try_read(scratch_bytes);
                if (bytes_read % producer.bytes_per_frame != 0) {
                    if (should_log_audio_server())
                        warnln("AudioServer: muted producer ring returned misaligned read: bytes_read={} bytes_per_frame={}", bytes_read, producer.bytes_per_frame);
                }
                if (bytes_read > 0)
                    producers_with_data++;
                total_bytes_read += bytes_read;
                continue;
            }

            if (!have_written_anything) {
                size_t bytes_read = producer.ring.try_read(out_bytes);
                size_t aligned_bytes_read = (bytes_read / producer.bytes_per_frame) * producer.bytes_per_frame;
                if (bytes_read != aligned_bytes_read) {
                    if (should_log_audio_server())
                        warnln("AudioServer: producer ring returned misaligned read: bytes_read={} bytes_per_frame={} (dropping tail)", bytes_read, producer.bytes_per_frame);
                    bytes_read = aligned_bytes_read;
                }
                if (bytes_read > 0)
                    producers_with_data++;
                total_bytes_read += bytes_read;
                if (bytes_read < out_bytes.size())
                    out_bytes.slice(bytes_read).fill(0);
                have_written_anything = true;
                continue;
            }

            size_t bytes_read = producer.ring.try_read(scratch_bytes);
            size_t aligned_bytes_read = (bytes_read / producer.bytes_per_frame) * producer.bytes_per_frame;
            if (bytes_read != aligned_bytes_read) {
                if (should_log_audio_server())
                    warnln("AudioServer: producer ring returned misaligned read: bytes_read={} bytes_per_frame={} (dropping tail)", bytes_read, producer.bytes_per_frame);
                bytes_read = aligned_bytes_read;
            }
            if (bytes_read > 0)
                producers_with_data++;
            total_bytes_read += bytes_read;
            if (bytes_read < scratch_bytes.size())
                scratch_bytes.slice(bytes_read).fill(0);

            for (size_t i = 0; i < aligned_buffer.size(); ++i) {
                float mixed = aligned_buffer[i] + scratch_span[i];
                // FIXME: Do not mask invalid samples here; producers should avoid NaN/Inf.
                aligned_buffer[i] = clamp(mixed, -1.0f, 1.0f);
            }
        }

        if (should_log_audio_server()) {
            AK::Duration now = AK::Duration::from_milliseconds(AK::MonotonicTime::now().milliseconds());
            if (last_debug_log_time.is_zero() || (now - last_debug_log_time) > AK::Duration::from_seconds(1)) {
                last_debug_log_time = now;
                float peak = 0.0f;
                for (float sample : aligned_buffer)
                    peak = max(peak, AK::abs(sample));
                dbgln("AudioServer: mixed callback (samples={}, producers={}, producers_with_data={}, bytes_read={}, peak={})",
                    aligned_buffer.size(), snapshot->producers.size(), producers_with_data, total_bytes_read, peak);
            }
        }

        snapshot->unref();

        return ReadonlySpan<float> { buffer };
    };

    auto stream_or_error = Audio::PlaybackStream::create(
        initial_output_state,
        target_latency_ms,
        move(sample_specification_callback),
        move(audio_data_request_callback));

    if (stream_or_error.is_error()) {
        if (should_log_audio_server())
            warnln("AudioServer: failed to start output device: {}", stream_or_error.error());
        // If we fail to start the device, just leave m_stream unset.
        return;
    }

    m_stream = stream_or_error.release_value();

    if (should_log_audio_server())
        dbgln("AudioServer: output device started");
}

void AudioOutputDevice::update_producer_snapshot()
{
    Vector<Producer> snapshot_producers;
    {
        Threading::MutexLocker locker(m_mutex);
        snapshot_producers.ensure_capacity(m_producers.size());
        for (auto& it : m_producers)
            snapshot_producers.unchecked_append(it.value);
    }

    ProducerSnapshot* old_snapshot = nullptr;
    if (snapshot_producers.is_empty()) {
        old_snapshot = m_producer_snapshot.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    } else {
        auto new_snapshot = adopt_ref(*new ProducerSnapshot);
        new_snapshot->producers = move(snapshot_producers);

        // Publish by storing a ref in the atomic.
        new_snapshot->ref();
        old_snapshot = m_producer_snapshot.exchange(new_snapshot.ptr(), AK::MemoryOrder::memory_order_acq_rel);
    }

    if (old_snapshot)
        old_snapshot->unref();
}

void AudioOutputDevice::set_producer_muted(u64 producer_id, bool muted)
{
    {
        Threading::MutexLocker locker(m_mutex);
        auto it = m_producers.find(producer_id);
        if (it == m_producers.end())
            return;
        it->value.muted = muted;
    }
    update_producer_snapshot();
}

bool AudioOutputDevice::has_sample_specification() const
{
    return m_has_sample_specification.load(AK::MemoryOrder::memory_order_acquire);
}

u32 AudioOutputDevice::device_sample_rate_hz() const
{
    return m_device_sample_rate_hz.load(AK::MemoryOrder::memory_order_acquire);
}

u32 AudioOutputDevice::device_channel_count() const
{
    return m_device_channel_count.load(AK::MemoryOrder::memory_order_acquire);
}

void AudioOutputDevice::when_ready(Function<void()> callback)
{
    if (has_sample_specification()) {
        callback();
        return;
    }

    Threading::MutexLocker locker(m_mutex);
    m_when_ready.append(move(callback));
}

void AudioOutputDevice::register_producer(u64 producer_id, Core::SharedSingleProducerCircularBuffer ring, size_t bytes_per_frame)
{
    {
        Threading::MutexLocker locker(m_mutex);
        m_producers.set(producer_id, Producer { .ring = move(ring), .bytes_per_frame = bytes_per_frame });
    }
    update_producer_snapshot();
}

void AudioOutputDevice::unregister_producer(u64 producer_id)
{
    {
        Threading::MutexLocker locker(m_mutex);
        m_producers.remove(producer_id);
    }
    update_producer_snapshot();
}

void AudioOutputDevice::notify_ready()
{
    Vector<Function<void()>> callbacks;
    {
        Threading::MutexLocker locker(m_mutex);
        callbacks = move(m_when_ready);
        m_when_ready.clear();
    }

    for (auto& callback : callbacks)
        callback();
}

}
