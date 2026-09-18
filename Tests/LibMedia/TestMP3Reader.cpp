/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Vector.h>
#include <LibCore/File.h>
#include <LibMedia/Containers/MP3/Reader.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/ReadonlyBytesCursor.h>
#include <LibTest/TestCase.h>

static constexpr size_t FRAME_BYTE_SIZE = 384;

// 128 kbps 48 kHz stereo Layer III frames, which each occupy 384 bytes.
static void append_frames(Vector<u8>& bytes, size_t count)
{
    for (size_t index = 0; index < count; index++) {
        bytes.append(0xFF);
        bytes.append(0xFB);
        bytes.append(0x94);
        bytes.append(0x00);
        for (size_t padding = Media::MP3::FrameHeader::SIZE; padding < FRAME_BYTE_SIZE; padding++)
            bytes.append(0);
    }
}

static NonnullRefPtr<Media::ReadonlyBytesCursor> create_cursor(ReadonlyBytes bytes)
{
    return make_ref_counted<Media::ReadonlyBytesCursor>(bytes);
}

static Optional<Media::MP3::Reader> read(Vector<u8> const& bytes)
{
    return Media::MP3::Reader::from_stream(create_cursor(bytes));
}

static ByteBuffer read_fixture(StringView path)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    return MUST(file->read_until_eof());
}

TEST_CASE(reader_takes_an_exact_duration_from_a_stream_info_frame)
{
    auto data = read_fixture("buffered-ranges/tone.mp3"sv);
    auto reader = Media::MP3::Reader::from_stream(create_cursor(data));
    EXPECT(reader.has_value());

    EXPECT(reader->stream_info().has_value());
    EXPECT_EQ(reader->stream_info()->frame_count, 335u);
    EXPECT_EQ(reader->frame_header().sample_rate, 48000u);

    // The identification tag and the frame describing the stream both precede the audio.
    EXPECT_EQ(reader->first_audio_frame_position(), 429u);

    EXPECT(!reader->duration_is_estimated());
    EXPECT_EQ(reader->duration(), AK::Duration::from_milliseconds(8'040));
}

TEST_CASE(reader_estimates_a_duration_without_a_stream_info_frame)
{
    auto data = read_fixture("bbb_without_stream_info.mp3"sv);
    auto reader = Media::MP3::Reader::from_stream(create_cursor(data));
    EXPECT(reader.has_value());

    EXPECT(!reader->stream_info().has_value());
    EXPECT_EQ(reader->first_audio_frame_position(), 0u);
    EXPECT_EQ(reader->frame_header().sample_rate, 44100u);

    // The file holds 385 frames of 443520 samples, or 10.0571 seconds, which the padding slots of
    // the scanned frames let the bitrate reproduce to within a fraction of a percent.
    EXPECT(reader->duration_is_estimated());
    EXPECT(reader->duration() > AK::Duration::from_milliseconds(10'050));
    EXPECT(reader->duration() < AK::Duration::from_milliseconds(10'065));
}

TEST_CASE(frame_iterator_walks_every_frame_of_a_stream)
{
    auto data = read_fixture("bbb_without_stream_info.mp3"sv);
    Media::MP3::FrameIterator iterator { create_cursor(data), 0, Media::MP3::FrameIterator::Resynchronize::No };

    size_t frame_count = 0;
    u64 sample_count = 0;
    for (auto frame = iterator.next(); frame.has_value(); frame = iterator.next()) {
        frame_count++;
        sample_count += frame->header.sample_count;
    }

    EXPECT_EQ(frame_count, 385u);
    EXPECT_EQ(sample_count, 443520u);
    EXPECT_EQ(iterator.position(), data.size());
}

TEST_CASE(frame_iterator_stops_where_a_frame_header_can_no_longer_be_read)
{
    Vector<u8> bytes;
    append_frames(bytes, 3);

    // Residence of the rest of a frame is the concern of whoever walks it, so the header of the
    // third frame is all that has to be missing for the walk to end.
    auto truncated = bytes.span().trim(2 * FRAME_BYTE_SIZE + 2);
    Media::MP3::FrameIterator iterator { create_cursor(truncated), 0, Media::MP3::FrameIterator::Resynchronize::No };
    EXPECT(iterator.next().has_value());
    EXPECT(iterator.next().has_value());
    EXPECT(!iterator.next().has_value());
    EXPECT_EQ(iterator.position(), 2u * FRAME_BYTE_SIZE);
}

TEST_CASE(reader_reports_what_it_scanned_of_a_stream_of_an_unknown_size)
{
    Vector<u8> frames;
    append_frames(frames, 3);

    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->add_chunk_at(0, frames);

    auto cursor = stream->create_cursor();
    cursor->set_is_blocking(false);

    // Nothing says how long the stream is, so the frames the scan walked are all there is to go on.
    auto reader = Media::MP3::Reader::from_stream(cursor);
    EXPECT(reader.has_value());
    EXPECT(reader->duration_is_estimated());
    EXPECT_EQ(reader->duration(), AK::Duration::from_time_units(3 * 1152, 1, 48'000));
}

TEST_CASE(reader_declines_a_stream_that_holds_no_frames)
{
    Vector<u8> silence;
    silence.resize(1000);
    EXPECT(!read(silence).has_value());
}

TEST_CASE(reader_searches_past_bytes_that_precede_the_audio)
{
    // Sniffing has already identified the stream by the time a reader is made of it, so bytes that
    // a cut left ahead of the audio are searched past rather than taken as another format.
    Vector<u8> leading_zeroes;
    leading_zeroes.resize(1000);
    append_frames(leading_zeroes, 3);
    auto reader = read(leading_zeroes);
    EXPECT(reader.has_value());
    EXPECT_EQ(reader->first_audio_frame_position(), 1000u);

    Vector<u8> leading_sync_codes;
    for (size_t index = 0; index < 64; index++)
        leading_sync_codes.append(0xFF);
    append_frames(leading_sync_codes, 3);
    reader = read(leading_sync_codes);
    EXPECT(reader.has_value());
    EXPECT_EQ(reader->first_audio_frame_position(), 64u);
}

TEST_CASE(reader_honors_the_size_an_identification_tag_declares)
{
    Vector<u8> tagged;
    Array<u8, 10> tag_header { 'I', 'D', '3', 4, 0, 0, 0, 0, 15, 80 };
    tagged.append(tag_header.data(), tag_header.size());
    auto tag_size = (15u << 7) | 80u;
    EXPECT_EQ(tag_size, 2000u);

    // Frames inside the tag would be found first if its declared size were ignored.
    Vector<u8> tag_contents;
    append_frames(tag_contents, 5);
    tag_contents.resize(tag_size);
    tagged.extend(tag_contents);
    append_frames(tagged, 3);

    auto reader = read(tagged);
    EXPECT(reader.has_value());
    EXPECT_EQ(reader->first_audio_frame_position(), 2010u);
}

TEST_CASE(reader_takes_a_stream_that_a_frame_begins)
{
    Vector<u8> one_frame;
    append_frames(one_frame, 1);
    EXPECT(read(one_frame).has_value());

    Vector<u8> silence;
    silence.resize(1000);
    EXPECT(!read(silence).has_value());
}

TEST_CASE(reader_needs_a_run_of_frames_wherever_it_has_to_search_for_them)
{
    // Where the audio belongs, one frame is proof enough. Found among the bytes ahead of it, a lone
    // frame is as likely to be a false sync code, so others have to follow it.
    Vector<u8> preceded_by_one_frame;
    preceded_by_one_frame.resize(1000);
    append_frames(preceded_by_one_frame, 1);
    EXPECT(!read(preceded_by_one_frame).has_value());

    Vector<u8> preceded_by_several;
    preceded_by_several.resize(1000);
    append_frames(preceded_by_several, 3);
    EXPECT(read(preceded_by_several).has_value());
}

TEST_CASE(backward_search_reads_back_through_bytes_holding_no_frames)
{
    // Far more bytes holding no frames lie between the target and the audio before it than a single
    // read of the search holds.
    Vector<u8> bytes;
    append_frames(bytes, 10);
    auto audio_end = bytes.size();
    bytes.resize(audio_end + 10'000);
    auto target = bytes.size() - 1;

    // The last two frames lack the run of frames after them that would rule out a false sync code.
    auto boundary = Media::MP3::Reader::find_frame_boundary_at_or_before(create_cursor(bytes), target, 0, bytes.size());
    EXPECT_EQ(boundary.value_or(0), 7 * FRAME_BYTE_SIZE);

    // The search goes no further back than its lower bound.
    boundary = Media::MP3::Reader::find_frame_boundary_at_or_before(create_cursor(bytes), target, audio_end, bytes.size());
    EXPECT(!boundary.has_value());
}
