/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Checked.h>
#include <LibMediaCodecs/Codecs/HDR10Plus.h>
#include <LibMediaCodecs/Codecs/RBSPBitReader.h>
#include <LibMediaCodecs/Codecs/SupplementalEnhancementInformation.h>

namespace Media::Codecs {

static constexpr u32 USER_DATA_REGISTERED_ITU_T_T35_PAYLOAD_TYPE = 4;
static constexpr u32 MASTERING_DISPLAY_COLOUR_VOLUME_PAYLOAD_TYPE = 137;
static constexpr u32 CONTENT_LIGHT_LEVEL_INFORMATION_PAYLOAD_TYPE = 144;
static constexpr u32 MASTERING_DISPLAY_COLOUR_VOLUME_PAYLOAD_SIZE = 24;
static constexpr u32 CONTENT_LIGHT_LEVEL_INFORMATION_PAYLOAD_SIZE = 4;

// ITU-T H.265 (07/2024), 7.3.5: payloadType and payloadSize are a run of 0xFF bytes added to the byte ending it.
static Optional<u32> read_sei_message_value(RBSPBitReader& reader)
{
    Checked<u32> value = 0;
    while (true) {
        auto byte = reader.read_bits<u8>(8);
        if (reader.has_overrun())
            return {};
        value += byte;
        if (value.has_overflow())
            return {};
        if (byte != 0xFF)
            return value.value();
    }
}

// ITU-T H.265 (07/2024), D.3.28: values outside these ranges are unknown or unspecified.
static constexpr u16 MINIMUM_CHROMATICITY_CODE = 5;
static constexpr u16 MAXIMUM_CHROMATICITY_X_CODE = 37'000;
static constexpr u16 MAXIMUM_CHROMATICITY_Y_CODE = 42'000;
static constexpr u32 MINIMUM_MAXIMUM_LUMINANCE_CODE = 50'000;
static constexpr u32 MAXIMUM_MAXIMUM_LUMINANCE_CODE = 100'000'000;
static constexpr u32 MAXIMUM_MINIMUM_LUMINANCE_CODE = 50'000;

static Optional<MasteringDisplayColorVolume> read_mastering_display_color_volume(RBSPBitReader& reader)
{
    constexpr float chromaticity_units_per_unit = 50'000.0f;
    constexpr float luminance_units_per_candela_per_square_metre = 10'000.0f;
    auto is_specified = true;
    auto read_chromaticity = [&] {
        auto x = reader.read_bits<u16>(16);
        auto y = reader.read_bits<u16>(16);
        if (x < MINIMUM_CHROMATICITY_CODE || x > MAXIMUM_CHROMATICITY_X_CODE || y < MINIMUM_CHROMATICITY_CODE || y > MAXIMUM_CHROMATICITY_Y_CODE)
            is_specified = false;
        return Chromaticity { static_cast<float>(x) / chromaticity_units_per_unit, static_cast<float>(y) / chromaticity_units_per_unit };
    };

    MasteringDisplayColorVolume volume;
    // The primaries are coded green, blue, then red, the order SMPTE ST 2086 lists them in.
    volume.green = read_chromaticity();
    volume.blue = read_chromaticity();
    volume.red = read_chromaticity();
    volume.white_point = read_chromaticity();
    auto maximum_luminance = reader.read_bits<u32>(32);
    auto minimum_luminance = reader.read_bits<u32>(32);
    if (maximum_luminance < MINIMUM_MAXIMUM_LUMINANCE_CODE || maximum_luminance > MAXIMUM_MAXIMUM_LUMINANCE_CODE)
        is_specified = false;
    // A minimum of zero is out of range too, but Blu-ray releases use it, so it is accepted as FFmpeg accepts it.
    if (minimum_luminance > MAXIMUM_MINIMUM_LUMINANCE_CODE || minimum_luminance >= maximum_luminance)
        is_specified = false;
    if (!is_specified)
        return {};
    volume.maximum_luminance = static_cast<float>(maximum_luminance) / luminance_units_per_candela_per_square_metre;
    volume.minimum_luminance = static_cast<float>(minimum_luminance) / luminance_units_per_candela_per_square_metre;
    return volume;
}

static Optional<float> read_hdr10_plus_maximum_scene_luminance(RBSPBitReader& reader, u32 payload_size)
{
    VERIFY(payload_size <= HDR10_PLUS_MAXIMUM_T35_MESSAGE_SIZE);
    Array<u8, HDR10_PLUS_MAXIMUM_T35_MESSAGE_SIZE> message;
    for (size_t index = 0; index < payload_size; index++)
        message[index] = reader.read_bits<u8>(8);
    return hdr10_plus_maximum_scene_luminance(ReadonlyBytes { message.data(), payload_size });
}

static ContentLightLevelInformation read_content_light_level_information(RBSPBitReader& reader)
{
    ContentLightLevelInformation information;
    information.maximum_content_light_level = reader.read_bits<u16>(16);
    information.maximum_frame_average_light_level = reader.read_bits<u16>(16);
    return information;
}

// ITU-T H.265 (07/2024), 7.3.2.4: sei_rbsp().
ColorMetadata high_dynamic_range_metadata_of_sei_payload(ReadonlyBytes escaped_sei_payload)
{
    ColorMetadata metadata;
    RBSPBitReader reader { escaped_sei_payload };

    // The last byte holds the RBSP trailing bits rather than the start of another message.
    while (reader.remaining_byte_count() > 1) {
        auto payload_type = read_sei_message_value(reader);
        auto payload_size = read_sei_message_value(reader);
        if (!payload_type.has_value() || !payload_size.has_value() || payload_size.value() > reader.remaining_byte_count())
            break;

        Optional<MasteringDisplayColorVolume> volume;
        Optional<ContentLightLevelInformation> information;
        Optional<float> maximum_scene_luminance;
        auto unread_payload_size = payload_size.value();
        if (payload_type == USER_DATA_REGISTERED_ITU_T_T35_PAYLOAD_TYPE && unread_payload_size <= HDR10_PLUS_MAXIMUM_T35_MESSAGE_SIZE) {
            maximum_scene_luminance = read_hdr10_plus_maximum_scene_luminance(reader, unread_payload_size);
            unread_payload_size = 0;
        } else if (payload_type == MASTERING_DISPLAY_COLOUR_VOLUME_PAYLOAD_TYPE && unread_payload_size >= MASTERING_DISPLAY_COLOUR_VOLUME_PAYLOAD_SIZE) {
            volume = read_mastering_display_color_volume(reader);
            unread_payload_size -= MASTERING_DISPLAY_COLOUR_VOLUME_PAYLOAD_SIZE;
        } else if (payload_type == CONTENT_LIGHT_LEVEL_INFORMATION_PAYLOAD_TYPE && unread_payload_size >= CONTENT_LIGHT_LEVEL_INFORMATION_PAYLOAD_SIZE) {
            information = read_content_light_level_information(reader);
            unread_payload_size -= CONTENT_LIGHT_LEVEL_INFORMATION_PAYLOAD_SIZE;
        }
        reader.skip_bits(static_cast<size_t>(unread_payload_size) * 8);
        // A message that runs into the RBSP trailing bits is missing some of its own.
        if (reader.has_overrun() || reader.remaining_byte_count() == 0)
            break;

        if (volume.has_value())
            metadata.mastering_display_color_volume = volume;
        if (information.has_value())
            metadata.content_light_level_information = information;
        if (maximum_scene_luminance.has_value())
            metadata.maximum_scene_luminance = maximum_scene_luminance;
    }
    return metadata;
}

}
