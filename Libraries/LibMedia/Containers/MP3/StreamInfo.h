/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibMedia/Export.h>

namespace Media::MP3 {

struct FrameHeader;

// The Xing, Info and VBRI tags that encoders place in the first frame to describe the whole stream.
struct StreamInfo {
    static constexpr size_t SEEK_TABLE_ENTRY_COUNT = 100;

    Optional<u32> frame_count;
    Optional<u32> byte_count;
    Optional<Array<u8, SEEK_TABLE_ENTRY_COUNT>> seek_table;
    u16 encoder_delay { 0 };
    u16 encoder_padding { 0 };

    static MEDIA_API Optional<StreamInfo> parse(ReadonlyBytes frame, FrameHeader const&);

    MEDIA_API Optional<u64> total_sample_count(FrameHeader const&) const;
};

}
