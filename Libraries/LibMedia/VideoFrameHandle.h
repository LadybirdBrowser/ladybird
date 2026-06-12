/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Time.h>
#include <LibGfx/Size.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Subsampling.h>

namespace Media {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, VideoFramePoolID);

// References a frame in a previously-announced pool slot buffer, along with the metadata needed
// to present it. The receiving process resolves it against its mapping of that slot's buffer,
// validating the slot acquisition ID.
struct MEDIA_API VideoFrameHandle {
    static VideoFrameHandle for_frame(VideoFrame const&);

    VideoFramePoolID pool_id { 0 };
    u32 slot_index { 0 };
    u64 slot_acquisition_id { 0 };
    AK::Duration timestamp;
    AK::Duration duration;
    Gfx::IntSize size;
    u8 bit_depth { 0 };
    Subsampling subsampling;
    CodingIndependentCodePoints cicp;
};

// How a frame's planes lay out within a pool slot's bytes, aligned for cache- and
// SIMD-friendly plane bases. Both the producer writing a decoded frame into a slot and a
// consumer resolving a handle against the slot's buffer derive the same layout.
struct FramePlaneLayout {
    size_t y_size { 0 };
    size_t u_offset { 0 };
    size_t u_size { 0 };
    size_t v_offset { 0 };
    size_t v_size { 0 };
    size_t total_byte_count { 0 };
};

MEDIA_API ErrorOr<FramePlaneLayout> frame_plane_layout(Gfx::IntSize, u8 bit_depth, Subsampling);

}

namespace IPC {

template<>
MEDIA_API ErrorOr<void> encode(Encoder&, Media::VideoFrameHandle const&);

template<>
MEDIA_API ErrorOr<Media::VideoFrameHandle> decode(Decoder&);

}
