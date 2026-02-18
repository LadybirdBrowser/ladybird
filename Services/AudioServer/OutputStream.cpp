/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Time.h>
#include <AudioServer/Debug.h>
#include <AudioServer/OutputDriver.h>
#include <AudioServer/OutputStream.h>
#include <LibCore/Event.h>
#include <LibCore/ThreadEventQueue.h>

namespace AudioServer {

ErrorOr<Core::AnonymousBuffer> OutputStream::create_timing_buffer()
{
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(sizeof(TimingInfo)));
    auto* storage = reinterpret_cast<TimingInfo*>(buffer.data<void>());
    if (!storage)
        return Error::from_string_literal("OutputStream: timing buffer had null mapping");

    storage->magic = TimingInfo::s_magic;
    storage->sequence.store(0, AK::MemoryOrder::memory_order_relaxed);
    storage->device_played_frames.store(0, AK::MemoryOrder::memory_order_relaxed);
    storage->ring_read_frames.store(0, AK::MemoryOrder::memory_order_relaxed);
    storage->server_monotonic_ns.store(0, AK::MemoryOrder::memory_order_relaxed);
    storage->underrun_count.store(0, AK::MemoryOrder::memory_order_relaxed);

    return buffer;
}

TimingInfo* OutputStream::timing_storage_from_buffer(Core::AnonymousBuffer& timing_buffer)
{
    if (!timing_buffer.is_valid())
        return nullptr;

    if (timing_buffer.size() < sizeof(TimingInfo))
        return nullptr;

    auto* storage = reinterpret_cast<TimingInfo*>(timing_buffer.data<void>());
    if (!storage)
        return nullptr;

    if (storage->magic != TimingInfo::s_magic)
        return nullptr;

    return storage;
}

void OutputStream::publish_timing(TimingInfo& storage, u64 device_played_frames, u64 server_monotonic_ns, u64 additional_ring_read_frames, u64 additional_underruns)
{
    storage.sequence.fetch_add(1, AK::MemoryOrder::memory_order_acq_rel);

    u64 ring_read_frames = storage.ring_read_frames.load(AK::MemoryOrder::memory_order_relaxed) + additional_ring_read_frames;
    u64 underrun_count = storage.underrun_count.load(AK::MemoryOrder::memory_order_relaxed) + additional_underruns;

    storage.device_played_frames.store(device_played_frames, AK::MemoryOrder::memory_order_release);
    storage.ring_read_frames.store(ring_read_frames, AK::MemoryOrder::memory_order_release);
    storage.server_monotonic_ns.store(server_monotonic_ns, AK::MemoryOrder::memory_order_release);
    storage.underrun_count.store(underrun_count, AK::MemoryOrder::memory_order_release);

    storage.sequence.fetch_add(1, AK::MemoryOrder::memory_order_release);
}

void OutputStream::OutputDriverDeleter::operator()(OutputDriver* driver)
{
    delete driver;
}

OutputStream::~OutputStream()
{
    ProducerSnapshot* snapshot = m_producer_snapshot.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    if (snapshot)
        snapshot->unref();
}

void OutputStream::ensure_started(Core::ThreadEventQueue& control_event_queue, u32 target_latency_ms)
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
    auto initial_output_state = OutputState::Playing;

    auto sample_specification_callback = [this, control_event_queue_ptr](Audio::SampleSpecification spec) {
        m_device_sample_rate_hz.store(spec.sample_rate(), AK::MemoryOrder::memory_order_release);
        m_device_channel_count.store(spec.channel_count(), AK::MemoryOrder::memory_order_release);

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

        u64 const device_played_frames = current_device_played_frames();
        u64 const server_monotonic_ns = static_cast<u64>(AK::MonotonicTime::now().milliseconds()) * 1000ull * 1000ull;

        ProducerSnapshot* snapshot = m_producer_snapshot.load(AK::MemoryOrder::memory_order_acquire);
        if (!snapshot)
            return ReadonlySpan<float> { buffer };

        snapshot->ref();

        auto out_bytes = Bytes { reinterpret_cast<u8*>(aligned_buffer.data()), aligned_buffer.size() * sizeof(float) };

        bool have_written_anything = false;
        size_t producers_with_data = 0;
        size_t total_bytes_read = 0;

        if (m_scratch.size() < buffer.size())
            m_scratch.resize(buffer.size());
        Span<float> scratch_span { m_scratch.data(), buffer.size() };
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

                if (auto* timing_storage = timing_storage_from_buffer(producer.timing_buffer); timing_storage) {
                    u64 read_frames = static_cast<u64>(bytes_read / producer.bytes_per_frame);
                    u64 session_played_frames = device_played_frames > producer.device_played_frame_base
                        ? (device_played_frames - producer.device_played_frame_base)
                        : 0;
                    publish_timing(*timing_storage, session_played_frames, server_monotonic_ns, read_frames, 0);
                }
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

                if (auto* timing_storage = timing_storage_from_buffer(producer.timing_buffer); timing_storage) {
                    u64 read_frames = static_cast<u64>(bytes_read / producer.bytes_per_frame);
                    u64 session_played_frames = device_played_frames > producer.device_played_frame_base
                        ? (device_played_frames - producer.device_played_frame_base)
                        : 0;
                    u64 additional_underruns = (bytes_read == 0) ? 1 : 0;
                    publish_timing(*timing_storage, session_played_frames, server_monotonic_ns, read_frames, additional_underruns);
                }

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

            if (auto* timing_storage = timing_storage_from_buffer(producer.timing_buffer); timing_storage) {
                u64 read_frames = static_cast<u64>(bytes_read / producer.bytes_per_frame);
                u64 session_played_frames = device_played_frames > producer.device_played_frame_base
                    ? (device_played_frames - producer.device_played_frame_base)
                    : 0;
                u64 additional_underruns = (bytes_read == 0) ? 1 : 0;
                publish_timing(*timing_storage, session_played_frames, server_monotonic_ns, read_frames, additional_underruns);
            }

            for (size_t i = 0; i < aligned_buffer.size(); ++i) {
                float mixed = aligned_buffer[i] + scratch_span[i];
                // FIXME: Do not mask invalid samples here; producers should avoid NaN/Inf.
                aligned_buffer[i] = clamp(mixed, -1.0f, 1.0f);
            }
        }

        if (should_log_audio_server()) {
            AK::Duration now = AK::Duration::from_milliseconds(AK::MonotonicTime::now().milliseconds());
            if (m_last_debug_log_time.is_zero() || (now - m_last_debug_log_time) > AK::Duration::from_seconds(1)) {
                m_last_debug_log_time = now;
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

    auto stream_or_error = create_platform_output_driver(
        m_device_handle,
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

void OutputStream::set_underrun_callback(Function<void()> callback)
{
    if (!m_stream)
        return;
    m_stream->set_underrun_callback(move(callback));
}

NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> OutputStream::resume()
{
    if (!m_stream) {
        auto promise = Core::ThreadedPromise<AK::Duration>::create();
        promise->reject(Error::from_string_literal("Audio output stream is unavailable"));
        return promise;
    }
    return m_stream->resume();
}

NonnullRefPtr<Core::ThreadedPromise<void>> OutputStream::drain_buffer_and_suspend()
{
    if (!m_stream) {
        auto promise = Core::ThreadedPromise<void>::create();
        promise->reject(Error::from_string_literal("Audio output stream is unavailable"));
        return promise;
    }
    return m_stream->drain_buffer_and_suspend();
}

NonnullRefPtr<Core::ThreadedPromise<void>> OutputStream::discard_buffer_and_suspend()
{
    if (!m_stream) {
        auto promise = Core::ThreadedPromise<void>::create();
        promise->reject(Error::from_string_literal("Audio output stream is unavailable"));
        return promise;
    }
    return m_stream->discard_buffer_and_suspend();
}

AK::Duration OutputStream::device_time_played() const
{
    if (!m_stream)
        return AK::Duration::zero();
    return m_stream->device_time_played();
}

NonnullRefPtr<Core::ThreadedPromise<void>> OutputStream::set_volume(double volume)
{
    if (!m_stream) {
        auto promise = Core::ThreadedPromise<void>::create();
        promise->reject(Error::from_string_literal("Audio output stream is unavailable"));
        return promise;
    }
    return m_stream->set_volume(volume);
}

void OutputStream::update_producer_snapshot()
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

void OutputStream::set_producer_muted(u64 producer_id, bool muted)
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

void OutputStream::when_ready(Function<void()> callback)
{
    u32 sample_rate_hz = m_device_sample_rate_hz.load(AK::MemoryOrder::memory_order_acquire);
    u32 channel_count = m_device_channel_count.load(AK::MemoryOrder::memory_order_acquire);
    if (sample_rate_hz > 0 && channel_count > 0) {
        callback();
        return;
    }

    Threading::MutexLocker locker(m_mutex);
    m_when_ready.append(move(callback));
}

u64 OutputStream::current_device_played_frames() const
{
    u32 sample_rate = m_device_sample_rate_hz.load(AK::MemoryOrder::memory_order_acquire);
    if (sample_rate == 0)
        return 0;

    AK::Duration time_played = device_time_played();
    i64 frames = time_played.to_time_units(1, sample_rate);
    return frames > 0 ? static_cast<u64>(frames) : 0;
}

void OutputStream::register_producer(u64 producer_id, AudioServer::SharedCircularBuffer ring, Core::AnonymousBuffer timing_buffer, size_t bytes_per_frame)
{
    if (timing_buffer.is_valid() && !timing_storage_from_buffer(timing_buffer)) {
        if (should_log_audio_server())
            warnln("AudioServer: invalid output timing buffer for producer {}", producer_id);
        timing_buffer = {};
    }

    u64 device_played_frame_base = current_device_played_frames();

    {
        Threading::MutexLocker locker(m_mutex);
        m_producers.set(producer_id, Producer {
                                         .ring = move(ring),
                                         .timing_buffer = move(timing_buffer),
                                         .device_played_frame_base = device_played_frame_base,
                                         .bytes_per_frame = bytes_per_frame,
                                     });
    }
    update_producer_snapshot();
}

void OutputStream::unregister_producer(u64 producer_id)
{
    {
        Threading::MutexLocker locker(m_mutex);
        m_producers.remove(producer_id);
    }
    update_producer_snapshot();
}

void OutputStream::notify_ready()
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
