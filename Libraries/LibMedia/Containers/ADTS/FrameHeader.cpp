/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/BitReader.h>
#include <LibMedia/Containers/ADTS/FrameHeader.h>

namespace Media::ADTS {

static constexpr u32 SAMPLING_RATES[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350, 0, 0, 0
};

// Channel configuration zero leaves the channels to be described by a configuration that ADTS does
// not carry, so a frame using it says nothing about its own layout.
static constexpr u8 CHANNEL_COUNTS[8] = { 0, 1, 2, 3, 4, 5, 6, 8 };

Optional<FrameHeader> FrameHeader::parse(ReadonlyBytes bytes)
{
    BitReader reader { bytes };
    if (reader.read_bits<u16>(SYNC_CODE_BIT_COUNT) != SYNC_CODE)
        return {};

    auto version = reader.read_bit() ? MPEGVersion::Version2 : MPEGVersion::Version4;

    if (reader.read_bits<u8>(2) != 0)
        return {};

    auto protection_absent = reader.read_bit();
    auto profile = reader.read_bits<u8>(2);

    auto sampling_frequency_index = reader.read_bits<u8>(4);
    auto sample_rate = SAMPLING_RATES[sampling_frequency_index];
    if (sample_rate == 0)
        return {};

    [[maybe_unused]] auto private_bit = reader.read_bit();

    auto channel_configuration = reader.read_bits<u8>(3);
    auto channel_count = CHANNEL_COUNTS[channel_configuration];
    if (channel_count == 0)
        return {};

    [[maybe_unused]] auto originality = reader.read_bit();
    [[maybe_unused]] auto home = reader.read_bit();
    [[maybe_unused]] auto copyright_identification_bit = reader.read_bit();
    [[maybe_unused]] auto copyright_identification_start = reader.read_bit();

    auto frame_byte_size = reader.read_bits<u16>(13);

    [[maybe_unused]] auto buffer_fullness = reader.read_bits<u16>(11);

    auto block_count = static_cast<u8>(reader.read_bits<u8>(2) + 1);

    if (reader.has_overrun())
        return {};

    auto header_byte_size = protection_absent ? SIZE : SIZE + CRC_SIZE;
    if (frame_byte_size <= header_byte_size)
        return {};

    return FrameHeader {
        .version = version,
        .audio_object_type = static_cast<u8>(profile + 1),
        .has_crc = !protection_absent,
        .sample_rate = sample_rate,
        .channel_count = channel_count,
        .block_count = block_count,
        .sample_count = static_cast<u16>(SAMPLES_PER_BLOCK * block_count),
        .frame_byte_size = frame_byte_size,
    };
}

}
