/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Span.h>
#include <LibMedia/Forward.h>
#include <LibMediaCodecs/DecoderError.h>

namespace Media::Codecs {

class Vorbis {
public:
    class PacketDurationParser {
    public:
        static DecoderErrorOr<PacketDurationParser> create(ReadonlyBytes codec_initialization_data);

        u32 samples_per_second() const { return m_samples_per_second; }
        // A packet's duration depends on the block size of the packet before it.
        DecoderErrorOr<u32> parse_duration_in_samples(MediaStreamCursor&, size_t packet_size);
        void reset();

    private:
        PacketDurationParser() = default;

        u32 m_samples_per_second { 0 };
        Array<u16, 2> m_block_sizes {};
        u8 m_mode_count { 0 };
        u8 m_mode_bit_count { 0 };
        u64 m_long_block_modes { 0 };
        u16 m_previous_block_size { 0 };
    };
};

}
