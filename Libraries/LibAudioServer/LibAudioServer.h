/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibAudioServer/SharedCircularBuffer.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Audio/ChannelMap.h>

namespace AudioServer {

using DeviceHandle = u64;

struct DeviceInfo {
    enum class Type : u8 {
        Output = 1,
        Input = 2,
    };

    Type type { Type::Output };
    DeviceHandle device_handle { 0 };
    ByteString label;
    ByteString dom_device_id;
    ByteString group_id;
    u32 sample_rate_hz { 0 };
    u32 channel_count { 0 };
    Audio::ChannelMap channel_layout;
    bool is_default { false };

    bool operator==(DeviceInfo const& other) const
    {
        return type == other.type
            && device_handle == other.device_handle
            && label == other.label
            && dom_device_id == other.dom_device_id
            && group_id == other.group_id
            && sample_rate_hz == other.sample_rate_hz
            && channel_count == other.channel_count
            && channel_layout == other.channel_layout
            && is_default == other.is_default;
    }
};

// NOTE: RingHeader struct is a trivial struct for AnonymousBuffer mapping. Zero it.
struct RingHeader {
    u32 sample_rate_hz;
    u32 channel_count;
    u32 channel_capacity;
    u64 capacity_frames;
    u64 read_frame;
    u64 write_frame;
    u64 overrun_frames_total;
    u64 timeline_generation;
    u32 timeline_sample_rate;
    u64 timeline_media_start_frame;
    u64 timeline_media_start_at_ring_frame;
};

static_assert(sizeof(RingHeader) % alignof(float) == 0);

struct InputStreamDescriptor {
    u64 stream_id { 0 };
    u32 sample_rate_hz { 0 };
    u32 channel_count { 0 };
    u32 channel_capacity { 0 };
    u64 capacity_frames { 0 };
    Core::AnonymousBuffer shared_memory;
    IPC::File notify_fd;
};

struct TimingInfo;

class TimingReader {
public:
    struct Snapshot {
        u64 device_played_frames { 0 };
        u64 ring_read_frames { 0 };
        u64 server_monotonic_ns { 0 };
        u64 underrun_count { 0 };
    };

    TimingReader() = default;

    static ErrorOr<TimingReader> attach(Core::AnonymousBuffer buffer);

    bool is_valid() const { return m_storage != nullptr; }

    Optional<Snapshot> read_snapshot() const;

private:
    TimingReader(Core::AnonymousBuffer buffer, TimingInfo* storage)
        : m_buffer(move(buffer))
        , m_storage(storage)
    {
    }

    Core::AnonymousBuffer m_buffer;
    TimingInfo* m_storage { nullptr };
};

struct OutputSink {
    u64 session_id { 0 };
    u32 sample_rate { 0 };
    u32 channel_count { 0 };
    Audio::ChannelMap channel_layout;
    SharedCircularBuffer ring;
    TimingReader timing;
};

struct CreateClientRequest {
    ByteString origin { "*"sv };
    ByteString top_level_origin { "*"sv };
    bool can_use_mic { false };
};

struct CreateClientResponse {
    IPC::File socket;
    ByteString grant_id;
};

Audio::ChannelMap channel_map_from_layout(Vector<u8> const& channel_layout);
Audio::ChannelMap channel_map_by_count(u32 channel_count);

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Audio::ChannelMap const&);

template<>
ErrorOr<Audio::ChannelMap> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, AudioServer::DeviceInfo const&);

template<>
ErrorOr<AudioServer::DeviceInfo> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder& encoder, AudioServer::InputStreamDescriptor const& descriptor);

template<>
ErrorOr<AudioServer::InputStreamDescriptor> decode(Decoder& decoder);

template<>
ErrorOr<void> encode(Encoder& encoder, AudioServer::CreateClientRequest const& info);

template<>
ErrorOr<AudioServer::CreateClientRequest> decode(Decoder& decoder);

template<>
ErrorOr<void> encode(Encoder& encoder, AudioServer::CreateClientResponse const& info);

template<>
ErrorOr<AudioServer::CreateClientResponse> decode(Decoder& decoder);

}
