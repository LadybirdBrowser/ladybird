/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibMedia/Export.h>

namespace Media::ADTS {

enum class MPEGVersion : u8 {
    Version2,
    Version4,
};

struct FrameHeader {
    static constexpr size_t SIZE = 7;
    static constexpr size_t CRC_SIZE = 2;
    static constexpr u8 SYNC_CODE_BIT_COUNT = 12;
    static constexpr u16 SYNC_CODE = 0b1111'1111'1111;
    static constexpr u16 SAMPLES_PER_BLOCK = 1024;

    MPEGVersion version { MPEGVersion::Version4 };
    u8 audio_object_type { 0 };
    bool has_crc { false };
    u32 sample_rate { 0 };
    u8 channel_count { 0 };
    u8 block_count { 1 };
    u16 sample_count { 0 };
    u16 frame_byte_size { 0 };

    // A frame carrying more than one block also carries the position of each one, so the frames a
    // reader can walk are the ones it can find the audio in.
    size_t payload_offset() const { return has_crc ? SIZE + CRC_SIZE : SIZE; }

    static constexpr bool has_sync_code(u16 value)
    {
        return (value >> (16 - SYNC_CODE_BIT_COUNT)) == SYNC_CODE;
    }

    static MEDIA_API Optional<FrameHeader> parse(ReadonlyBytes);
};

}
