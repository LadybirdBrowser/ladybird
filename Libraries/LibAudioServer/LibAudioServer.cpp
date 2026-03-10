/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/InputStream.h>
#include <AudioServer/OutputDriver.h>
#include <LibAudioServer/LibAudioServer.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, AudioServer::DeviceInfo const& info)
{
    TRY(encoder.encode(static_cast<u8>(info.type)));
    TRY(encoder.encode(info.device_handle));
    TRY(encoder.encode(info.label));
    TRY(encoder.encode(info.dom_device_id));
    TRY(encoder.encode(info.group_id));
    TRY(encoder.encode(info.sample_rate_hz));
    TRY(encoder.encode(info.channel_count));
    TRY(encoder.encode(info.channel_layout));
    TRY(encoder.encode(info.is_default));
    return {};
}

template<>
ErrorOr<AudioServer::DeviceInfo> decode(Decoder& decoder)
{
    AudioServer::DeviceInfo info;

    u8 raw_type = TRY(decoder.decode<u8>());
    if (raw_type < static_cast<u8>(AudioServer::DeviceInfo::Type::Output)
        || raw_type > static_cast<u8>(AudioServer::DeviceInfo::Type::Input))
        return Error::from_string_literal("Invalid DeviceInfo type");

    info.type = static_cast<AudioServer::DeviceInfo::Type>(raw_type);
    info.device_handle = TRY(decoder.decode<AudioServer::DeviceHandle>());
    info.label = TRY(decoder.decode<ByteString>());
    info.dom_device_id = TRY(decoder.decode<ByteString>());
    info.group_id = TRY(decoder.decode<ByteString>());
    info.sample_rate_hz = TRY(decoder.decode<u32>());
    info.channel_count = TRY(decoder.decode<u32>());
    info.channel_layout = TRY(decoder.decode<Audio::ChannelMap>());
    info.is_default = TRY(decoder.decode<bool>());
    return info;
}

template<>
ErrorOr<void> encode(Encoder& encoder, AudioServer::InputStreamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.stream_id));
    TRY(encoder.encode(descriptor.sample_rate_hz));
    TRY(encoder.encode(descriptor.channel_count));
    TRY(encoder.encode(descriptor.channel_capacity));
    TRY(encoder.encode(descriptor.capacity_frames));
    TRY(encoder.encode(descriptor.shared_memory));
    TRY(encoder.encode(descriptor.notify_fd));
    return {};
}

template<>
ErrorOr<AudioServer::InputStreamDescriptor> decode(Decoder& decoder)
{
    AudioServer::InputStreamDescriptor descriptor;
    descriptor.stream_id = TRY(decoder.decode<u64>());
    descriptor.sample_rate_hz = TRY(decoder.decode<u32>());
    descriptor.channel_count = TRY(decoder.decode<u32>());
    descriptor.channel_capacity = TRY(decoder.decode<u32>());
    descriptor.capacity_frames = TRY(decoder.decode<u64>());
    descriptor.shared_memory = TRY(decoder.decode<Core::AnonymousBuffer>());
    descriptor.notify_fd = TRY(decoder.decode<IPC::File>());
    return descriptor;
}

template<>
ErrorOr<void> encode(Encoder& encoder, AudioServer::CreateClientRequest const& info)
{
    TRY(encoder.encode(info.origin));
    TRY(encoder.encode(info.top_level_origin));
    TRY(encoder.encode(info.can_use_mic));
    return {};
}

template<>
ErrorOr<AudioServer::CreateClientRequest> decode(Decoder& decoder)
{
    AudioServer::CreateClientRequest info;
    info.origin = TRY(decoder.decode<ByteString>());
    info.top_level_origin = TRY(decoder.decode<ByteString>());
    info.can_use_mic = TRY(decoder.decode<bool>());
    return info;
}

template<>
ErrorOr<void> encode(Encoder& encoder, AudioServer::CreateClientResponse const& info)
{
    TRY(encoder.encode(info.socket));
    TRY(encoder.encode(info.grant_id));
    return {};
}

template<>
ErrorOr<AudioServer::CreateClientResponse> decode(Decoder& decoder)
{
    AudioServer::CreateClientResponse info;
    info.socket = TRY(decoder.decode<IPC::File>());
    info.grant_id = TRY(decoder.decode<ByteString>());
    return info;
}

template<>
ErrorOr<void> encode(Encoder& encoder, AudioServer::OutputSinkTransport const& pack)
{
    TRY(encoder.encode(pack.session_id));
    TRY(encoder.encode(pack.sample_rate));
    TRY(encoder.encode(pack.channel_count));
    TRY(encoder.encode(pack.channel_layout));
    TRY(encoder.encode(pack.sample_ring_buffer.anonymous_buffer()));
    TRY(encoder.encode(pack.timing_buffer));
    return {};
}

template<>
ErrorOr<AudioServer::OutputSinkTransport> decode(Decoder& decoder)
{
    AudioServer::OutputSinkTransport pack;
    pack.session_id = TRY(decoder.decode<u64>());
    pack.sample_rate = TRY(decoder.decode<u32>());
    pack.channel_count = TRY(decoder.decode<u32>());
    pack.channel_layout = TRY(decoder.decode<Audio::ChannelMap>());
    auto sample_ring_anonymous_buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    auto sample_ring_or_error = AudioServer::SharedCircularBuffer::attach(move(sample_ring_anonymous_buffer));
    if (sample_ring_or_error.is_error())
        return sample_ring_or_error.release_error();
    pack.sample_ring_buffer = sample_ring_or_error.release_value();
    pack.timing_buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    return pack;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Audio::ChannelMap const& channel_map)
{
    Vector<u8> channel_layout;
    channel_layout.ensure_capacity(channel_map.channel_count());
    for (u8 i = 0; i < channel_map.channel_count(); ++i)
        TRY(channel_layout.try_append(to_underlying(channel_map.channel_at(i))));
    TRY(encoder.encode(channel_layout));
    return {};
}

template<>
ErrorOr<Audio::ChannelMap> decode(Decoder& decoder)
{
    auto channel_layout = TRY(decoder.decode<Vector<u8>>());
    Vector<Audio::Channel, 32> channels;
    channels.resize(channel_layout.size());
    for (size_t i = 0; i < channel_layout.size(); ++i) {
        u8 encoded_channel = channel_layout[i];
        if (encoded_channel >= to_underlying(Audio::Channel::Count))
            channels[i] = Audio::Channel::Unknown;
        else
            channels[i] = static_cast<Audio::Channel>(encoded_channel);
    }
    return Audio::ChannelMap(channels);
}

}

namespace AudioServer {

ErrorOr<TimingReader> TimingReader::attach(Core::AnonymousBuffer buffer)
{
    if (!buffer.is_valid())
        return Error::from_string_literal("TimingReader: buffer is invalid");

    if (buffer.size() < sizeof(TimingInfo))
        return Error::from_string_literal("TimingReader: buffer too small");

    auto* storage = reinterpret_cast<TimingInfo*>(buffer.data<void>());
    if (!storage)
        return Error::from_string_literal("TimingReader: buffer had null mapping");

    if (storage->magic != TimingInfo::s_magic)
        return Error::from_string_literal("TimingReader: invalid magic");

    return TimingReader { move(buffer), storage };
}

Optional<TimingReader::Snapshot> TimingReader::read_snapshot() const
{
    if (!is_valid())
        return {};

    for (size_t i = 0; i < 3; ++i) {
        u32 sequence_before = m_storage->sequence.load(AK::MemoryOrder::memory_order_acquire);
        if ((sequence_before & 1u) != 0)
            continue;

        Snapshot snapshot {
            .device_played_frames = m_storage->device_played_frames.load(AK::MemoryOrder::memory_order_acquire),
            .ring_read_frames = m_storage->ring_read_frames.load(AK::MemoryOrder::memory_order_acquire),
            .server_monotonic_ns = m_storage->server_monotonic_ns.load(AK::MemoryOrder::memory_order_acquire),
            .underrun_count = m_storage->underrun_count.load(AK::MemoryOrder::memory_order_acquire),
        };

        u32 sequence_after = m_storage->sequence.load(AK::MemoryOrder::memory_order_acquire);
        if (sequence_before == sequence_after && (sequence_after & 1u) == 0)
            return snapshot;
    }

    return {};
}

}
