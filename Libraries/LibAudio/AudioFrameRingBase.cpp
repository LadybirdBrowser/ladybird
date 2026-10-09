/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibAudio/AudioFrameRingBase.h>

namespace Audio {

u64 AudioFrameRingBase::round_up_to_a_power_of_two(u64 value)
{
    u64 result = 1;
    while (result < value)
        result <<= 1;
    return result;
}

size_t AudioFrameRingBase::try_push(ReadonlySpan<float> interleaved_samples)
{
    auto channel_count = m_channel_count;
    auto frame_capacity = m_frame_capacity;
    VERIFY(interleaved_samples.size() % channel_count == 0);

    auto tail = m_header->tail.load(AK::MemoryOrder::memory_order_relaxed);
    auto head = m_header->head.load(AK::MemoryOrder::memory_order_acquire);
    auto frames_to_write = min<u64>(interleaved_samples.size() / channel_count, frame_capacity - (tail - head));
    if (frames_to_write == 0)
        return 0;

    auto first_frame_index = tail & (frame_capacity - 1);
    auto first_chunk_frames = min(frames_to_write, frame_capacity - first_frame_index);
    interleaved_samples.slice(0, first_chunk_frames * channel_count).copy_to(m_samples.slice(first_frame_index * channel_count));
    if (first_chunk_frames < frames_to_write)
        interleaved_samples.slice(first_chunk_frames * channel_count, (frames_to_write - first_chunk_frames) * channel_count).copy_to(m_samples);

    m_header->tail.store(tail + frames_to_write, AK::MemoryOrder::memory_order_release);
    return frames_to_write;
}

size_t AudioFrameRingBase::try_pop(Span<float> interleaved_samples)
{
    auto channel_count = m_channel_count;
    auto frame_capacity = m_frame_capacity;
    VERIFY(interleaved_samples.size() % channel_count == 0);

    auto head = m_header->head.load(AK::MemoryOrder::memory_order_relaxed);
    auto tail = m_header->tail.load(AK::MemoryOrder::memory_order_acquire);
    auto frames_to_read = min<u64>(interleaved_samples.size() / channel_count, tail - head);
    if (frames_to_read == 0)
        return 0;

    auto first_frame_index = head & (frame_capacity - 1);
    auto first_chunk_frames = min(frames_to_read, frame_capacity - first_frame_index);
    m_samples.slice(first_frame_index * channel_count, first_chunk_frames * channel_count).copy_to(interleaved_samples);
    if (first_chunk_frames < frames_to_read)
        m_samples.slice(0, (frames_to_read - first_chunk_frames) * channel_count).copy_to(interleaved_samples.slice(first_chunk_frames * channel_count));

    m_header->head.store(head + frames_to_read, AK::MemoryOrder::memory_order_release);
    return frames_to_read;
}

size_t AudioFrameRingBase::frames_available() const
{
    auto head = m_header->head.load(AK::MemoryOrder::memory_order_relaxed);
    auto tail = m_header->tail.load(AK::MemoryOrder::memory_order_acquire);
    return tail - head;
}

size_t AudioFrameRingBase::frames_free() const
{
    auto tail = m_header->tail.load(AK::MemoryOrder::memory_order_relaxed);
    auto head = m_header->head.load(AK::MemoryOrder::memory_order_acquire);
    return m_frame_capacity - (tail - head);
}

}
