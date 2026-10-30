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
    static constexpr size_t STREAM_INFO_SIZE = 34;

    struct StreamInfo {
        u16 minimum_block_size { 0 };
        u16 maximum_block_size { 0 };
        u32 minimum_frame_size { 0 };
        u32 maximum_frame_size { 0 };
        u32 sample_rate { 0 };
        u8 channel_count { 0 };
        u8 bits_per_sample { 0 };
        // Zero when the stream does not say.
        u64 total_sample_count { 0 };
    };

    struct FrameInfo {
        u64 sample_number;
        u16 block_size;
    };

    static MEDIA_API DecoderErrorOr<FixedArray<u8>> codec_initialization_data_from_isobmff_configuration(ReadonlyBytes);
    static DecoderErrorOr<StreamInfo> parse_stream_info(ReadonlyBytes);
    static bool is_sync_code(u16);
    static DecoderErrorOr<Optional<FrameInfo>> parse_frame_header(MediaStreamCursor&, u16 sync_code, u16 fixed_block_size);
    // The CRC a frame ends with, over every byte of the frame before it.
    static u16 compute_frame_crc(ReadonlyBytes);
    // The format assigns these positions to the channels of each count.
    static MEDIA_API Audio::ChannelMap channel_map(u8 channel_count);

    class PacketDurationParser {
    public:
        static DecoderErrorOr<PacketDurationParser> create(ReadonlyBytes codec_initialization_data);

        u32 samples_per_second() const { return m_samples_per_second; }
        static DecoderErrorOr<u32> parse_duration_in_samples(MediaStreamCursor&, size_t packet_size);
        static void reset() { }

    private:
        explicit PacketDurationParser(u32 samples_per_second)
            : m_samples_per_second(samples_per_second)
        {
        }

        u32 m_samples_per_second { 0 };
    };
};

}
