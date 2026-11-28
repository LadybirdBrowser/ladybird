/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibIPC/Forward.h>

namespace Camera {

// What a stream would like from its camera, which gives the closest it can do. Zero is no preference.
struct StreamRequest {
    u32 width { 0 };
    u32 height { 0 };
    u32 frame_rate { 0 };
};

static constexpr u32 MAXIMUM_PICTURE_DIMENSION = 4096;

// The size of an I420 picture: a plane of luma and two of chroma at half the resolution each way.
constexpr size_t i420_size(u32 width, u32 height)
{
    auto chroma_width = (static_cast<size_t>(width) + 1) / 2;
    auto chroma_height = (static_cast<size_t>(height) + 1) / 2;
    return static_cast<size_t>(width) * height + 2 * chroma_width * chroma_height;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Camera::StreamRequest const&);

template<>
ErrorOr<Camera::StreamRequest> decode(Decoder&);

}
