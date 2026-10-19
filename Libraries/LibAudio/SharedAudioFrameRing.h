/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Error.h>
#include <AK/Platform.h>
#include <AK/SeqLock.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibAudio/AudioFrameRingBase.h>
#include <LibAudio/Export.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/Forward.h>

namespace Audio {

// Where the consumer stood at its last device callback, and when the device will have played that far.
struct PlaybackStreamConsumptionAnchor {
    u64 frames_consumed { 0 };
    i64 played_at_ns { 0 };
    bool playing { false };
};
static_assert(IsTriviallyCopyable<PlaybackStreamConsumptionAnchor>);

// A single-producer, single-consumer ring of interleaved float frames in shared memory, with the consumption anchor.
class AUDIO_API SharedAudioFrameRing final : public AudioFrameRingBase {
public:
    // Producer side. The frame capacity is rounded up to the next power of two.
    static ErrorOr<SharedAudioFrameRing> create(u32 sample_rate, u32 channel_count, size_t minimum_frame_capacity);
    // Consumer side, from a buffer received over IPC. Fails unless the header agrees with the buffer's size.
    static ErrorOr<SharedAudioFrameRing> create(Core::AnonymousBuffer);

    SharedAudioFrameRing(SharedAudioFrameRing const& other)
        : AudioFrameRingBase(other)
        , m_buffer(other.m_buffer)
        , m_shared_header(other.m_shared_header)
        , m_sample_rate(other.m_sample_rate)
        , m_last_estimated_frames_played(other.m_last_estimated_frames_played.load(AK::MemoryOrder::memory_order_relaxed))
    {
    }
    SharedAudioFrameRing(SharedAudioFrameRing&& other)
        : AudioFrameRingBase(other)
        , m_buffer(move(other.m_buffer))
        , m_shared_header(exchange(other.m_shared_header, nullptr))
        , m_sample_rate(other.m_sample_rate)
        , m_last_estimated_frames_played(other.m_last_estimated_frames_played.load(AK::MemoryOrder::memory_order_relaxed))
    {
    }
    SharedAudioFrameRing& operator=(SharedAudioFrameRing const& other)
    {
        if (this == &other)
            return *this;
        AudioFrameRingBase::operator=(other);
        m_buffer = other.m_buffer;
        m_shared_header = other.m_shared_header;
        m_sample_rate = other.m_sample_rate;
        m_last_estimated_frames_played.store(other.m_last_estimated_frames_played.load(AK::MemoryOrder::memory_order_relaxed), AK::MemoryOrder::memory_order_relaxed);
        return *this;
    }
    SharedAudioFrameRing& operator=(SharedAudioFrameRing&& other)
    {
        if (this == &other)
            return *this;
        AudioFrameRingBase::operator=(other);
        m_buffer = move(other.m_buffer);
        m_shared_header = exchange(other.m_shared_header, nullptr);
        m_sample_rate = other.m_sample_rate;
        m_last_estimated_frames_played.store(other.m_last_estimated_frames_played.load(AK::MemoryOrder::memory_order_relaxed), AK::MemoryOrder::memory_order_relaxed);
        return *this;
    }

    Core::AnonymousBuffer const& buffer() const { return m_buffer; }
    u32 sample_rate() const { return m_sample_rate; }

    // Producer only.
    u64 frames_written() const { return m_header->tail.load(AK::MemoryOrder::memory_order_relaxed); }
    // Monotonic and never past what was written; extrapolates from the anchor while the consumer was playing. Any thread.
    u64 estimate_frames_played(i64 monotonic_ns) const;

    // Consumer only.
    // Adds this ring's next frames, scaled by gain, into `mix` and returns how many frames it added.
    size_t pop_mixing_into(Span<float> mix, float gain);
    // Drops up to the given number of this ring's next frames and returns how many it dropped.
    size_t discard(size_t maximum_frame_count);
    void discard_all();
    // `played_at_ns` is when the device will have played everything consumed so far.
    void publish_consumption(i64 played_at_ns, bool playing);

private:
    struct Header {
        AudioFrameRingHeader ring;
        u32 sample_rate { 0 };
        AK_CACHE_ALIGNED SeqLock<PlaybackStreamConsumptionAnchor> consumption;
    };
    static constexpr size_t SAMPLES_OFFSET = (sizeof(Header) + AK_SYSTEM_CACHE_ALIGNMENT_SIZE - 1) & ~(AK_SYSTEM_CACHE_ALIGNMENT_SIZE - 1);

    static size_t buffer_size_for(u64 frame_capacity, u32 channel_count);

    SharedAudioFrameRing(Core::AnonymousBuffer, Header*, u32 sample_rate, u32 channel_count, u64 frame_capacity);

    Core::AnonymousBuffer m_buffer;
    Header* m_shared_header { nullptr };
    u32 m_sample_rate { 0 };
    mutable Atomic<u64> m_last_estimated_frames_played { 0 };
};

}

namespace IPC {

template<>
AUDIO_API ErrorOr<void> encode(Encoder&, Audio::SharedAudioFrameRing const&);

template<>
AUDIO_API ErrorOr<Audio::SharedAudioFrameRing> decode(Decoder&);

}
