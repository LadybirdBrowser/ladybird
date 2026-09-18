/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/Containers/MP3/FrameHeader.h>
#include <LibTest/TestCase.h>

TEST_CASE(frame_header_describes_a_version_1_layer_iii_frame)
{
    // The first frame of buffered-ranges/tone.mp3: 128 kbps, 48000 Hz, stereo.
    Array<u8, 4> bytes { 0xFF, 0xFB, 0x94, 0x00 };

    auto header = Media::MP3::FrameHeader::parse(bytes);
    EXPECT(header.has_value());
    EXPECT_EQ(header->layer, 3);
    EXPECT_EQ(header->sample_rate, 48000u);
    EXPECT_EQ(header->channel_count, 2);
    EXPECT_EQ(header->sample_count, 1152);
    EXPECT_EQ(header->frame_byte_size, 384);
    EXPECT(!header->has_crc);
}

TEST_CASE(frame_header_reads_the_protection_bit)
{
    // The protection bit is clear when the frame carries a CRC.
    Array<u8, 4> protected_frame { 0xFF, 0xFA, 0x94, 0x00 };

    auto header = Media::MP3::FrameHeader::parse(protected_frame);
    EXPECT(header.has_value());
    EXPECT(header->has_crc);
    EXPECT_EQ(header->frame_byte_size, 384);
}

TEST_CASE(frame_header_counts_the_padding_slot)
{
    // The same frame with its padding bit set occupies one more byte.
    Array<u8, 4> bytes { 0xFF, 0xFB, 0x96, 0x00 };

    auto header = Media::MP3::FrameHeader::parse(bytes);
    EXPECT(header.has_value());
    EXPECT_EQ(header->frame_byte_size, 385);
}

TEST_CASE(frame_header_reads_the_channel_mode)
{
    // Channel mode 0b11 is the only one describing a single channel.
    Array<u8, 4> single_channel { 0xFF, 0xFB, 0x94, 0xC0 };
    EXPECT_EQ(Media::MP3::FrameHeader::parse(single_channel)->channel_count, 1);

    Array<u8, 4> dual_channel { 0xFF, 0xFB, 0x94, 0x80 };
    EXPECT_EQ(Media::MP3::FrameHeader::parse(dual_channel)->channel_count, 2);
}

TEST_CASE(frame_header_describes_a_version_2_layer_iii_frame)
{
    // MPEG-2 halves the samples a Layer III frame carries.
    Array<u8, 4> bytes { 0xFF, 0xF3, 0x94, 0x00 };

    auto header = Media::MP3::FrameHeader::parse(bytes);
    EXPECT(header.has_value());
    EXPECT_EQ(header->sample_rate, 24000u);
    EXPECT_EQ(header->sample_count, 576);
    EXPECT_EQ(header->layer, 3);
}

TEST_CASE(frame_header_rejects_reserved_and_truncated_values)
{
    Array<u8, 4> no_sync { 0xFF, 0x1B, 0x94, 0x00 };
    EXPECT(!Media::MP3::FrameHeader::parse(no_sync).has_value());

    Array<u8, 4> reserved_version { 0xFF, 0xEB, 0x94, 0x00 };
    EXPECT(!Media::MP3::FrameHeader::parse(reserved_version).has_value());

    Array<u8, 4> reserved_layer { 0xFF, 0xF9, 0x94, 0x00 };
    EXPECT(!Media::MP3::FrameHeader::parse(reserved_layer).has_value());

    Array<u8, 4> free_bitrate { 0xFF, 0xFB, 0x04, 0x00 };
    EXPECT(!Media::MP3::FrameHeader::parse(free_bitrate).has_value());

    Array<u8, 4> reserved_sample_rate { 0xFF, 0xFB, 0x9C, 0x00 };
    EXPECT(!Media::MP3::FrameHeader::parse(reserved_sample_rate).has_value());

    Array<u8, 4> header { 0xFF, 0xFB, 0x94, 0x00 };
    EXPECT(!Media::MP3::FrameHeader::parse(header.span().trim(3)).has_value());
}
