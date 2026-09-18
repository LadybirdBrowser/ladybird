/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/BitReader.h>
#include <LibMedia/Containers/MP3/FrameHeader.h>

namespace Media::MP3 {

static constexpr i16 BITRATES[2][3][16] = {
    // Version 1
    {
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, -1 },     // Layer III
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, -1 },    // Layer II
        { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, -1 }, // Layer I
    },
    // Version 2/2.5
    {
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, -1 },      // Layer III
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, -1 },      // Layer II
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, -1 }, // Layer I
    }
};

static constexpr u16 SAMPLES_PER_FRAME[2][3] = {
    // Version 1
    {
        1152, // Layer III
        1152, // Layer II
        384,  // Layer I
    },
    // Version 2/2.5
    {
        576,  // Layer III
        1152, // Layer II
        384,  // Layer I
    },
};

static constexpr u16 SAMPLING_RATES[4][4] = {
    { 11025, 12000, 8000, 0 },  // Version 2.5
    { 0, 0, 0, 0 },             // Reserved
    { 22050, 24000, 16000, 0 }, // Version 2
    { 44100, 48000, 32000, 0 }, // Version 1
};

Optional<FrameHeader> FrameHeader::parse(ReadonlyBytes bytes)
{
    static constexpr u8 SINGLE_CHANNEL_MODE = 0b11;

    BitReader reader { bytes };
    if (reader.read_bits<u16>(SYNC_CODE_BIT_COUNT) != SYNC_CODE)
        return {};

    auto mpeg_version = reader.read_bits<u8>(2);
    if (mpeg_version == 0b01)
        return {};
    auto is_mpeg_version_2 = mpeg_version != 0b11;

    auto layer_description = reader.read_bits<u8>(2);
    if (layer_description == 0b00)
        return {};
    auto is_layer_i = layer_description == 0b11;
    auto layer_description_index = layer_description - 1;

    [[maybe_unused]] auto protection_bit = reader.read_bit();

    auto bitrate_description = reader.read_bits<u8>(4);
    auto bitrate = BITRATES[is_mpeg_version_2][layer_description_index][bitrate_description];
    if (bitrate <= 0)
        return {};

    auto sampling_frequency_index = reader.read_bits<u8>(2);
    auto sampling_frequency = SAMPLING_RATES[mpeg_version][sampling_frequency_index];
    if (sampling_frequency == 0)
        return {};

    auto padding_bit = reader.read_bit();
    [[maybe_unused]] auto private_bit = reader.read_bit();
    auto channel_mode = reader.read_bits<u8>(2);

    if (reader.has_overrun())
        return {};

    auto sample_count = SAMPLES_PER_FRAME[is_mpeg_version_2][layer_description_index];

    constexpr size_t bytes_per_kb = 1000 / 8;
    size_t slot_size = is_layer_i ? 4 : 1;
    auto slot_count = static_cast<u64>(sample_count) * static_cast<u64>(bitrate) * bytes_per_kb / (sampling_frequency * slot_size);

    return FrameHeader {
        .layer = static_cast<u8>(4 - layer_description),
        .sample_rate = sampling_frequency,
        .channel_count = static_cast<u8>(channel_mode == SINGLE_CHANNEL_MODE ? 1 : 2),
        .sample_count = sample_count,
        .frame_byte_size = static_cast<u16>((slot_count + padding_bit) * slot_size),
    };
}

}
