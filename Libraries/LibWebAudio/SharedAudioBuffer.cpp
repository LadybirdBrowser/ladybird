/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Checked.h>
#include <AK/StdLibExtras.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebAudio/SharedAudioBuffer.h>

namespace Web::WebAudio::Render {

static constexpr u32 shared_audio_buffer_magic = 0x53414246; // SABF

struct SharedAudioBufferHeader {
    u32 magic;
    u32 channel_count;
    u64 length_in_sample_frames;
    f32 sample_rate_hz;
    u32 reserved;
};

static_assert(sizeof(SharedAudioBufferHeader) % alignof(f32) == 0);

static ErrorOr<size_t> checked_sample_count(size_t channel_count, size_t length_in_sample_frames)
{
    Checked<size_t> count = channel_count;
    count *= length_in_sample_frames;
    if (count.has_overflow())
        return Error::from_string_literal("SharedAudioBuffer sample count overflow");
    return count.value();
}

static ErrorOr<size_t> checked_storage_size_bytes(size_t channel_count, size_t length_in_sample_frames)
{
    size_t sample_count = TRY(checked_sample_count(channel_count, length_in_sample_frames));

    Checked<size_t> size = sizeof(SharedAudioBufferHeader);
    size += sample_count * sizeof(f32);
    if (size.has_overflow())
        return Error::from_string_literal("SharedAudioBuffer storage size overflow");
    return size.value();
}

SharedAudioBuffer::SharedAudioBuffer(f32 sample_rate, size_t channel_count, size_t length_in_sample_frames, Core::AnonymousBuffer storage)
    : m_sample_rate(sample_rate)
    , m_channel_count(channel_count)
    , m_length_in_sample_frames(length_in_sample_frames)
    , m_data(move(storage))
{
}

NonnullRefPtr<SharedAudioBuffer> SharedAudioBuffer::create(f32 sample_rate, size_t channel_count, size_t length_in_sample_frames, Vector<Vector<f32>>&& channels)
{
    size_t storage_size_bytes = MUST(checked_storage_size_bytes(channel_count, length_in_sample_frames));
    Core::AnonymousBuffer storage = MUST(Core::AnonymousBuffer::create_with_size(storage_size_bytes));

    auto* header = storage.data<SharedAudioBufferHeader>();
    header->magic = shared_audio_buffer_magic;
    header->channel_count = static_cast<u32>(channel_count);
    header->length_in_sample_frames = static_cast<u64>(length_in_sample_frames);
    header->sample_rate_hz = sample_rate;
    header->reserved = 0;

    auto* samples = reinterpret_cast<f32*>(reinterpret_cast<u8*>(header) + sizeof(SharedAudioBufferHeader));
    size_t sample_count = MUST(checked_sample_count(channel_count, length_in_sample_frames));
    for (size_t i = 0; i < sample_count; ++i)
        samples[i] = 0.0f;

    size_t channels_to_copy = min(channel_count, channels.size());
    for (size_t ch = 0; ch < channels_to_copy; ++ch) {
        auto const& source = channels[ch];
        size_t frames_to_copy = min(length_in_sample_frames, source.size());
        f32* destination = samples + (ch * length_in_sample_frames);
        for (size_t i = 0; i < frames_to_copy; ++i)
            destination[i] = source[i];
    }

    return adopt_ref(*new SharedAudioBuffer(sample_rate, channel_count, length_in_sample_frames, move(storage)));
}

ErrorOr<NonnullRefPtr<SharedAudioBuffer>> SharedAudioBuffer::create_from_buffer(Core::AnonymousBuffer shared_memory)
{
    if (!shared_memory.is_valid())
        return Error::from_string_literal("SharedAudioBuffer shared memory is invalid");

    if (shared_memory.size() < sizeof(SharedAudioBufferHeader))
        return Error::from_string_literal("SharedAudioBuffer shared memory is too small");

    auto const* header = shared_memory.data<SharedAudioBufferHeader>();
    if (header->magic != shared_audio_buffer_magic)
        return Error::from_string_literal("SharedAudioBuffer magic mismatch");

    size_t channel_count = static_cast<size_t>(header->channel_count);
    size_t length_in_sample_frames = static_cast<size_t>(header->length_in_sample_frames);
    size_t required_size = TRY(checked_storage_size_bytes(channel_count, length_in_sample_frames));
    if (shared_memory.size() < required_size)
        return Error::from_string_literal("SharedAudioBuffer shared memory payload is truncated");

    return adopt_ref(*new SharedAudioBuffer(header->sample_rate_hz, channel_count, length_in_sample_frames, move(shared_memory)));
}

f32 SharedAudioBuffer::sample_rate() const
{
    return m_sample_rate;
}

size_t SharedAudioBuffer::channel_count() const
{
    return m_channel_count;
}

size_t SharedAudioBuffer::length_in_sample_frames() const
{
    return m_length_in_sample_frames;
}

ReadonlySpan<f32> SharedAudioBuffer::channel(size_t index) const
{
    if (index >= m_channel_count)
        return {};

    if (!m_data.is_valid())
        return {};

    if (m_data.size() < sizeof(SharedAudioBufferHeader))
        return {};

    auto const* header = m_data.data<SharedAudioBufferHeader>();
    if (header->magic != shared_audio_buffer_magic)
        return {};

    auto const* base = reinterpret_cast<f32 const*>(reinterpret_cast<u8 const*>(header) + sizeof(SharedAudioBufferHeader));

    return ReadonlySpan<f32> { base + (index * m_length_in_sample_frames), m_length_in_sample_frames };
}

Core::AnonymousBuffer SharedAudioBuffer::to_buffer() const
{
    return m_data;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::Render::SharedAudioBufferBinding const& binding)
{
    TRY(encoder.encode(binding.buffer_id));
    TRY(encoder.encode(binding.shared_memory));
    return {};
}

template<>
ErrorOr<Web::WebAudio::Render::SharedAudioBufferBinding> decode(Decoder& decoder)
{
    Web::WebAudio::Render::SharedAudioBufferBinding binding;
    binding.buffer_id = TRY(decoder.decode<u64>());
    binding.shared_memory = TRY(decoder.decode<Core::AnonymousBuffer>());
    return binding;
}

}
