/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AudioServer/AudioInputStreamDescriptor.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

template<>
inline ErrorOr<void> IPC::encode(Encoder& encoder, AudioServer::RingStreamFormat const& format)
{
    TRY(encoder.encode(format.sample_rate_hz));
    TRY(encoder.encode(format.channel_count));
    TRY(encoder.encode(format.channel_capacity));
    TRY(encoder.encode(format.capacity_frames));
    return {};
}

template<>
inline ErrorOr<AudioServer::RingStreamFormat> IPC::decode(Decoder& decoder)
{
    AudioServer::RingStreamFormat format;
    format.sample_rate_hz = TRY(decoder.decode<u32>());
    format.channel_count = TRY(decoder.decode<u32>());
    format.channel_capacity = TRY(decoder.decode<u32>());
    format.capacity_frames = TRY(decoder.decode<u64>());
    return format;
}

template<>
inline ErrorOr<void> IPC::encode(Encoder& encoder, AudioServer::AudioInputStreamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.stream_id));
    TRY(encoder.encode(descriptor.format));
    TRY(encoder.encode(static_cast<u8>(descriptor.overflow_policy)));
    TRY(encoder.encode(descriptor.shared_memory));
    TRY(encoder.encode(descriptor.notify_fd));
    return {};
}

template<>
inline ErrorOr<AudioServer::AudioInputStreamDescriptor> IPC::decode(Decoder& decoder)
{
    AudioServer::AudioInputStreamDescriptor descriptor;
    descriptor.stream_id = TRY(decoder.decode<AudioServer::AudioInputStreamID>());
    descriptor.format = TRY(decoder.decode<AudioServer::RingStreamFormat>());
    descriptor.overflow_policy = static_cast<AudioServer::StreamOverflowPolicy>(TRY(decoder.decode<u8>()));
    descriptor.shared_memory = TRY(decoder.decode<Core::AnonymousBuffer>());
    descriptor.notify_fd = TRY(decoder.decode<IPC::File>());
    return descriptor;
}
