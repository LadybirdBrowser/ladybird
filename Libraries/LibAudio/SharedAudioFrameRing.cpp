/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibAudio/SharedAudioFrameRing.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <math.h>

namespace Audio {

// Far above anything a device can play, yet small enough that mixing bounded samples from any realistic number of
// streams, with gains of at most 1, stays finite.
static constexpr float MAXIMUM_SAMPLE_MAGNITUDE = 16.0f;

size_t SharedAudioFrameRing::buffer_size_for(u64 frame_capacity, u32 channel_count)
{
    return SAMPLES_OFFSET + frame_capacity * channel_count * sizeof(float);
}

ErrorOr<SharedAudioFrameRing> SharedAudioFrameRing::create(u32 sample_rate, u32 channel_count, size_t minimum_frame_capacity)
{
    VERIFY(sample_rate > 0);
    VERIFY(channel_count > 0);
    VERIFY(minimum_frame_capacity > 0);

    auto frame_capacity = round_up_to_a_power_of_two(minimum_frame_capacity);
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(buffer_size_for(frame_capacity, channel_count)));
    auto* header = new (buffer.data<void>()) Header;
    header->ring.channel_count = channel_count;
    header->ring.frame_capacity = frame_capacity;
    header->sample_rate = sample_rate;
    return SharedAudioFrameRing { move(buffer), header, sample_rate, channel_count, frame_capacity };
}

ErrorOr<SharedAudioFrameRing> SharedAudioFrameRing::create(Core::AnonymousBuffer buffer)
{
    if (!buffer.is_valid() || buffer.size() < SAMPLES_OFFSET)
        return Error::from_string_literal("Audio frame ring buffer is too small for its header");
    TRY(buffer.validate_backing_size());

    // Each dimension is read once: the other side of the ring can rewrite the header at any time.
    auto* header = reinterpret_cast<Header*>(buffer.data<void>());
    auto sample_rate = AK::atomic_load(&header->sample_rate, AK::MemoryOrder::memory_order_relaxed);
    auto channel_count = AK::atomic_load(&header->ring.channel_count, AK::MemoryOrder::memory_order_relaxed);
    auto frame_capacity = AK::atomic_load(&header->ring.frame_capacity, AK::MemoryOrder::memory_order_relaxed);
    if (sample_rate == 0 || channel_count == 0)
        return Error::from_string_literal("Audio frame ring has an invalid sample specification");
    if (frame_capacity == 0 || !is_power_of_two(frame_capacity))
        return Error::from_string_literal("Audio frame ring has an invalid frame capacity");
    // Divided rather than multiplied, so that no claimed capacity can wrap the size it needs.
    auto maximum_frame_capacity = (buffer.size() - SAMPLES_OFFSET) / (static_cast<u64>(channel_count) * sizeof(float));
    if (frame_capacity > maximum_frame_capacity)
        return Error::from_string_literal("Audio frame ring buffer is too small for its frame capacity");
    return SharedAudioFrameRing { move(buffer), header, sample_rate, channel_count, frame_capacity };
}

SharedAudioFrameRing::SharedAudioFrameRing(Core::AnonymousBuffer buffer, Header* header, u32 sample_rate, u32 channel_count, u64 frame_capacity)
    : m_buffer(move(buffer))
    , m_shared_header(header)
    , m_sample_rate(sample_rate)
{
    auto* samples = reinterpret_cast<float*>(m_buffer.data<u8>() + SAMPLES_OFFSET);
    set_storage(header->ring, channel_count, frame_capacity, { samples, static_cast<size_t>(frame_capacity * channel_count) });
}

static u64 frames_in_nanoseconds(i64 nanoseconds, u32 sample_rate)
{
    auto whole_seconds = nanoseconds / 1'000'000'000;
    auto remainder_ns = nanoseconds % 1'000'000'000;
    return static_cast<u64>(whole_seconds * sample_rate + (remainder_ns * sample_rate) / 1'000'000'000);
}

u64 SharedAudioFrameRing::estimate_frames_played(i64 monotonic_ns) const
{
    auto previous_estimate = m_last_estimated_frames_played.load(AK::MemoryOrder::memory_order_relaxed);
    auto anchor = m_shared_header->consumption.read();
    if (!anchor.has_value())
        return previous_estimate;

    u64 estimate = anchor->frames_consumed;
    if (monotonic_ns < anchor->played_at_ns) {
        // The device still holds the tail of what it consumed.
        auto pending_frames = frames_in_nanoseconds(anchor->played_at_ns - monotonic_ns, m_sample_rate);
        estimate = pending_frames >= estimate ? 0 : estimate - pending_frames;
    } else if (anchor->playing) {
        estimate += frames_in_nanoseconds(monotonic_ns - anchor->played_at_ns, m_sample_rate);
    }
    estimate = min(estimate, frames_written());

    // Only ever raise the published estimate, whichever thread gets there first.
    while (estimate > previous_estimate) {
        if (m_last_estimated_frames_played.compare_exchange_strong(previous_estimate, estimate, AK::MemoryOrder::memory_order_relaxed))
            return estimate;
    }
    return previous_estimate;
}

size_t SharedAudioFrameRing::pop_mixing_into(Span<float> mix, float gain)
{
    auto channel_count = m_channel_count;
    auto frame_capacity = m_frame_capacity;
    VERIFY(mix.size() % channel_count == 0);

    auto head = m_header->head.load(AK::MemoryOrder::memory_order_relaxed);
    auto tail = m_header->tail.load(AK::MemoryOrder::memory_order_acquire);
    auto frames_to_read = min<u64>(mix.size() / channel_count, tail - head);
    if (frames_to_read == 0)
        return 0;

    auto sample_capacity = frame_capacity * channel_count;
    auto ring_index = (head & (frame_capacity - 1)) * channel_count;
    // The producer can write anything into the samples, even while they are read, so each is read once and made
    // finite and bounded before it is mixed.
    for (size_t i = 0; i < frames_to_read * channel_count; i++) {
        auto sample = m_samples[ring_index];
        if (!isfinite(sample))
            sample = 0.0f;
        mix[i] += clamp(sample, -MAXIMUM_SAMPLE_MAGNITUDE, MAXIMUM_SAMPLE_MAGNITUDE) * gain;
        if (++ring_index == sample_capacity)
            ring_index = 0;
    }

    m_header->head.store(head + frames_to_read, AK::MemoryOrder::memory_order_release);
    return frames_to_read;
}

size_t SharedAudioFrameRing::discard(size_t maximum_frame_count)
{
    auto head = m_header->head.load(AK::MemoryOrder::memory_order_relaxed);
    auto tail = m_header->tail.load(AK::MemoryOrder::memory_order_acquire);
    auto frames_to_drop = min<u64>(maximum_frame_count, tail - head);
    m_header->head.store(head + frames_to_drop, AK::MemoryOrder::memory_order_release);
    return frames_to_drop;
}

void SharedAudioFrameRing::discard_all()
{
    auto tail = m_header->tail.load(AK::MemoryOrder::memory_order_acquire);
    m_header->head.store(tail, AK::MemoryOrder::memory_order_release);
}

void SharedAudioFrameRing::publish_consumption(i64 played_at_ns, bool playing)
{
    PlaybackStreamConsumptionAnchor anchor {
        .frames_consumed = m_header->head.load(AK::MemoryOrder::memory_order_relaxed),
        .played_at_ns = played_at_ns,
        .playing = playing,
    };
    m_shared_header->consumption.store(anchor);
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Audio::SharedAudioFrameRing const& ring)
{
    return encoder.encode(ring.buffer());
}

template<>
ErrorOr<Audio::SharedAudioFrameRing> decode(Decoder& decoder)
{
    auto buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    return Audio::SharedAudioFrameRing::create(move(buffer));
}

}
