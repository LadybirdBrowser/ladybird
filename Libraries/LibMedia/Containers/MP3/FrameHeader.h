/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibMedia/Export.h>

namespace Media::MP3 {

struct FrameHeader {
    static constexpr size_t SIZE = 4;
    static constexpr u8 SYNC_CODE_BIT_COUNT = 11;
    static constexpr u16 SYNC_CODE = 0b111'1111'1111;

    u8 layer { 0 };
    u32 sample_rate { 0 };
    u8 channel_count { 0 };
    u16 sample_count { 0 };
    u16 frame_byte_size { 0 };

    static MEDIA_API Optional<FrameHeader> parse(ReadonlyBytes);
};

}
