/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Media {

static constexpr u32 MAX_VIDEO_FRAME_DIMENSION = 32767;
static constexpr u64 MAX_VIDEO_FRAME_AREA = 1ull << 29;

constexpr bool video_frame_exceeds_limits(u64 width, u64 height)
{
    return width > MAX_VIDEO_FRAME_DIMENSION || height > MAX_VIDEO_FRAME_DIMENSION || width * height > MAX_VIDEO_FRAME_AREA;
}

}
