/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Types.h>
#include <LibWeb/WebAudio/Engine/StreamTransport.h>
#include <LibWeb/WebAudio/Engine/StreamTransportDescriptors.h>

namespace Web::WebAudio::Render {

using ValidatedRingStream = RingStreamView;

inline ErrorOr<ValidatedRingStream> validate_ring_stream_descriptor(RingStreamDescriptor& descriptor)
{
    if (descriptor.stream_id == 0)
        return Error::from_string_literal("RingStream: invalid stream id");

    if (!descriptor.shared_memory.is_valid())
        return Error::from_string_literal("RingStream: missing shared memory");

    if (descriptor.shared_memory.size() < sizeof(RingStreamHeader))
        return Error::from_string_literal("RingStream: shared memory too small for header");

    RingStreamHeader* header = descriptor.shared_memory.data<RingStreamHeader>();
    if (!header)
        return Error::from_string_literal("RingStream: shared memory header mapping failed");

    if (header->version != ring_stream_version)
        return Error::from_string_literal("RingStream: unsupported header version");

    if (header->capacity_frames == 0 || header->channel_capacity == 0)
        return Error::from_string_literal("RingStream: invalid capacity");

    if (header->channel_count == 0)
        return Error::from_string_literal("RingStream: invalid channel count");

    if (header->channel_count > header->channel_capacity)
        return Error::from_string_literal("RingStream: channel_count exceeds channel_capacity");

    // If the descriptor includes a nonzero format, require it to match the shared header.
    if (descriptor.format.sample_rate_hz != 0 && descriptor.format.sample_rate_hz != header->sample_rate_hz)
        return Error::from_string_literal("RingStream: descriptor sample rate does not match shared header");
    if (descriptor.format.channel_count != 0 && descriptor.format.channel_count != header->channel_count)
        return Error::from_string_literal("RingStream: descriptor channel count does not match shared header");
    if (descriptor.format.channel_capacity != 0 && descriptor.format.channel_capacity != header->channel_capacity)
        return Error::from_string_literal("RingStream: descriptor channel capacity does not match shared header");
    if (descriptor.format.capacity_frames != 0 && descriptor.format.capacity_frames != header->capacity_frames)
        return Error::from_string_literal("RingStream: descriptor capacity does not match shared header");

    size_t const required_bytes = ring_stream_bytes_total(header->channel_capacity, header->capacity_frames);
    if (descriptor.shared_memory.size() < required_bytes)
        return Error::from_string_literal("RingStream: shared memory too small for ring data");

    u8* base = descriptor.shared_memory.data<u8>();
    if (!base)
        return Error::from_string_literal("RingStream: shared memory base mapping failed");

    size_t const data_bytes = ring_stream_bytes_for_data(header->channel_capacity, header->capacity_frames);
    auto* data_f32 = reinterpret_cast<f32*>(base + sizeof(RingStreamHeader));
    Span<f32> data { data_f32, data_bytes / sizeof(f32) };

    return ValidatedRingStream { .header = header, .interleaved_frames = data };
}

}
