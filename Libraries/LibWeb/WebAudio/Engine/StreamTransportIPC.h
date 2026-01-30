/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/WebAudio/Engine/StreamTransportDescriptors.h>

template<>
inline ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebAudio::Render::RingStreamFormat const& format)
{
    TRY(encoder.encode(format.sample_rate_hz));
    TRY(encoder.encode(format.channel_count));
    TRY(encoder.encode(format.channel_capacity));
    TRY(encoder.encode(format.capacity_frames));
    return {};
}

template<>
inline ErrorOr<Web::WebAudio::Render::RingStreamFormat> IPC::decode(Decoder& decoder)
{
    Web::WebAudio::Render::RingStreamFormat format;
    format.sample_rate_hz = TRY(decoder.decode<u32>());
    format.channel_count = TRY(decoder.decode<u32>());
    format.channel_capacity = TRY(decoder.decode<u32>());
    format.capacity_frames = TRY(decoder.decode<u64>());
    return format;
}

template<>
inline ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebAudio::Render::RingStreamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.stream_id));
    TRY(encoder.encode(descriptor.format));
    TRY(encoder.encode(static_cast<u8>(descriptor.overflow_policy)));
    TRY(encoder.encode(descriptor.shared_memory));
    TRY(encoder.encode(descriptor.notify_fd));
    return {};
}

template<>
inline ErrorOr<Web::WebAudio::Render::RingStreamDescriptor> IPC::decode(Decoder& decoder)
{
    Web::WebAudio::Render::RingStreamDescriptor descriptor;
    descriptor.stream_id = TRY(decoder.decode<Web::WebAudio::Render::StreamID>());
    descriptor.format = TRY(decoder.decode<Web::WebAudio::Render::RingStreamFormat>());
    descriptor.overflow_policy = static_cast<Web::WebAudio::Render::StreamOverflowPolicy>(TRY(decoder.decode<u8>()));
    descriptor.shared_memory = TRY(decoder.decode<Core::AnonymousBuffer>());
    descriptor.notify_fd = TRY(decoder.decode<IPC::File>());
    return descriptor;
}

template<>
inline ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebAudio::Render::AudioInputStreamMetadata const& metadata)
{
    TRY(encoder.encode(metadata.device_id));
    TRY(encoder.encode(metadata.sample_rate_hz));
    TRY(encoder.encode(metadata.channel_count));
    TRY(encoder.encode(metadata.capacity_frames));
    TRY(encoder.encode(metadata.overflow_policy));
    return {};
}

template<>
inline ErrorOr<Web::WebAudio::Render::AudioInputStreamMetadata> IPC::decode(Decoder& decoder)
{
    Web::WebAudio::Render::AudioInputStreamMetadata metadata;
    metadata.device_id = TRY(decoder.decode<AudioServer::AudioInputDeviceID>());
    metadata.sample_rate_hz = TRY(decoder.decode<u32>());
    metadata.channel_count = TRY(decoder.decode<u32>());
    metadata.capacity_frames = TRY(decoder.decode<u64>());
    metadata.overflow_policy = TRY(decoder.decode<u8>());
    return metadata;
}

template<>
inline ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.provider_id));
    TRY(encoder.encode(descriptor.ring_stream));
    return {};
}

template<>
inline ErrorOr<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor> IPC::decode(Decoder& decoder)
{
    Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor descriptor;
    descriptor.provider_id = TRY(decoder.decode<u64>());
    descriptor.ring_stream = TRY(decoder.decode<Web::WebAudio::Render::RingStreamDescriptor>());
    return descriptor;
}

template<>
inline ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.provider_id));
    TRY(encoder.encode(descriptor.metadata));
    return {};
}

template<>
inline ErrorOr<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> IPC::decode(Decoder& decoder)
{
    Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor descriptor;
    descriptor.provider_id = TRY(decoder.decode<u64>());
    descriptor.metadata = TRY(decoder.decode<Web::WebAudio::Render::AudioInputStreamMetadata>());
    return descriptor;
}

template<>
inline ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebAudio::Render::SharedBufferStreamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.pool_buffer));
    TRY(encoder.encode(descriptor.ready_ring_buffer));
    TRY(encoder.encode(descriptor.free_ring_buffer));
    return {};
}

template<>
inline ErrorOr<Web::WebAudio::Render::SharedBufferStreamDescriptor> IPC::decode(Decoder& decoder)
{
    Web::WebAudio::Render::SharedBufferStreamDescriptor descriptor;
    descriptor.pool_buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    descriptor.ready_ring_buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    descriptor.free_ring_buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    return descriptor;
}

template<>
inline ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebAudio::Render::ScriptProcessorStreamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.node_id));
    TRY(encoder.encode(descriptor.buffer_size));
    TRY(encoder.encode(descriptor.input_channel_count));
    TRY(encoder.encode(descriptor.output_channel_count));
    TRY(encoder.encode(descriptor.request_stream));
    TRY(encoder.encode(descriptor.response_stream));
    TRY(encoder.encode(descriptor.request_notify_write_fd));
    return {};
}

template<>
inline ErrorOr<Web::WebAudio::Render::ScriptProcessorStreamDescriptor> IPC::decode(Decoder& decoder)
{
    Web::WebAudio::Render::ScriptProcessorStreamDescriptor descriptor;
    descriptor.node_id = TRY(decoder.decode<u64>());
    descriptor.buffer_size = TRY(decoder.decode<u32>());
    descriptor.input_channel_count = TRY(decoder.decode<u32>());
    descriptor.output_channel_count = TRY(decoder.decode<u32>());
    descriptor.request_stream = TRY(decoder.decode<Web::WebAudio::Render::SharedBufferStreamDescriptor>());
    descriptor.response_stream = TRY(decoder.decode<Web::WebAudio::Render::SharedBufferStreamDescriptor>());
    descriptor.request_notify_write_fd = TRY(decoder.decode<IPC::File>());
    return descriptor;
}

template<>
inline ErrorOr<void> IPC::encode(Encoder& encoder, Web::WebAudio::Render::WorkletNodePortDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.node_id));
    TRY(encoder.encode(descriptor.processor_port_fd));
    return {};
}

template<>
inline ErrorOr<Web::WebAudio::Render::WorkletNodePortDescriptor> IPC::decode(Decoder& decoder)
{
    Web::WebAudio::Render::WorkletNodePortDescriptor descriptor;
    descriptor.node_id = TRY(decoder.decode<u64>());
    descriptor.processor_port_fd = TRY(decoder.decode<IPC::File>());
    return descriptor;
}
