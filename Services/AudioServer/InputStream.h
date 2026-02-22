/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/RefCounted.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibAudioServer/LibAudioServer.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/System.h>
#include <LibIPC/File.h>

namespace AudioServer {

class InputStream : public RefCounted<InputStream> {
public:
    virtual ~InputStream();

    InputStreamDescriptor const& descriptor() const { return m_descriptor; }
    ErrorOr<InputStreamDescriptor> descriptor_for_ipc() const;

    void set_stream_id(u64 id) { m_descriptor.stream_id = id; }
    u32 channel_count() const { return m_descriptor.channel_count; }

protected:
    using RingHeader = AudioServer::RingHeader;

    struct RingView {
        RingHeader* header { nullptr };
        Span<float> interleaved_frames;
    };

    ErrorOr<void> initialize_shared_ring_storage(u32 sample_rate_hz, u32 channel_count, u64 capacity_frames);
    size_t try_push_interleaved(ReadonlySpan<float> interleaved_samples, u32 input_channel_count);
    ErrorOr<void> create_notify_pipe();

    InputStreamDescriptor m_descriptor;
    RingView m_view;

private:
    static size_t ring_stream_bytes_for_data(u32 channel_capacity, u64 capacity_frames)
    {
        return static_cast<size_t>(channel_capacity) * static_cast<size_t>(capacity_frames) * sizeof(float);
    }

    static size_t ring_stream_bytes_total(u32 channel_capacity, u64 capacity_frames)
    {
        return sizeof(RingHeader) + ring_stream_bytes_for_data(channel_capacity, capacity_frames);
    }

    static void initialize_ring_header(RingHeader& header, u32 sample_rate_hz, u32 channel_count, u32 channel_capacity, u64 capacity_frames);

    int m_notify_write_fd { -1 };
};

ErrorOr<NonnullRefPtr<InputStream>> create_platform_input_stream(DeviceHandle device_handle, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames);

}
