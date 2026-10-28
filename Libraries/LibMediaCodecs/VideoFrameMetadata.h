/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibGfx/Size.h>
#include <LibIPC/Forward.h>
#include <LibMediaCodecs/Color/CodingIndependentCodePoints.h>
#include <LibMediaCodecs/Export.h>
#include <LibMediaCodecs/Forward.h>
#include <LibMediaCodecs/Subsampling.h>
#include <LibMediaCodecs/VideoFrame.h>

namespace Media {

struct MEDIACODECS_API VideoFrameMetadata {
    static VideoFrameMetadata for_frame(VideoFrame const&);

    bool is_valid() const;

    AK::Duration timestamp;
    AK::Duration duration;
    Gfx::IntSize size;
    u8 bit_depth { 0 };
    Subsampling subsampling;
    CodingIndependentCodePoints cicp;
};

// How a frame's planes lay out within a pool slot's bytes, aligned for cache- and
// SIMD-friendly plane bases. Both the producer writing a decoded frame into a slot and a
// consumer reading the frame from the slot's buffer derive the same layout.
struct FramePlaneLayout {
    size_t y_size { 0 };
    size_t u_offset { 0 };
    size_t u_size { 0 };
    size_t v_offset { 0 };
    size_t v_size { 0 };
    size_t total_byte_count { 0 };
};

MEDIACODECS_API ErrorOr<FramePlaneLayout> frame_plane_layout(Gfx::IntSize, u8 bit_depth, Subsampling);

}

namespace IPC {

template<>
MEDIACODECS_API ErrorOr<void> encode(Encoder&, Media::VideoFrameMetadata const&);

template<>
MEDIACODECS_API ErrorOr<Media::VideoFrameMetadata> decode(Decoder&);

}
