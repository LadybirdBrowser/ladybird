/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Error.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <LibAudioServer/LibAudioServer.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>

namespace Web::WebAudio::Render {

using StreamID = u64;

using AudioServer::RingHeader;

static_assert(sizeof(RingHeader) % alignof(f32) == 0);

struct RingStreamView {
    RingHeader* header { nullptr };
    Span<f32> interleaved_frames;
};

struct RingStreamFormat {
    u32 sample_rate_hz { 0 };
    u32 channel_count { 0 };
    u32 channel_capacity { 0 };
    u64 capacity_frames { 0 };
};

struct RingStreamDescriptor {
    StreamID stream_id { 0 };
    RingStreamFormat format;
    Core::AnonymousBuffer shared_memory;
    IPC::File notify_fd;
};

struct AudioInputStreamMetadata {
    AudioServer::DeviceHandle device_handle { 0 };
    u32 sample_rate_hz { 0 };
    u32 channel_count { 0 };
    u64 capacity_frames { 0 };
};

struct MediaElementAudioSourceStreamDescriptor {
    u64 provider_id { 0 };
    RingStreamDescriptor ring_stream;
};

struct MediaStreamAudioSourceStreamDescriptor {
    u64 provider_id { 0 };
    AudioInputStreamMetadata metadata;
};

struct PacketStreamDescriptor {
    StreamID stream_id { 0 };
    IPC::File notify_fd;
};

struct SharedBufferStreamDescriptor {
    Core::AnonymousBuffer pool_buffer;
    Core::AnonymousBuffer ready_ring_buffer;
    Core::AnonymousBuffer free_ring_buffer;
};

struct ScriptProcessorStreamDescriptor {
    u64 node_id { 0 };
    u32 buffer_size { 0 };
    u32 input_channel_count { 0 };
    u32 output_channel_count { 0 };

    SharedBufferStreamDescriptor request_stream;
    SharedBufferStreamDescriptor response_stream;

    IPC::File request_notify_write_fd;
};

struct WorkletNodePortDescriptor {
    u64 node_id { 0 };
    IPC::File processor_port_fd;
};

struct AnalyserFeedbackHeader {
    u32 fft_size;
    u64 analyser_node_id;
    u64 rendered_frames_total;
};

static_assert(sizeof(AnalyserFeedbackHeader) % alignof(f32) == 0);

struct CompressorFeedbackPage {
    u64 compressor_node_id;
    u64 rendered_frames_total;
    f32 reduction_db;
};

static_assert(sizeof(CompressorFeedbackPage) % alignof(f32) == 0);

struct TimingFeedbackPage {
    u32 sequence;
    u32 sample_rate_hz;
    u32 channel_count;
    u64 rendered_frames_total;
    u64 underrun_frames_total;
    u64 graph_generation;
    u32 is_suspended;
    u64 suspend_generation;
};

inline size_t analyser_feedback_page_size(u32 fft_size)
{
    // Payload: header + time-domain floats (fft_size) + frequency dB floats (fft_size / 2).
    return sizeof(AnalyserFeedbackHeader)
        + (static_cast<size_t>(fft_size) * sizeof(f32))
        + (static_cast<size_t>(fft_size) / 2 * sizeof(f32));
}

inline u64 ring_stream_load_read_frame(RingHeader& header)
{
    return AK::atomic_load(&header.read_frame, AK::MemoryOrder::memory_order_acquire);
}

inline u64 ring_stream_load_write_frame(RingHeader& header)
{
    return AK::atomic_load(&header.write_frame, AK::MemoryOrder::memory_order_acquire);
}

inline void ring_stream_store_read_frame(RingHeader& header, u64 value)
{
    AK::atomic_store(&header.read_frame, value, AK::MemoryOrder::memory_order_release);
}

inline void ring_stream_store_write_frame(RingHeader& header, u64 value)
{
    AK::atomic_store(&header.write_frame, value, AK::MemoryOrder::memory_order_release);
}

inline size_t ring_stream_available_frames(RingHeader const& header, u64 read_frame, u64 write_frame)
{
    if (write_frame <= read_frame)
        return 0;
    u64 available = write_frame - read_frame;
    available = available < header.capacity_frames ? available : header.capacity_frames;
    return static_cast<size_t>(available);
}

inline bool ring_stream_consumer_detect_and_fix_overrun(RingHeader& header, u64& in_out_read_frame, u64 write_frame)
{
    if (write_frame <= in_out_read_frame)
        return false;

    u64 unread = write_frame - in_out_read_frame;
    if (unread <= header.capacity_frames)
        return false;

    u64 new_read = write_frame - header.capacity_frames;
    u64 dropped = new_read - in_out_read_frame;

    header.overrun_frames_total += dropped;
    in_out_read_frame = new_read;
    ring_stream_store_read_frame(header, new_read);
    return true;
}

bool try_signal_event_fd(int fd);
void drain_event_fd(int fd);
void write_timing_page(TimingFeedbackPage& page, u32 sample_rate_hz, u32 channel_count, u64 rendered_frames_total, u64 underrun_frames_total, u64 graph_generation, bool is_suspended, u64 suspend_generation);
bool read_timing_page(TimingFeedbackPage const& page, TimingFeedbackPage& out);

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::WebAudio::Render::RingStreamFormat const&);

template<>
ErrorOr<Web::WebAudio::Render::RingStreamFormat> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Web::WebAudio::Render::RingStreamDescriptor const&);

template<>
ErrorOr<Web::WebAudio::Render::RingStreamDescriptor> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Web::WebAudio::Render::AudioInputStreamMetadata const&);

template<>
ErrorOr<Web::WebAudio::Render::AudioInputStreamMetadata> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor const&);

template<>
ErrorOr<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor const&);

template<>
ErrorOr<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Web::WebAudio::Render::SharedBufferStreamDescriptor const&);

template<>
ErrorOr<Web::WebAudio::Render::SharedBufferStreamDescriptor> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Web::WebAudio::Render::ScriptProcessorStreamDescriptor const&);

template<>
ErrorOr<Web::WebAudio::Render::ScriptProcessorStreamDescriptor> decode(Decoder&);

template<>
ErrorOr<void> encode(Encoder&, Web::WebAudio::Render::WorkletNodePortDescriptor const&);

template<>
ErrorOr<Web::WebAudio::Render::WorkletNodePortDescriptor> decode(Decoder&);

}
