/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Platform.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibAudio/Export.h>

namespace Audio {

// Invariants:
// - head and tail are monotonically increasing frame counters, with tail >= head.
// - tail is only written by the producer; head is only written by the consumer.
// - tail - head is the number of frames in the ring, at most frame_capacity.
struct AudioFrameRingHeader {
    u32 channel_count { 0 };
    u64 frame_capacity { 0 };
    AK_CACHE_ALIGNED Atomic<u64> tail { 0 };
    AK_CACHE_ALIGNED Atomic<u64> head { 0 };
};

// The lock-free single-producer/single-consumer discipline of a ring of interleaved float32 frames, over a header
// and sample storage owned by the derived class.
class AUDIO_API AudioFrameRingBase {
public:
    u32 channel_count() const { return m_channel_count; }
    size_t frame_capacity() const { return m_frame_capacity; }

    // Producer only. Writes as many whole frames as fit and returns the number of frames
    // actually written; the caller decides what to do with any frames that did not fit.
    size_t try_push(ReadonlySpan<float> interleaved_samples);

    // Consumer only. Reads up to interleaved_samples.size() / channel_count() frames and
    // returns the number of frames actually read.
    size_t try_pop(Span<float> interleaved_samples);

    // Conservative on the consumer thread: at least this many frames can be popped.
    size_t frames_available() const;
    // Conservative on the producer thread: at least this many frames can be pushed.
    size_t frames_free() const;

protected:
    AudioFrameRingBase() = default;

    static u64 round_up_to_a_power_of_two(u64);

    // The dimensions come from the caller: the header may be in memory the other side of the ring can write.
    void set_storage(AudioFrameRingHeader& header, u32 channel_count, u64 frame_capacity, Span<float> samples)
    {
        m_header = &header;
        m_samples = samples;
        m_channel_count = channel_count;
        m_frame_capacity = frame_capacity;
    }

    AudioFrameRingHeader* m_header { nullptr };
    Span<float> m_samples;
    u32 m_channel_count { 0 };
    size_t m_frame_capacity { 0 };
};

}
