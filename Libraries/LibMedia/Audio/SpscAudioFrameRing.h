/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/FixedArray.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibMedia/Export.h>

namespace Media {

// A lock-free, allocation-free single-producer/single-consumer ring of interleaved float32
// frames. Its acquire/release head-tail discipline follows AK/SingleProducerCircularQueue.h;
// atomic reference counting permits producer and consumer ownership on different threads.
class MEDIA_API SpscAudioFrameRing : public AtomicRefCounted<SpscAudioFrameRing> {
public:
    // The frame capacity is rounded up to the next power of two.
    SpscAudioFrameRing(size_t frame_capacity, u32 channel_count);

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

private:
    size_t const m_frame_capacity { 0 };
    u32 const m_channel_count { 0 };
    FixedArray<float> m_samples;

    // Invariants (see AK/SingleProducerCircularQueue.h):
    // - m_head and m_tail are monotonically increasing frame counters, with tail >= head.
    // - m_tail is only written by the producer; m_head is only written by the consumer.
    // - m_tail - m_head is the number of frames in the ring, at most m_frame_capacity.
    AK_CACHE_ALIGNED Atomic<size_t> m_tail { 0 };
    AK_CACHE_ALIGNED Atomic<size_t> m_head { 0 };
};

}
