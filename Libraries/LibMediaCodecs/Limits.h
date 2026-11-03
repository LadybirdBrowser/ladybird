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

static constexpr u32 MIN_AUDIO_SAMPLE_RATE = 3000;
static constexpr u32 MAX_AUDIO_SAMPLE_RATE = 768000;

constexpr bool audio_sample_rate_is_outside_limits(u64 sample_rate)
{
    return sample_rate < MIN_AUDIO_SAMPLE_RATE || sample_rate > MAX_AUDIO_SAMPLE_RATE;
}

static constexpr size_t MAX_DECODED_AUDIO_BYTE_COUNT = 512 * MiB;

static constexpr size_t MAX_METADATA_STRING_SIZE = 16 * KiB;
static constexpr size_t MAX_CODEC_CONFIGURATION_SIZE = 16 * MiB;

}
