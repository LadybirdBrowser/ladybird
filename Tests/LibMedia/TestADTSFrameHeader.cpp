/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibMedia/Containers/ADTS/FrameHeader.h>
#include <LibTest/TestCase.h>

TEST_CASE(frame_header_describes_an_mpeg_4_low_complexity_frame)
{
    // The first frame of a 48 kHz stereo AAC-LC stream muxed by FFmpeg.
    Array<u8, 7> bytes { 0xFF, 0xF1, 0x4C, 0x80, 0x31, 0x1F, 0xFC };

    auto header = Media::ADTS::FrameHeader::parse(bytes);
    EXPECT(header.has_value());
    EXPECT_EQ(header->version, Media::ADTS::MPEGVersion::Version4);
    EXPECT_EQ(header->audio_object_type, 2);
    EXPECT_EQ(header->sample_rate, 48000u);
    EXPECT_EQ(header->channel_count, 2);
    EXPECT_EQ(header->sample_count, 1024);
    EXPECT_EQ(header->frame_byte_size, 392);
    EXPECT_EQ(header->block_count, 1);
    EXPECT(!header->has_crc);
    EXPECT_EQ(header->payload_offset(), 7u);
}

TEST_CASE(frame_header_describes_an_mpeg_2_high_efficiency_frame)
{
    // High Efficiency AAC describes itself at the sample rate of its core, which is half of what it
    // decodes to, so a frame still covers 1024 samples of the rate the header names.
    Array<u8, 7> bytes { 0xFF, 0xF9, 0x58, 0xA0, 0x04, 0x20, 0x00 };

    auto header = Media::ADTS::FrameHeader::parse(bytes);
    EXPECT(header.has_value());
    EXPECT_EQ(header->version, Media::ADTS::MPEGVersion::Version2);
    EXPECT_EQ(header->audio_object_type, 2);
    EXPECT_EQ(header->sample_rate, 24000u);
    EXPECT_EQ(header->channel_count, 2);
    EXPECT_EQ(header->frame_byte_size, 33);
}

TEST_CASE(frame_header_reads_the_channel_configuration_across_its_two_bytes)
{
    // Parametric stereo describes a single core channel, and the field spans the byte boundary.
    Array<u8, 7> bytes { 0xFF, 0xF9, 0x58, 0x60, 0x02, 0xE0, 0x00 };

    auto header = Media::ADTS::FrameHeader::parse(bytes);
    EXPECT(header.has_value());
    EXPECT_EQ(header->channel_count, 1);
    EXPECT_EQ(header->frame_byte_size, 23);
}

TEST_CASE(frame_header_accounts_for_the_crc_that_the_protection_bit_announces)
{
    // Clearing the protection bit puts a CRC between the header and the audio.
    Array<u8, 7> bytes { 0xFF, 0xF0, 0x4C, 0x80, 0x31, 0x1F, 0xFC };

    auto header = Media::ADTS::FrameHeader::parse(bytes);
    EXPECT(header.has_value());
    EXPECT(header->has_crc);
    EXPECT_EQ(header->payload_offset(), 9u);

    // The length the header states covers the CRC, so the audio is shorter, not the frame longer.
    EXPECT_EQ(header->frame_byte_size, 392);
}

TEST_CASE(frame_header_counts_the_blocks_a_frame_carries)
{
    Array<u8, 7> bytes { 0xFF, 0xF1, 0x4C, 0x80, 0x31, 0x1F, 0xFF };

    auto header = Media::ADTS::FrameHeader::parse(bytes);
    EXPECT(header.has_value());
    EXPECT_EQ(header->block_count, 4);
    EXPECT_EQ(header->sample_count, 4096);
}

TEST_CASE(frame_header_declines_what_does_not_describe_a_frame)
{
    // No sync code.
    Array<u8, 7> not_a_frame { 0x00, 0x00, 0x4C, 0x80, 0x31, 0x1F, 0xFC };
    EXPECT(!Media::ADTS::FrameHeader::parse(not_a_frame).has_value());

    // A layer other than zero is not ADTS.
    Array<u8, 7> layered { 0xFF, 0xF3, 0x4C, 0x80, 0x31, 0x1F, 0xFC };
    EXPECT(!Media::ADTS::FrameHeader::parse(layered).has_value());

    // Sampling frequency index 13 is reserved.
    Array<u8, 7> reserved_rate { 0xFF, 0xF1, 0x74, 0x80, 0x31, 0x1F, 0xFC };
    EXPECT(!Media::ADTS::FrameHeader::parse(reserved_rate).has_value());

    // Channel configuration zero leaves the layout to a configuration ADTS does not carry.
    Array<u8, 7> no_channels { 0xFF, 0xF1, 0x4C, 0x00, 0x31, 0x1F, 0xFC };
    EXPECT(!Media::ADTS::FrameHeader::parse(no_channels).has_value());

    // A frame that claims to be shorter than its own header describes nothing.
    Array<u8, 7> too_short { 0xFF, 0xF1, 0x4C, 0x80, 0x00, 0x3F, 0xFC };
    EXPECT(!Media::ADTS::FrameHeader::parse(too_short).has_value());

    // A header cut short reads past its end.
    Array<u8, 4> truncated { 0xFF, 0xF1, 0x4C, 0x80 };
    EXPECT(!Media::ADTS::FrameHeader::parse(truncated).has_value());
}
