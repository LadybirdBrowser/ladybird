/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
#include <AK/MemoryStream.h>
#include <LibMedia/BitReader.h>
#include <LibMedia/Codecs/AAC.h>

namespace Media::Codecs {

Optional<AAC::Parameters> AAC::parse_configuration_record(ReadonlyBytes audio_specific_config, u8 object_type_indication)
{
    static constexpr u8 ESCAPED_AUDIO_OBJECT_TYPE = 31;

    BitReader reader { audio_specific_config };
    u32 audio_object_type = reader.read_bits<u8>(5);
    if (audio_object_type == ESCAPED_AUDIO_OBJECT_TYPE)
        audio_object_type = 32 + reader.read_bits<u8>(6);
    if (reader.has_overrun())
        return {};

    return Parameters { object_type_indication, audio_object_type };
}

DecoderErrorOr<FixedArray<u8>> AAC::elementary_stream_descriptor_for_configuration_record(ReadonlyBytes audio_specific_config)
{
    static constexpr u8 ELEMENTARY_STREAM_DESCRIPTOR_TAG = 0x03;
    static constexpr u8 DECODER_CONFIGURATION_DESCRIPTOR_TAG = 0x04;
    static constexpr u8 DECODER_SPECIFIC_INFO_TAG = 0x05;
    static constexpr u8 SYNC_LAYER_CONFIGURATION_DESCRIPTOR_TAG = 0x06;

    static constexpr u16 ELEMENTARY_STREAM_ID = 0;
    static constexpr u8 ELEMENTARY_STREAM_FLAGS = 0;
    // https://mp4ra.org/registered-types/object-types#stream-types
    static constexpr u8 AUDIO_STREAM_TYPE = 0x05;
    static constexpr u8 STREAM_TYPE_RESERVED_BIT = 1;
    static constexpr u8 MP4_SYNC_LAYER_CONFIGURATION = 0x02;

    static constexpr size_t DESCRIPTOR_HEADER_SIZE = 5;
    static constexpr size_t MAXIMUM_DESCRIPTOR_SIZE = (1 << 28) - 1;
    static constexpr size_t ELEMENTARY_STREAM_FIELDS_SIZE = 3;
    static constexpr size_t DECODER_CONFIGURATION_FIELDS_SIZE = 13;
    static constexpr size_t SYNC_LAYER_CONFIGURATION_SIZE = 1;

    size_t decoder_specific_info_size = 0;
    if (!audio_specific_config.is_empty())
        decoder_specific_info_size = DESCRIPTOR_HEADER_SIZE + audio_specific_config.size();
    auto decoder_configuration_size = DECODER_CONFIGURATION_FIELDS_SIZE + decoder_specific_info_size;
    auto elementary_stream_size = ELEMENTARY_STREAM_FIELDS_SIZE + DESCRIPTOR_HEADER_SIZE + decoder_configuration_size + DESCRIPTOR_HEADER_SIZE + SYNC_LAYER_CONFIGURATION_SIZE;
    if (elementary_stream_size > MAXIMUM_DESCRIPTOR_SIZE)
        return DecoderError::corrupted("Audio Specific Config is too large for an ES_Descriptor"sv);

    auto descriptor = DECODER_TRY_ALLOC(FixedArray<u8>::create(DESCRIPTOR_HEADER_SIZE + elementary_stream_size));
    FixedMemoryStream writer { descriptor.span() };

    auto write_descriptor_header = [&](u8 tag, size_t size) -> ErrorOr<void> {
        TRY(writer.write_value(tag));
        TRY(writer.write_value(static_cast<u8>(0x80 | ((size >> 21) & 0x7F))));
        TRY(writer.write_value(static_cast<u8>(0x80 | ((size >> 14) & 0x7F))));
        TRY(writer.write_value(static_cast<u8>(0x80 | ((size >> 7) & 0x7F))));
        TRY(writer.write_value(static_cast<u8>(size & 0x7F)));
        return {};
    };

    auto write_descriptor = [&]() -> ErrorOr<void> {
        TRY(write_descriptor_header(ELEMENTARY_STREAM_DESCRIPTOR_TAG, elementary_stream_size));
        TRY(writer.write_value(BigEndian<u16> { ELEMENTARY_STREAM_ID }));
        TRY(writer.write_value(ELEMENTARY_STREAM_FLAGS));

        TRY(write_descriptor_header(DECODER_CONFIGURATION_DESCRIPTOR_TAG, decoder_configuration_size));
        TRY(writer.write_value(MPEG4_AUDIO_OBJECT_TYPE_INDICATION));
        TRY(writer.write_value(static_cast<u8>((AUDIO_STREAM_TYPE << 2) | STREAM_TYPE_RESERVED_BIT)));
        Array<u8, 11> const unknown_buffer_size_and_bit_rates {};
        TRY(writer.write_until_depleted(unknown_buffer_size_and_bit_rates.span()));

        if (!audio_specific_config.is_empty()) {
            TRY(write_descriptor_header(DECODER_SPECIFIC_INFO_TAG, audio_specific_config.size()));
            TRY(writer.write_until_depleted(audio_specific_config));
        }

        TRY(write_descriptor_header(SYNC_LAYER_CONFIGURATION_DESCRIPTOR_TAG, SYNC_LAYER_CONFIGURATION_SIZE));
        TRY(writer.write_value(MP4_SYNC_LAYER_CONFIGURATION));
        return {};
    };
    MUST(write_descriptor());
    VERIFY(writer.is_eof());

    return descriptor;
}

}
