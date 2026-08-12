/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibMedia/Audio/SpscAudioFrameRing.h>

namespace Media {

static size_t round_up_to_a_power_of_two(size_t value)
{
    size_t result = 1;
    while (result < value)
        result <<= 1;
    return result;
}

SpscAudioFrameRing::SpscAudioFrameRing(size_t frame_capacity, u32 channel_count)
    : m_frame_capacity(round_up_to_a_power_of_two(frame_capacity))
    , m_channel_count(channel_count)
    , m_samples(MUST(FixedArray<float>::create(m_frame_capacity * channel_count)))
{
    VERIFY(frame_capacity > 0);
    VERIFY(channel_count > 0);
}

size_t SpscAudioFrameRing::try_push(ReadonlySpan<float> interleaved_samples)
{
    VERIFY(interleaved_samples.size() % m_channel_count == 0);

    auto tail = m_tail.load(AK::MemoryOrder::memory_order_relaxed);
    auto head = m_head.load(AK::MemoryOrder::memory_order_acquire);
    auto frames_to_write = min(interleaved_samples.size() / m_channel_count, m_frame_capacity - (tail - head));
    if (frames_to_write == 0)
        return 0;

    auto first_frame_index = tail & (m_frame_capacity - 1);
    auto first_chunk_frames = min(frames_to_write, m_frame_capacity - first_frame_index);
    interleaved_samples.slice(0, first_chunk_frames * m_channel_count).copy_to(m_samples.span().slice(first_frame_index * m_channel_count));
    if (first_chunk_frames < frames_to_write)
        interleaved_samples.slice(first_chunk_frames * m_channel_count, (frames_to_write - first_chunk_frames) * m_channel_count).copy_to(m_samples.span());

    m_tail.store(tail + frames_to_write, AK::MemoryOrder::memory_order_release);
    return frames_to_write;
}

size_t SpscAudioFrameRing::try_pop(Span<float> interleaved_samples)
{
    VERIFY(interleaved_samples.size() % m_channel_count == 0);

    auto head = m_head.load(AK::MemoryOrder::memory_order_relaxed);
    auto tail = m_tail.load(AK::MemoryOrder::memory_order_acquire);
    auto frames_to_read = min(interleaved_samples.size() / m_channel_count, tail - head);
    if (frames_to_read == 0)
        return 0;

    auto first_frame_index = head & (m_frame_capacity - 1);
    auto first_chunk_frames = min(frames_to_read, m_frame_capacity - first_frame_index);
    m_samples.span().slice(first_frame_index * m_channel_count, first_chunk_frames * m_channel_count).copy_to(interleaved_samples);
    if (first_chunk_frames < frames_to_read)
        m_samples.span().slice(0, (frames_to_read - first_chunk_frames) * m_channel_count).copy_to(interleaved_samples.slice(first_chunk_frames * m_channel_count));

    m_head.store(head + frames_to_read, AK::MemoryOrder::memory_order_release);
    return frames_to_read;
}

size_t SpscAudioFrameRing::frames_available() const
{
    auto head = m_head.load(AK::MemoryOrder::memory_order_relaxed);
    auto tail = m_tail.load(AK::MemoryOrder::memory_order_acquire);
    return tail - head;
}

size_t SpscAudioFrameRing::frames_free() const
{
    auto tail = m_tail.load(AK::MemoryOrder::memory_order_relaxed);
    auto head = m_head.load(AK::MemoryOrder::memory_order_acquire);
    return m_frame_capacity - (tail - head);
}

}
