/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <AK/Memory.h>
#include <AK/RefCounted.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <AudioServer/InputStream.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/System.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>

namespace AudioServer {

void InputStream::initialize_ring_header(RingHeader& header, u32 sample_rate_hz, u32 channel_count, u32 channel_capacity, u64 capacity_frames)
{
    header.sample_rate_hz = sample_rate_hz;
    header.channel_count = channel_count;
    header.channel_capacity = channel_capacity;
    header.capacity_frames = capacity_frames;
    header.overrun_frames_total = 0;
    header.timeline_generation = 1;
    header.timeline_sample_rate = 0;
    header.timeline_media_start_frame = 0;
    header.timeline_media_start_at_ring_frame = 0;

    AK::atomic_store(&header.read_frame, static_cast<u64>(0), AK::MemoryOrder::memory_order_release);
    AK::atomic_store(&header.write_frame, static_cast<u64>(0), AK::MemoryOrder::memory_order_release);
}

ErrorOr<void> InputStream::initialize_shared_ring_storage(u32 sample_rate_hz, u32 channel_count, u64 capacity_frames)
{
    if (sample_rate_hz == 0 || channel_count == 0 || capacity_frames == 0)
        return Error::from_string_literal("invalid ring stream format");

    u32 channel_capacity = channel_count;
    size_t total_bytes = ring_stream_bytes_total(channel_capacity, capacity_frames);
    m_descriptor.shared_memory = TRY(Core::AnonymousBuffer::create_with_size(total_bytes));

    auto* header = m_descriptor.shared_memory.data<RingHeader>();
    if (!header)
        return Error::from_string_literal("failed to map ring stream header");

    secure_zero(header, sizeof(RingHeader));
    initialize_ring_header(*header, sample_rate_hz, channel_count, channel_capacity, capacity_frames);

    m_view.header = header;
    auto* data = reinterpret_cast<float*>(header + 1);
    m_view.interleaved_frames = { data, ring_stream_bytes_for_data(channel_capacity, capacity_frames) / sizeof(float) };

    m_descriptor.stream_id = 0;
    m_descriptor.sample_rate_hz = sample_rate_hz;
    m_descriptor.channel_count = channel_count;
    m_descriptor.channel_capacity = channel_capacity;
    m_descriptor.capacity_frames = capacity_frames;
    TRY(create_notify_pipe());
    return {};
}

size_t InputStream::try_push_interleaved(ReadonlySpan<float> interleaved_samples, u32 input_channel_count)
{
    RingView view = m_view;
    if (!view.header)
        return 0;
    if (input_channel_count == 0)
        return 0;

    size_t input_frame_count = interleaved_samples.size() / input_channel_count;
    if (input_frame_count == 0)
        return 0;

    RingHeader& header = *view.header;
    u64 capacity_frames = header.capacity_frames;
    u32 channel_capacity = header.channel_capacity;
    if (capacity_frames == 0 || channel_capacity == 0)
        return 0;

    u64 read_frame = AK::atomic_load(&header.read_frame, AK::MemoryOrder::memory_order_acquire);
    u64 write_frame = AK::atomic_load(&header.write_frame, AK::MemoryOrder::memory_order_acquire);
    u64 used = write_frame > read_frame ? (write_frame - read_frame) : 0;
    if (used > capacity_frames) {
        read_frame = write_frame - capacity_frames;
        AK::atomic_store(&header.read_frame, read_frame, AK::MemoryOrder::memory_order_release);
        used = capacity_frames;
    }

    size_t frames_to_write = input_frame_count;
    size_t skipped_frames = 0;
    if (frames_to_write > capacity_frames) {
        skipped_frames = frames_to_write - static_cast<size_t>(capacity_frames);
        frames_to_write = static_cast<size_t>(capacity_frames);
    }

    size_t available = used < capacity_frames ? static_cast<size_t>(capacity_frames - used) : 0;
    if (frames_to_write > available) {
        size_t drop = frames_to_write - available;
        read_frame += drop;
        AK::atomic_store(&header.read_frame, read_frame, AK::MemoryOrder::memory_order_release);
    }

    size_t channel_capacity_size = static_cast<size_t>(channel_capacity);
    size_t start_frame_index = static_cast<size_t>(write_frame % capacity_frames);
    size_t first_chunk_frames = min(frames_to_write, static_cast<size_t>(capacity_frames - start_frame_index));
    size_t second_chunk_frames = frames_to_write - first_chunk_frames;

    ReadonlySpan<float> input = interleaved_samples.slice(skipped_frames * input_channel_count);
    size_t input_offset = 0;

    size_t first_base = start_frame_index * channel_capacity_size;
    for (size_t i = 0; i < first_chunk_frames; ++i) {
        float* dst = view.interleaved_frames.data() + first_base + (i * channel_capacity_size);
        for (u32 ch = 0; ch < channel_capacity; ++ch) {
            float value = 0.0f;
            if (ch < input_channel_count)
                value = input[input_offset + ch];
            dst[ch] = value;
        }
        input_offset += input_channel_count;
    }

    for (size_t i = 0; i < second_chunk_frames; ++i) {
        float* dst = view.interleaved_frames.data() + (i * channel_capacity_size);
        for (u32 ch = 0; ch < channel_capacity; ++ch) {
            float value = 0.0f;
            if (ch < input_channel_count)
                value = input[input_offset + ch];
            dst[ch] = value;
        }
        input_offset += input_channel_count;
    }

    AK::atomic_store(&header.write_frame, write_frame + frames_to_write, AK::MemoryOrder::memory_order_release);
    return frames_to_write;
}

InputStream::~InputStream()
{
    if (m_notify_write_fd >= 0)
        (void)Core::System::close(m_notify_write_fd);
}

ErrorOr<InputStreamDescriptor> InputStream::descriptor_for_ipc() const
{
    auto notify_fd = TRY(IPC::File::clone_fd(m_descriptor.notify_fd.fd()));
    return InputStreamDescriptor {
        .stream_id = m_descriptor.stream_id,
        .sample_rate_hz = m_descriptor.sample_rate_hz,
        .channel_count = m_descriptor.channel_count,
        .channel_capacity = m_descriptor.channel_capacity,
        .capacity_frames = m_descriptor.capacity_frames,
        .shared_memory = m_descriptor.shared_memory,
        .notify_fd = move(notify_fd),
    };
}

ErrorOr<void> InputStream::create_notify_pipe()
{
    auto pipe_fds_or_error = Core::System::pipe2(O_CLOEXEC);
    if (pipe_fds_or_error.is_error())
        return pipe_fds_or_error.release_error();

    int read_fd = pipe_fds_or_error.value()[0];
    int write_fd = pipe_fds_or_error.value()[1];
    m_descriptor.notify_fd = IPC::File::adopt_fd(read_fd);
    m_notify_write_fd = write_fd;
    return {};
}

}
