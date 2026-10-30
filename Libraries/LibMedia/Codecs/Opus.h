/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/Span.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMediaCodecs/DecoderError.h>

namespace Media::Codecs {

class Opus {
public:
    static MEDIA_API DecoderErrorOr<FixedArray<u8>> codec_initialization_data_from_isobmff_configuration(ReadonlyBytes);
    static DecoderErrorOr<u32> parse_frame_duration_in_samples(MediaStreamCursor&, size_t frame_size);

    class PacketDurationParser {
    public:
        static u32 samples_per_second() { return 48'000; }
        static DecoderErrorOr<u32> parse_duration_in_samples(MediaStreamCursor& cursor, size_t packet_size) { return parse_frame_duration_in_samples(cursor, packet_size); }
        static void reset() { }
    };
};

}
