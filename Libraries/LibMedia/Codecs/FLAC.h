/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibAudio/ChannelMap.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMediaCodecs/DecoderError.h>

namespace Media::Codecs {

class FLAC {
public:
    struct FrameInfo {
        u64 sample_number;
        u16 block_size;
    };

    static MEDIA_API DecoderErrorOr<FixedArray<u8>> codec_initialization_data_from_isobmff_configuration(ReadonlyBytes);
    static bool is_sync_code(u16);
    static DecoderErrorOr<Optional<FrameInfo>> parse_frame_header(MediaStreamCursor&, u16 sync_code, u16 fixed_block_size);
    // The CRC a frame ends with, over every byte of the frame before it.
    static u16 compute_frame_crc(ReadonlyBytes);
    // The format assigns these positions to the channels of each count.
    static MEDIA_API Audio::ChannelMap channel_map(u8 channel_count);
};

}
