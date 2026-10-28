/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibGfx/YUVData.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMediaCodecs/Limits.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibMediaCodecs/VideoFramePool.h>

#include "VideoFrameMetadata.h"

namespace Media {

ErrorOr<FramePlaneLayout> frame_plane_layout(Gfx::IntSize size, u8 bit_depth, Subsampling subsampling)
{
    constexpr size_t PLANE_ALIGNMENT = 64;

    auto plane_sizes = TRY(Gfx::YUVData::plane_sizes(size, bit_depth, subsampling));

    auto u_offset = align_up_to(plane_sizes.y, PLANE_ALIGNMENT);
    auto v_offset = align_up_to(u_offset + plane_sizes.u, PLANE_ALIGNMENT);
    Checked<size_t> checked_total_byte_count = v_offset;
    checked_total_byte_count += plane_sizes.v;
    if (checked_total_byte_count.has_overflow())
        return Error::from_string_literal("Video frame plane layout overflows");

    return FramePlaneLayout {
        .y_size = plane_sizes.y,
        .u_offset = u_offset,
        .u_size = plane_sizes.u,
        .v_offset = v_offset,
        .v_size = plane_sizes.v,
        .total_byte_count = checked_total_byte_count.value(),
    };
}

bool VideoFrameMetadata::is_valid() const
{
    if (duration.is_negative())
        return false;
    if (bit_depth != 8 && bit_depth != 10 && bit_depth != 12)
        return false;
    if (size.width() < 1 || size.height() < 1)
        return false;
    if (static_cast<u32>(size.width()) > MAX_VIDEO_FRAME_DIMENSION || static_cast<u32>(size.height()) > MAX_VIDEO_FRAME_DIMENSION)
        return false;
    if (static_cast<u64>(size.width()) * static_cast<u64>(size.height()) > MAX_VIDEO_FRAME_AREA)
        return false;
    return cicp.is_valid_or_unspecified();
}

VideoFrameMetadata VideoFrameMetadata::for_frame(VideoFrame const& frame)
{
    return VideoFrameMetadata {
        .timestamp = frame.timestamp(),
        .duration = frame.duration(),
        .size = frame.size().to_type<int>(),
        .bit_depth = frame.bit_depth(),
        .subsampling = frame.subsampling(),
        .cicp = frame.cicp(),
    };
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Media::VideoFrameMetadata const& metadata)
{
    TRY(encoder.encode(metadata.timestamp));
    TRY(encoder.encode(metadata.duration));
    TRY(encoder.encode(metadata.size));
    TRY(encoder.encode(metadata.bit_depth));
    TRY(encoder.encode(metadata.subsampling.x()));
    TRY(encoder.encode(metadata.subsampling.y()));
    TRY(encoder.encode(metadata.cicp));
    return {};
}

template<>
ErrorOr<Media::VideoFrameMetadata> decode(Decoder& decoder)
{
    Media::VideoFrameMetadata metadata;
    metadata.timestamp = TRY(decoder.decode<AK::Duration>());
    metadata.duration = TRY(decoder.decode<AK::Duration>());
    metadata.size = TRY(decoder.decode<Gfx::IntSize>());
    metadata.bit_depth = TRY(decoder.decode<u8>());
    metadata.subsampling = Media::Subsampling {
        TRY(decoder.decode<bool>()),
        TRY(decoder.decode<bool>()),
    };
    metadata.cicp = TRY(decoder.decode<Media::CodingIndependentCodePoints>());
    if (!metadata.is_valid())
        return Error::from_string_literal("Video frame metadata describes a frame that could not have been decoded");
    return metadata;
}

}
