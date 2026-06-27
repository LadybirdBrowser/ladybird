/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibGfx/YUVData.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMedia/VideoFrame.h>
#include <LibMedia/VideoFramePool.h>

#include "VideoFrameHandle.h"

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

VideoFrameHandle VideoFrameHandle::for_frame(VideoFrame const& frame)
{
    // Owner-backed (a held pool slot) or consumer-backed (resolved from a handle); both carry the slot identity.
    VideoFramePoolID pool_id { 0 };
    u32 slot_index = 0;
    u64 slot_acquisition_id = 0;
    if (auto const* pool_slot = frame.pool_slot()) {
        pool_id = pool_slot->pool().id();
        slot_index = pool_slot->slot_index();
        slot_acquisition_id = pool_slot->slot_acquisition_id();
    } else {
        auto const* resolved_slot = frame.resolved_slot();
        VERIFY(resolved_slot != nullptr);
        pool_id = resolved_slot->pool_id();
        slot_index = resolved_slot->slot_index();
        slot_acquisition_id = resolved_slot->slot_acquisition_id();
    }

    auto const& yuv_data = frame.yuv_data();
    return VideoFrameHandle {
        .pool_id = pool_id,
        .slot_index = slot_index,
        .slot_acquisition_id = slot_acquisition_id,
        .timestamp = frame.timestamp(),
        .duration = frame.duration(),
        .size = yuv_data.size(),
        .bit_depth = yuv_data.bit_depth(),
        .subsampling = yuv_data.subsampling(),
        .cicp = yuv_data.cicp(),
    };
}

}

namespace IPC {

static bool color_primaries_ipc_value_valid(Media::ColorPrimaries color_primaries)
{
    return color_primaries == Media::ColorPrimaries::Unspecified
        || Media::color_primaries_valid(color_primaries);
}

static bool transfer_characteristics_ipc_value_valid(Media::TransferCharacteristics transfer_characteristics)
{
    return transfer_characteristics == Media::TransferCharacteristics::Unspecified
        || Media::transfer_characteristics_valid(transfer_characteristics);
}

static bool matrix_coefficients_ipc_value_valid(Media::MatrixCoefficients matrix_coefficients)
{
    return matrix_coefficients == Media::MatrixCoefficients::Unspecified
        || Media::matrix_coefficients_valid(matrix_coefficients);
}

static bool video_full_range_flag_ipc_value_valid(Media::VideoFullRangeFlag video_full_range_flag)
{
    return video_full_range_flag == Media::VideoFullRangeFlag::Unspecified
        || Media::video_full_range_flag_valid(video_full_range_flag);
}

template<>
ErrorOr<void> encode(Encoder& encoder, Media::VideoFrameHandle const& handle)
{
    TRY(encoder.encode(handle.pool_id));
    TRY(encoder.encode(handle.slot_index));
    TRY(encoder.encode(handle.slot_acquisition_id));
    TRY(encoder.encode(handle.timestamp));
    TRY(encoder.encode(handle.duration));
    TRY(encoder.encode(handle.size));
    TRY(encoder.encode(handle.bit_depth));
    TRY(encoder.encode(handle.subsampling.x()));
    TRY(encoder.encode(handle.subsampling.y()));
    TRY(encoder.encode(handle.cicp.color_primaries()));
    TRY(encoder.encode(handle.cicp.transfer_characteristics()));
    TRY(encoder.encode(handle.cicp.matrix_coefficients()));
    TRY(encoder.encode(handle.cicp.video_full_range_flag()));
    return {};
}

template<>
ErrorOr<Media::VideoFrameHandle> decode(Decoder& decoder)
{
    Media::VideoFrameHandle handle;
    handle.pool_id = TRY(decoder.decode<Media::VideoFramePoolID>());
    handle.slot_index = TRY(decoder.decode<u32>());
    handle.slot_acquisition_id = TRY(decoder.decode<u64>());
    handle.timestamp = TRY(decoder.decode<AK::Duration>());
    handle.duration = TRY(decoder.decode<AK::Duration>());
    handle.size = TRY(decoder.decode<Gfx::IntSize>());
    handle.bit_depth = TRY(decoder.decode<u8>());
    handle.subsampling = Media::Subsampling {
        TRY(decoder.decode<bool>()),
        TRY(decoder.decode<bool>()),
    };
    handle.cicp = Media::CodingIndependentCodePoints {
        TRY(decoder.decode<Media::ColorPrimaries>()),
        TRY(decoder.decode<Media::TransferCharacteristics>()),
        TRY(decoder.decode<Media::MatrixCoefficients>()),
        TRY(decoder.decode<Media::VideoFullRangeFlag>()),
    };

    if (!color_primaries_ipc_value_valid(handle.cicp.color_primaries())
        || !transfer_characteristics_ipc_value_valid(handle.cicp.transfer_characteristics())
        || !matrix_coefficients_ipc_value_valid(handle.cicp.matrix_coefficients())
        || !video_full_range_flag_ipc_value_valid(handle.cicp.video_full_range_flag()))
        return Error::from_string_literal("IPC: VideoFrameHandle contained invalid CICP metadata");

    return handle;
}

}
