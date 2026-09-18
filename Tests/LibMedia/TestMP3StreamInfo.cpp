/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCore/File.h>
#include <LibMedia/Containers/ID3.h>
#include <LibMedia/Containers/MP3/FrameHeader.h>
#include <LibMedia/Containers/MP3/StreamInfo.h>
#include <LibTest/TestCase.h>

static void append_big_endian(Vector<u8>& bytes, u32 value)
{
    for (auto shift : { 24, 16, 8, 0 })
        bytes.append(static_cast<u8>(value >> shift));
}

static void append_characters(Vector<u8>& bytes, StringView characters)
{
    for (auto character : characters)
        bytes.append(static_cast<u8>(character));
}

// A 128 kbps 48 kHz stereo Layer III frame, whose side information places a tag at offset 36.
static Vector<u8> version_1_stereo_frame(Vector<u8> const& tag)
{
    Vector<u8> frame;
    frame.resize(384);
    frame[0] = 0xFF;
    frame[1] = 0xFB;
    frame[2] = 0x94;
    frame[3] = 0x00;
    tag.span().copy_to(frame.span().slice(36));
    return frame;
}

// The same frame with its protection bit clear, which places a CRC before the side information.
static Vector<u8> version_1_stereo_frame_with_crc(Vector<u8> const& tag)
{
    Vector<u8> frame;
    frame.resize(384);
    frame[0] = 0xFF;
    frame[1] = 0xFA;
    frame[2] = 0x94;
    frame[3] = 0x00;
    tag.span().copy_to(frame.span().slice(38));
    return frame;
}

static Media::MP3::FrameHeader parse_header(Vector<u8> const& frame)
{
    auto header = Media::MP3::FrameHeader::parse(frame);
    VERIFY(header.has_value());
    return *header;
}

TEST_CASE(stream_info_reads_the_info_tag_of_a_real_file)
{
    auto file = MUST(Core::File::open("buffered-ranges/tone.mp3"sv, Core::File::OpenMode::Read));
    auto data = MUST(file->read_until_eof());

    auto tag_size = Media::ID3::version_2_tag_size(data);
    EXPECT_EQ(tag_size, 45u);

    auto frame = data.span().slice(*tag_size);
    auto header = Media::MP3::FrameHeader::parse(frame);
    EXPECT(header.has_value());

    auto stream_info = Media::MP3::StreamInfo::parse(frame, *header);
    EXPECT(stream_info.has_value());
    EXPECT_EQ(stream_info->frame_count, 335u);
    EXPECT_EQ(stream_info->byte_count, 129024u);
    EXPECT(stream_info->seek_table.has_value());
    EXPECT_EQ(stream_info->encoder_delay, 576);
    EXPECT_EQ(stream_info->encoder_padding, 1344);

    EXPECT_EQ(stream_info->total_sample_count(*header), 385920u);
}

TEST_CASE(stream_info_reads_a_xing_tag_without_a_seek_table)
{
    Vector<u8> tag;
    append_characters(tag, "Xing"sv);
    append_big_endian(tag, 0x03);
    append_big_endian(tag, 1000);
    append_big_endian(tag, 250000);

    auto frame = version_1_stereo_frame(tag);
    auto header = parse_header(frame);

    auto stream_info = Media::MP3::StreamInfo::parse(frame, header);
    EXPECT(stream_info.has_value());
    EXPECT_EQ(stream_info->frame_count, 1000u);
    EXPECT_EQ(stream_info->byte_count, 250000u);
    EXPECT(!stream_info->seek_table.has_value());
    EXPECT_EQ(stream_info->encoder_delay, 0);
    EXPECT_EQ(stream_info->total_sample_count(header), 1152u * 1000u);
}

TEST_CASE(stream_info_reads_a_xing_tag_that_a_crc_displaces)
{
    Vector<u8> tag;
    append_characters(tag, "Xing"sv);
    append_big_endian(tag, 0x03);
    append_big_endian(tag, 1000);
    append_big_endian(tag, 250000);

    auto frame = version_1_stereo_frame_with_crc(tag);
    auto header = parse_header(frame);
    EXPECT(header.has_crc);

    auto stream_info = Media::MP3::StreamInfo::parse(frame, header);
    EXPECT(stream_info.has_value());
    EXPECT_EQ(stream_info->frame_count, 1000u);
    EXPECT_EQ(stream_info->byte_count, 250000u);
}

TEST_CASE(stream_info_reads_the_encoder_delays_that_follow_the_seek_table)
{
    Vector<u8> tag;
    append_characters(tag, "Info"sv);
    append_big_endian(tag, 0x0F);
    append_big_endian(tag, 100);
    append_big_endian(tag, 38400);
    for (size_t index = 0; index < Media::MP3::StreamInfo::SEEK_TABLE_ENTRY_COUNT; index++)
        tag.append(static_cast<u8>(index));
    append_big_endian(tag, 0);
    append_characters(tag, "LAME3.100"sv);
    for (size_t index = 0; index < 12; index++)
        tag.append(0);
    tag.append(0x24);
    tag.append(0x05);
    tag.append(0x40);

    auto frame = version_1_stereo_frame(tag);
    auto header = parse_header(frame);

    auto stream_info = Media::MP3::StreamInfo::parse(frame, header);
    EXPECT(stream_info.has_value());
    EXPECT(stream_info->seek_table.has_value());
    EXPECT_EQ(stream_info->seek_table.value()[7], 7);
    EXPECT_EQ(stream_info->encoder_delay, 576);
    EXPECT_EQ(stream_info->encoder_padding, 1344);
}

TEST_CASE(stream_info_reads_a_vbri_tag)
{
    Vector<u8> tag;
    append_characters(tag, "VBRI"sv);
    tag.append(0);
    tag.append(1);
    append_big_endian(tag, 0);
    append_big_endian(tag, 250000);
    append_big_endian(tag, 1000);

    auto frame = version_1_stereo_frame(tag);
    auto header = parse_header(frame);

    auto stream_info = Media::MP3::StreamInfo::parse(frame, header);
    EXPECT(stream_info.has_value());
    EXPECT_EQ(stream_info->byte_count, 250000u);
    EXPECT_EQ(stream_info->frame_count, 1000u);
}

TEST_CASE(stream_info_rejects_frames_that_describe_nothing)
{
    Vector<u8> empty;
    auto plain_frame = version_1_stereo_frame(empty);
    EXPECT(!Media::MP3::StreamInfo::parse(plain_frame, parse_header(plain_frame)).has_value());

    Vector<u8> unsupported_vbri;
    append_characters(unsupported_vbri, "VBRI"sv);
    unsupported_vbri.append(0);
    unsupported_vbri.append(2);
    append_big_endian(unsupported_vbri, 0);
    append_big_endian(unsupported_vbri, 250000);
    append_big_endian(unsupported_vbri, 1000);
    auto vbri_frame = version_1_stereo_frame(unsupported_vbri);
    EXPECT(!Media::MP3::StreamInfo::parse(vbri_frame, parse_header(vbri_frame)).has_value());

    Vector<u8> tag;
    append_characters(tag, "Xing"sv);
    append_big_endian(tag, 0x03);
    append_big_endian(tag, 1000);
    append_big_endian(tag, 250000);
    auto truncated = version_1_stereo_frame(tag);
    auto header = parse_header(truncated);
    EXPECT(!Media::MP3::StreamInfo::parse(truncated.span().trim(44), header).has_value());

    auto layer_ii_header = header;
    layer_ii_header.layer = 2;
    EXPECT(!Media::MP3::StreamInfo::parse(truncated, layer_ii_header).has_value());
}
