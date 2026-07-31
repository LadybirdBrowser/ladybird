/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
#include <AK/MemoryStream.h>
#include <LibMedia/Codecs/Opus.h>
#include <LibMedia/MediaStream.h>
#include <LibMedia/ReadonlyBytesCursor.h>

namespace Media::Codecs {

DecoderErrorOr<FixedArray<u8>> Opus::codec_initialization_data_from_isobmff_configuration(ReadonlyBytes configuration)
{
    constexpr size_t minimum_configuration_size = 11;
    if (configuration.size() < minimum_configuration_size)
        return DecoderError::corrupted("Opus configuration is too small"sv);

    auto cursor = make_ref_counted<ReadonlyBytesCursor>(configuration);
    auto version = DECODER_TRY(DecoderErrorCategory::Corrupted, cursor->read_value<u8>());
    if (version == 0) {
        auto channel_count = DECODER_TRY(DecoderErrorCategory::Corrupted, cursor->read_value<u8>());
        auto pre_skip = DECODER_TRY(DecoderErrorCategory::Corrupted, cursor->read_value<u16>());
        auto input_sample_rate = DECODER_TRY(DecoderErrorCategory::Corrupted, cursor->read_value<u32>());
        auto output_gain = DECODER_TRY(DecoderErrorCategory::Corrupted, cursor->read_value<i16>());
        auto channel_mapping_family = DECODER_TRY(DecoderErrorCategory::Corrupted, cursor->read_value<u8>());

        auto signature = "OpusHead"sv.bytes();
        auto initialization_data = DECODER_TRY_ALLOC(FixedArray<u8>::create(signature.size() + configuration.size()));
        FixedMemoryStream writer { initialization_data.span() };
        DECODER_TRY(DecoderErrorCategory::Corrupted, writer.write_until_depleted(signature));
        DECODER_TRY(DecoderErrorCategory::Corrupted, writer.write_value(static_cast<u8>(1)));
        DECODER_TRY(DecoderErrorCategory::Corrupted, writer.write_value(channel_count));
        DECODER_TRY(DecoderErrorCategory::Corrupted, writer.write_value(LittleEndian<u16> { pre_skip }));
        DECODER_TRY(DecoderErrorCategory::Corrupted, writer.write_value(LittleEndian<u32> { input_sample_rate }));
        DECODER_TRY(DecoderErrorCategory::Corrupted, writer.write_value(LittleEndian<i16> { output_gain }));
        DECODER_TRY(DecoderErrorCategory::Corrupted, writer.write_value(channel_mapping_family));
        DECODER_TRY(DecoderErrorCategory::Corrupted, writer.write_until_depleted(configuration.slice(cursor->position())));
        return initialization_data;
    }

    return DecoderError::corrupted("Opus configuration has an unsupported version"sv);
}

DecoderErrorOr<u32> Opus::parse_frame_duration_in_samples(MediaStreamCursor& cursor, size_t frame_size)
{
    if (frame_size == 0)
        return DecoderError::corrupted("Opus frame is too small"sv);

    // https://datatracker.ietf.org/doc/html/rfc6716#section-3.1
    auto toc_byte = TRY(cursor.read_value<u8>());
    auto configuration_number = (toc_byte >> 3) & 0b1'1111;
    auto packet_code = toc_byte & 0b11;

    // clang-format off
    constexpr Array frame_durations = {
            10000, 20000, 40000, 60000, // SILK-only NB
            10000, 20000, 40000, 60000, // SILK-only MB
            10000, 20000, 40000, 60000, // SILK-only WB
            10000, 20000,               // Hybrid SWB
            10000, 20000,               // Hybrid FB
            2500,  5000,  10000, 20000, // CELT-only NB
            2500,  5000,  10000, 20000, // CELT-only WB
            2500,  5000,  10000, 20000, // CELT-only SWB
            2500,  5000,  10000, 20000, // CELT-only FB
    };
    // clang-format on

    auto frame_duration = frame_durations[configuration_number];
    auto packet_duration = TRY([&] -> DecoderErrorOr<int> {
        switch (packet_code) {
        case 0:
            return frame_duration;
        case 1:
        case 2:
            return frame_duration * 2;
        case 3: {
            if (frame_size == 1)
                return DecoderError::corrupted("Opus frame is too small"sv);
            auto frame_count_byte = TRY(cursor.read_value<u8>());
            auto frame_count = frame_count_byte & 0b11'1111;
            return frame_duration * frame_count;
        }
        default:
            VERIFY_NOT_REACHED();
        }
    }());

    return packet_duration * 48 / 1000;
}

DecoderErrorOr<AK::Duration> Opus::parse_frame_duration(MediaStreamCursor& cursor, size_t frame_size)
{
    return AK::Duration::from_time_units(TRY(parse_frame_duration_in_samples(cursor, frame_size)), 1, 48000);
}

}
