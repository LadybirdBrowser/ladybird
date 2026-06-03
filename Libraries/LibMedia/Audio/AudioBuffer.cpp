/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/TypedTransfer.h>
#include <LibMedia/Audio/AudioBuffer.h>

namespace Audio {

AudioBuffer::AudioBuffer(SampleSpecification sample_specification)
    : m_sample_specification(sample_specification)
{
}

void AudioBuffer::clear()
{
    m_frame_count = 0;
    m_start_offset = 0;
}

void AudioBuffer::ensure_capacity(size_t required_frame_capacity)
{
    if (m_frame_capacity >= required_frame_capacity)
        return;

    auto new_frame_capacity = max(required_frame_capacity, max<size_t>(m_frame_capacity * 2, 1));
    auto new_data = MUST(FixedArray<float>::create(new_frame_capacity * m_sample_specification.channel_count()));
    for (size_t channel = 0; channel < m_sample_specification.channel_count(); channel++) {
        auto new_channel = new_data.span().slice(channel * new_frame_capacity, m_frame_count);
        copy_channel_from_buffer(channel, 0, new_channel);
    }

    m_data = move(new_data);
    m_frame_capacity = new_frame_capacity;
    m_start_offset = 0;
}

void AudioBuffer::copy_channel_to_buffer(ReadonlySpan<float> source, size_t channel, size_t destination_offset)
{
    VERIFY(channel < m_sample_specification.channel_count());
    VERIFY(destination_offset <= m_frame_capacity);
    VERIFY(source.size() <= m_frame_capacity - destination_offset);

    auto write_offset = (m_start_offset + destination_offset) % m_frame_capacity;
    auto first_chunk_size = min(source.size(), m_frame_capacity - write_offset);
    auto channel_data = m_data.span().slice(channel * m_frame_capacity, m_frame_capacity);
    AK::TypedTransfer<float>::copy(channel_data.slice(write_offset).data(), source.data(), first_chunk_size);

    auto second_chunk_size = source.size() - first_chunk_size;
    if (second_chunk_size > 0)
        AK::TypedTransfer<float>::copy(channel_data.data(), source.slice(first_chunk_size).data(), second_chunk_size);
}

void AudioBuffer::copy_channel_from_buffer(size_t channel, size_t source_offset, Span<float> destination) const
{
    VERIFY(channel < m_sample_specification.channel_count());
    VERIFY(source_offset <= m_frame_count);
    VERIFY(destination.size() <= m_frame_count - source_offset);

    if (destination.is_empty())
        return;

    auto read_offset = (m_start_offset + source_offset) % m_frame_capacity;
    auto first_chunk_size = min(destination.size(), m_frame_capacity - read_offset);
    auto channel_data = m_data.span().slice(channel * m_frame_capacity, m_frame_capacity);
    AK::TypedTransfer<float>::copy(destination.data(), channel_data.slice(read_offset).data(), first_chunk_size);

    auto second_chunk_size = destination.size() - first_chunk_size;
    if (second_chunk_size > 0)
        AK::TypedTransfer<float>::copy(destination.slice(first_chunk_size).data(), channel_data.data(), second_chunk_size);
}

void AudioBuffer::zero_channel(size_t channel, size_t destination_offset, size_t frame_count)
{
    VERIFY(channel < m_sample_specification.channel_count());
    VERIFY(destination_offset <= m_frame_capacity);
    VERIFY(frame_count <= m_frame_capacity - destination_offset);

    auto write_offset = (m_start_offset + destination_offset) % m_frame_capacity;
    auto first_chunk_size = min(frame_count, m_frame_capacity - write_offset);
    auto channel_data = m_data.span().slice(channel * m_frame_capacity, m_frame_capacity);
    for (auto& sample : channel_data.slice(write_offset, first_chunk_size))
        sample = 0.0f;

    auto second_chunk_size = frame_count - first_chunk_size;
    if (second_chunk_size > 0) {
        for (auto& sample : channel_data.slice(0, second_chunk_size))
            sample = 0.0f;
    }
}

void AudioBuffer::append(Media::AudioBlock const& block)
{
    VERIFY(block.sample_specification() == m_sample_specification);
    if (block.frame_count() == 0)
        return;

    auto old_frame_count = m_frame_count;
    ensure_capacity(m_frame_count + block.frame_count());
    for (size_t channel = 0; channel < block.channel_count(); channel++)
        copy_channel_to_buffer(block.channel_data(channel), channel, old_frame_count);
    m_frame_count += block.frame_count();
}

void AudioBuffer::append_silence(size_t frame_count)
{
    if (frame_count == 0)
        return;

    auto old_frame_count = m_frame_count;
    ensure_capacity(m_frame_count + frame_count);
    for (size_t channel = 0; channel < m_sample_specification.channel_count(); channel++)
        zero_channel(channel, old_frame_count, frame_count);
    m_frame_count += frame_count;
}

void AudioBuffer::drop_front(size_t frame_count)
{
    VERIFY(frame_count <= m_frame_count);
    if (frame_count == 0)
        return;

    m_frame_count -= frame_count;
    if (m_frame_count == 0) {
        m_start_offset = 0;
        return;
    }

    m_start_offset = (m_start_offset + frame_count) % m_frame_capacity;
}

void AudioBuffer::copy_frames_to(size_t source_offset, size_t frame_count, size_t destination_offset, Media::AudioBlock& destination) const
{
    VERIFY(destination.sample_specification() == m_sample_specification);
    VERIFY(source_offset <= m_frame_count);
    VERIFY(frame_count <= m_frame_count - source_offset);
    VERIFY(destination_offset <= destination.frame_count());
    VERIFY(frame_count <= destination.frame_count() - destination_offset);

    for (size_t channel = 0; channel < destination.channel_count(); channel++) {
        auto destination_channel = destination.channel_data(channel).slice(destination_offset, frame_count);
        copy_channel_from_buffer(channel, source_offset, destination_channel);
    }
}

}
