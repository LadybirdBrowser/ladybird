/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Vector.h>
#include <LibCore/File.h>
#include <LibMedia/Containers/ADTS/Reader.h>
#include <LibMedia/ReadonlyBytesCursor.h>
#include <LibTest/TestCase.h>

static constexpr size_t FRAME_BYTE_SIZE = 256;

// 44100 Hz stereo AAC-LC frames carrying no CRC and one block each.
static void append_frames(Vector<u8>& bytes, size_t count, size_t frame_byte_size = FRAME_BYTE_SIZE)
{
    for (size_t index = 0; index < count; index++) {
        bytes.append(0xFF);
        bytes.append(0xF1);
        bytes.append(0x50);
        bytes.append(static_cast<u8>(0x80 | ((frame_byte_size >> 11) & 3)));
        bytes.append(static_cast<u8>((frame_byte_size >> 3) & 0xFF));
        bytes.append(static_cast<u8>(((frame_byte_size & 7) << 5) | 0x1F));
        bytes.append(0xFC);
        for (size_t padding = Media::ADTS::FrameHeader::SIZE; padding < frame_byte_size; padding++)
            bytes.append(0);
    }
}

static NonnullRefPtr<Media::ReadonlyBytesCursor> create_cursor(ReadonlyBytes bytes)
{
    return make_ref_counted<Media::ReadonlyBytesCursor>(bytes);
}

static Optional<Media::ADTS::Reader> read(Vector<u8> const& bytes)
{
    return Media::ADTS::Reader::from_stream(create_cursor(bytes));
}

static ByteBuffer read_fixture(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return MUST(file->read_until_eof());
}

TEST_CASE(reader_describes_the_stream_its_first_frame_announces)
{
    Vector<u8> bytes;
    append_frames(bytes, 8);

    auto reader = read(bytes);
    EXPECT(reader.has_value());
    EXPECT_EQ(reader->first_frame_position(), 0u);
    EXPECT_EQ(reader->frame_header().sample_rate, 44100u);
    EXPECT_EQ(reader->frame_header().channel_count, 2);
    EXPECT_EQ(reader->frame_header().audio_object_type, 2);
    EXPECT_EQ(reader->frame_header().frame_byte_size, FRAME_BYTE_SIZE);
}

TEST_CASE(reader_estimates_a_duration_from_the_bitrate_at_the_front_of_a_stream)
{
    // Every frame is the same size, so extrapolating from the front describes the whole stream.
    Vector<u8> bytes;
    append_frames(bytes, 200);

    auto reader = read(bytes);
    EXPECT(reader.has_value());
    EXPECT_EQ(reader->duration(), AK::Duration::from_time_units(200 * 1024, 1, 44100));
}

TEST_CASE(frame_iterator_walks_every_frame_of_a_stream)
{
    Vector<u8> bytes;
    append_frames(bytes, 12);

    Media::ADTS::FrameIterator iterator { create_cursor(bytes), 0, Media::ADTS::FrameIterator::Resynchronize::No };

    size_t frame_count = 0;
    size_t expected_position = 0;
    while (true) {
        auto frame = iterator.next();
        if (!frame.has_value())
            break;
        EXPECT_EQ(frame->position, expected_position);
        expected_position += FRAME_BYTE_SIZE;
        frame_count++;
    }
    EXPECT_EQ(frame_count, 12u);
}

TEST_CASE(frame_iterator_reads_the_audio_without_the_framing_around_it)
{
    Vector<u8> bytes;
    append_frames(bytes, 2);

    Media::ADTS::FrameIterator iterator { create_cursor(bytes), 0, Media::ADTS::FrameIterator::Resynchronize::No };
    auto frame = iterator.next();
    EXPECT(frame.has_value());

    // The header frames the audio rather than describing it, and a decoder configured from the
    // record the header implies expects the access unit alone.
    auto data = MUST(iterator.read_frame_data(*frame));
    EXPECT_EQ(data.size(), FRAME_BYTE_SIZE - Media::ADTS::FrameHeader::SIZE);
}

TEST_CASE(frame_iterator_resumes_at_the_frames_that_follow_damage)
{
    Vector<u8> bytes;
    append_frames(bytes, 2);
    bytes.resize(bytes.size() + 500);
    append_frames(bytes, 3);

    Media::ADTS::FrameIterator iterator { create_cursor(bytes), 0, Media::ADTS::FrameIterator::Resynchronize::Yes };

    size_t frame_count = 0;
    while (iterator.next().has_value())
        frame_count++;
    EXPECT_EQ(frame_count, 5u);
}

TEST_CASE(frame_iterator_stops_where_a_frame_header_can_no_longer_be_read)
{
    Vector<u8> bytes;
    append_frames(bytes, 3);
    bytes.resize(bytes.size() - (FRAME_BYTE_SIZE - 2));

    Media::ADTS::FrameIterator iterator { create_cursor(bytes), 0, Media::ADTS::FrameIterator::Resynchronize::No };

    size_t frame_count = 0;
    while (iterator.next().has_value())
        frame_count++;
    EXPECT_EQ(frame_count, 2u);
}

TEST_CASE(frame_iterator_reports_a_frame_whose_audio_has_not_all_arrived)
{
    // A header states how far its frame reaches, so the frame is known before the audio behind it
    // has been read. What counts that frame towards a timeline is the byte range holding all of it.
    Vector<u8> bytes;
    append_frames(bytes, 2);
    bytes.resize(bytes.size() - 4);

    Media::ADTS::FrameIterator iterator { create_cursor(bytes), 0, Media::ADTS::FrameIterator::Resynchronize::No };
    EXPECT(iterator.next().has_value());

    auto truncated = iterator.next();
    EXPECT(truncated.has_value());
    EXPECT_EQ(truncated->header.frame_byte_size, FRAME_BYTE_SIZE);
    EXPECT(iterator.read_frame_data(*truncated).is_error());
}

TEST_CASE(reader_searches_past_bytes_that_precede_the_audio)
{
    Vector<u8> bytes;
    bytes.resize(1000);
    append_frames(bytes, 8);

    auto reader = read(bytes);
    EXPECT(reader.has_value());
    EXPECT_EQ(reader->first_frame_position(), 1000u);
}

TEST_CASE(reader_skips_an_identification_tag_that_precedes_the_audio)
{
    Vector<u8> bytes;
    // An ID3v2 tag of 100 bytes, whose size is stored as four seven-bit groups.
    for (auto byte : Array<u8, 10> { 'I', 'D', '3', 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 100 })
        bytes.append(byte);
    bytes.resize(110);
    append_frames(bytes, 8);

    auto reader = read(bytes);
    EXPECT(reader.has_value());
    EXPECT_EQ(reader->first_frame_position(), 110u);
}

TEST_CASE(reader_needs_a_run_of_frames_wherever_it_has_to_search_for_them)
{
    // A sync code with nothing behind it is not where the audio begins.
    Vector<u8> bytes;
    bytes.resize(1000);
    bytes[500] = 0xFF;
    bytes[501] = 0xF1;
    bytes[502] = 0x50;
    bytes[503] = 0x80;
    bytes[504] = 0x20;
    bytes[505] = 0x1F;
    bytes[506] = 0xFC;
    append_frames(bytes, 8);

    auto reader = read(bytes);
    EXPECT(reader.has_value());
    EXPECT_EQ(reader->first_frame_position(), 1000u);
}

TEST_CASE(reader_declines_a_stream_that_holds_no_frames)
{
    Vector<u8> bytes;
    bytes.resize(4096);
    EXPECT(!read(bytes).has_value());
}

TEST_CASE(backward_search_reads_back_through_bytes_holding_no_frames)
{
    Vector<u8> bytes;
    append_frames(bytes, 8);
    auto audio_end = bytes.size();
    bytes.resize(bytes.size() + 2000);

    auto cursor = create_cursor(bytes);
    auto boundary = Media::ADTS::Reader::find_frame_boundary_at_or_before(cursor, bytes.size() - 1, 0, bytes.size());
    EXPECT(boundary.has_value());

    // The last frame that a run of frames reaches, not the bytes trailing them.
    EXPECT_EQ(*boundary, audio_end - FRAME_BYTE_SIZE * 3);
}

TEST_CASE(reader_reads_a_stream_that_an_encoder_produced)
{
    auto data = read_fixture("bbb.adts"sv);
    auto reader = Media::ADTS::Reader::from_stream(create_cursor(data));
    EXPECT(reader.has_value());
    EXPECT_EQ(reader->first_frame_position(), 0u);
    EXPECT_EQ(reader->frame_header().sample_rate, 44100u);
    EXPECT_EQ(reader->frame_header().channel_count, 2);

    Media::ADTS::FrameIterator iterator { create_cursor(data), reader->first_frame_position(), Media::ADTS::FrameIterator::Resynchronize::No };
    size_t frame_count = 0;
    u64 sample_count = 0;
    size_t byte_count = 0;
    while (true) {
        auto frame = iterator.next();
        if (!frame.has_value())
            break;
        frame_count++;
        sample_count += frame->header.sample_count;
        byte_count += frame->header.frame_byte_size;
    }

    // The walk reaches every frame and accounts for every byte of the file.
    EXPECT_EQ(frame_count, 219u);
    EXPECT_EQ(byte_count, data.size());

    auto walked_duration = AK::Duration::from_time_units(static_cast<i64>(sample_count), 1, 44100);
    EXPECT_EQ(walked_duration, AK::Duration::from_time_units(219 * 1024, 1, 44100));

    // The clip opens quietly, so the frames at its front are smaller than those behind them and the
    // estimate taken from them runs long. Nothing read from the front of a stream avoids that.
    auto estimate = reader->duration();
    EXPECT(estimate > walked_duration);
    EXPECT(estimate < walked_duration + AK::Duration::from_milliseconds(600));
}
