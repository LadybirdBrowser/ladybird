/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/BitStream.h>
#include <AK/Endian.h>
#include <AK/MemoryStream.h>
#include <LibMediaCodecs/BitReader.h>
#include <LibMediaCodecs/Codecs/AAC.h>

namespace Media::Codecs {

static constexpr u8 ESCAPED_AUDIO_OBJECT_TYPE = 31;
static constexpr u8 ESCAPED_SAMPLE_RATE_INDEX = 0xF;
static constexpr Array SAMPLE_RATES {
    96000u, 88200u, 64000u, 48000u, 44100u, 32000u, 24000u, 22050u, 16000u, 12000u, 11025u, 8000u, 7350u
};

static u32 read_audio_object_type(BitReader& reader)
{
    u32 audio_object_type = reader.read_bits<u8>(5);
    if (audio_object_type == ESCAPED_AUDIO_OBJECT_TYPE)
        audio_object_type = 32 + reader.read_bits<u8>(6);
    return audio_object_type;
}

static Optional<u32> read_sample_rate(BitReader& reader)
{
    auto index = reader.read_bits<u8>(4);
    if (index == ESCAPED_SAMPLE_RATE_INDEX)
        return reader.read_bits<u32>(24);
    if (index >= SAMPLE_RATES.size())
        return {};
    return SAMPLE_RATES[index];
}

Optional<AAC::Parameters> AAC::parse_configuration_record(ReadonlyBytes audio_specific_config, u8 object_type_indication)
{
    BitReader reader { audio_specific_config };
    auto audio_object_type = read_audio_object_type(reader);
    if (reader.has_overrun())
        return {};

    return Parameters { object_type_indication, audio_object_type };
}

static Optional<u8> channel_count_from_configuration(u8 channel_configuration)
{
    switch (channel_configuration) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
        return channel_configuration;
    case 7:
        return 8;
    default:
        return {};
    }
}

Optional<AAC::ExplicitSpectralBandReplication> AAC::parse_explicit_spectral_band_replication(ReadonlyBytes audio_specific_config)
{
    static constexpr u16 SPECTRAL_BAND_REPLICATION_SYNC_EXTENSION_TYPE = 0x2B7;

    BitReader reader { audio_specific_config };

    // Parse the AudioSpecificConfig fields first.
    auto audio_object_type = read_audio_object_type(reader);
    auto sample_rate = read_sample_rate(reader);
    auto channel_configuration = reader.read_bits<u8>(4);

    Optional<u32> output_sample_rate;
    if (audio_object_type == SPECTRAL_BAND_REPLICATION_AUDIO_OBJECT_TYPE || audio_object_type == PARAMETRIC_STEREO_AUDIO_OBJECT_TYPE) {
        output_sample_rate = read_sample_rate(reader);
        if (!output_sample_rate.has_value())
            return {};
        audio_object_type = read_audio_object_type(reader);
    }

    if (!sample_rate.has_value() || reader.has_overrun())
        return {};

    // Parse the GASpecificConfig when the audio object type requires it.
    if (audio_object_type < MAIN_AUDIO_OBJECT_TYPE || audio_object_type > LONG_TERM_PREDICTION_AUDIO_OBJECT_TYPE)
        return {};

    // A channel configuration of zero is followed by a program config element, which we don't parse.
    auto channel_count = channel_count_from_configuration(channel_configuration);
    if (!channel_count.has_value())
        return {};

    GeneralAudioSpecificConfig general_audio_specific_config {};
    general_audio_specific_config.frame_length_flag = reader.read_bit();
    auto depends_on_core_coder = reader.read_bit();
    if (depends_on_core_coder)
        general_audio_specific_config.core_coder_delay = reader.read_bits<u16>(14);
    auto extension_flag = reader.read_bit();
    if (extension_flag)
        general_audio_specific_config.extension_flag_3 = reader.read_bit();
    if (reader.has_overrun())
        return {};

    // Backward-compatible signaling follows the core configuration with a sync extension.
    if (!output_sample_rate.has_value() && reader.bits_remaining() >= 16) {
        auto sync_extension_type = reader.read_bits<u16>(11);
        if (sync_extension_type != SPECTRAL_BAND_REPLICATION_SYNC_EXTENSION_TYPE)
            return {};
        if (read_audio_object_type(reader) != SPECTRAL_BAND_REPLICATION_AUDIO_OBJECT_TYPE)
            return {};
        auto spectral_band_replication_present = reader.read_bit();
        if (!spectral_band_replication_present)
            return {};
        output_sample_rate = read_sample_rate(reader);
        if (!output_sample_rate.has_value() || reader.has_overrun())
            return {};
    }

    if (!output_sample_rate.has_value())
        return {};

    return ExplicitSpectralBandReplication {
        .core_audio_object_type = audio_object_type,
        .core_sample_rate = sample_rate.value(),
        .channel_count = channel_count.value(),
        .general_audio_specific_config = general_audio_specific_config,
        .output_sample_rate = output_sample_rate.value(),
    };
}

static u8 sample_rate_index(u32 sample_rate)
{
    for (size_t index = 0; index < SAMPLE_RATES.size(); index++) {
        if (SAMPLE_RATES[index] == sample_rate)
            return static_cast<u8>(index);
    }
    return ESCAPED_SAMPLE_RATE_INDEX;
}

// The channel configurations that name a layout, indexed by the number of channels they describe.
static Optional<u8> channel_configuration(u8 channel_count)
{
    switch (channel_count) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
        return channel_count;
    case 8:
        return 7;
    default:
        return {};
    }
}

DecoderErrorOr<FixedArray<u8>> AAC::create_configuration_record(u32 audio_object_type, u32 sample_rate, u8 channel_count, Optional<u32> spectral_band_replication_sample_rate, GeneralAudioSpecificConfig const& general_audio_specific_config)
{
    static constexpr u16 SPECTRAL_BAND_REPLICATION_SYNC_EXTENSION_TYPE = 0x2B7;
    static constexpr size_t MAXIMUM_SIZE = 16;

    VERIFY(audio_object_type < ESCAPED_AUDIO_OBJECT_TYPE);

    auto channels = channel_configuration(channel_count);
    if (!channels.has_value())
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "No AAC channel configuration describes {} channels", channel_count);

    Array<u8, MAXIMUM_SIZE> buffer {};
    FixedMemoryStream buffer_stream { buffer.span() };
    BigEndianOutputBitStream writer { MaybeOwned<Stream> { buffer_stream } };

    auto write_configuration = [&]() -> ErrorOr<void> {
        auto write_sample_rate = [&](u32 rate) -> ErrorOr<void> {
            auto index = sample_rate_index(rate);
            TRY(writer.write_bits(index, 4));
            if (index == ESCAPED_SAMPLE_RATE_INDEX)
                TRY(writer.write_bits(rate, 24));
            return {};
        };

        TRY(writer.write_bits(audio_object_type, 5));
        TRY(write_sample_rate(sample_rate));
        TRY(writer.write_bits(*channels, 4));
        TRY(writer.write_bits(general_audio_specific_config.frame_length_flag, 1));
        TRY(writer.write_bits(general_audio_specific_config.core_coder_delay.has_value(), 1));
        if (general_audio_specific_config.core_coder_delay.has_value())
            TRY(writer.write_bits(*general_audio_specific_config.core_coder_delay, 14));
        TRY(writer.write_bits(general_audio_specific_config.extension_flag_3.has_value(), 1));
        if (general_audio_specific_config.extension_flag_3.has_value())
            TRY(writer.write_bits(*general_audio_specific_config.extension_flag_3, 1));

        if (spectral_band_replication_sample_rate.has_value()) {
            TRY(writer.write_bits(SPECTRAL_BAND_REPLICATION_SYNC_EXTENSION_TYPE, 11));
            TRY(writer.write_bits(SPECTRAL_BAND_REPLICATION_AUDIO_OBJECT_TYPE, 5));
            TRY(writer.write_bits(1u, 1));
            TRY(write_sample_rate(*spectral_band_replication_sample_rate));
        }

        TRY(writer.align_to_byte_boundary());
        return {};
    };
    if (write_configuration().is_error())
        return DecoderError::corrupted("AAC configuration record does not fit its own fields"sv);

    return DECODER_TRY_ALLOC(FixedArray<u8>::create(buffer.span().trim(buffer_stream.offset())));
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
