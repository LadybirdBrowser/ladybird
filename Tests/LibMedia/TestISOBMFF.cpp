/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BitCast.h>
#include <AK/ByteBuffer.h>
#include <AK/NumericLimits.h>
#include <LibMedia/Containers/ISOBMFF/FragmentSampleIterator.h>
#include <LibMedia/Containers/ISOBMFF/Reader.h>
#include <LibMedia/Containers/ISOBMFF/Streamer.h>
#include <LibMedia/ReadonlyBytesCursor.h>
#include <LibTest/TestCase.h>

namespace {

struct BoxMarker {
    size_t start { 0 };
};

static void append_u16(ByteBuffer& bytes, u16 value)
{
    bytes.append(static_cast<u8>(value >> 8));
    bytes.append(static_cast<u8>(value));
}

static void append_u32(ByteBuffer& bytes, u32 value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.append(static_cast<u8>(value >> shift));
}

static void append_u64(ByteBuffer& bytes, u64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.append(static_cast<u8>(value >> shift));
}

static void patch_u32(ByteBuffer& bytes, size_t position, u32 value)
{
    VERIFY(position + sizeof(value) <= bytes.size());
    for (size_t index = 0; index < sizeof(value); ++index)
        bytes[position + index] = static_cast<u8>(value >> ((sizeof(value) - index - 1) * 8));
}

static void append_four_cc(ByteBuffer& bytes, char const (&type)[5])
{
    for (size_t index = 0; index < 4; ++index)
        bytes.append(type[index]);
}

static BoxMarker begin_box(ByteBuffer& bytes, char const (&type)[5])
{
    auto start = bytes.size();
    append_u32(bytes, 0);
    append_four_cc(bytes, type);
    return { start };
}

static void finish_box(ByteBuffer& bytes, BoxMarker box)
{
    auto size = bytes.size() - box.start;
    VERIFY(size <= NumericLimits<u32>::max());
    patch_u32(bytes, box.start, size);
}

static void append_full_box_header(ByteBuffer& bytes, u8 version = 0, u32 flags = 0)
{
    append_u32(bytes, (static_cast<u32>(version) << 24) | flags);
}

static Media::ISOBMFF::Streamer streamer_for(ByteBuffer const& bytes)
{
    return Media::ISOBMFF::Streamer(make_ref_counted<Media::ReadonlyBytesCursor>(bytes.bytes()));
}

static void append_sample_count_box(ByteBuffer& bytes, char const (&type)[5], u32 entry_count)
{
    auto box = begin_box(bytes, type);
    append_full_box_header(bytes);
    append_u32(bytes, entry_count);
    finish_box(bytes, box);
}

static void append_minimal_track(ByteBuffer& bytes, bool overflowing_sample_table_counts = false, bool nonzero_media_rate_fraction = false, u32 timescale = 1'000)
{
    auto track = begin_box(bytes, "trak");

    auto track_header = begin_box(bytes, "tkhd");
    append_full_box_header(bytes, 0, 1);
    append_u64(bytes, 0);
    append_u32(bytes, 1);
    finish_box(bytes, track_header);

    if (nonzero_media_rate_fraction) {
        auto edit = begin_box(bytes, "edts");
        auto edit_list = begin_box(bytes, "elst");
        append_full_box_header(bytes);
        append_u32(bytes, 1);
        append_u32(bytes, 0);
        append_u32(bytes, 0);
        append_u16(bytes, 1);
        append_u16(bytes, 1);
        finish_box(bytes, edit_list);
        finish_box(bytes, edit);
    }

    auto media = begin_box(bytes, "mdia");

    auto media_header = begin_box(bytes, "mdhd");
    append_full_box_header(bytes);
    append_u64(bytes, 0);
    append_u32(bytes, timescale);
    append_u32(bytes, 0);
    append_u16(bytes, 0);
    finish_box(bytes, media_header);

    auto handler = begin_box(bytes, "hdlr");
    append_full_box_header(bytes);
    append_u32(bytes, 0);
    append_four_cc(bytes, "vide");
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    bytes.append(0);
    finish_box(bytes, handler);

    auto media_information = begin_box(bytes, "minf");
    auto sample_table = begin_box(bytes, "stbl");

    auto sample_description = begin_box(bytes, "stsd");
    append_full_box_header(bytes);
    append_u32(bytes, 0);
    finish_box(bytes, sample_description);

    append_sample_count_box(bytes, "stts", overflowing_sample_table_counts ? NumericLimits<u32>::max() : 0);
    append_sample_count_box(bytes, "stsc", overflowing_sample_table_counts ? 1 : 0);
    append_sample_count_box(bytes, "stco", 0);

    finish_box(bytes, sample_table);
    finish_box(bytes, media_information);
    finish_box(bytes, media);
    finish_box(bytes, track);
}

static void append_movie_extends_box(ByteBuffer& bytes)
{
    auto movie_extends = begin_box(bytes, "mvex");
    auto track_extends = begin_box(bytes, "trex");
    append_full_box_header(bytes);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 0);
    finish_box(bytes, track_extends);
    finish_box(bytes, movie_extends);
}

static void append_movie_box(ByteBuffer& bytes, bool overflowing_sample_table_counts = false, bool nonzero_media_rate_fraction = false, u32 movie_timescale = 1'000, u32 track_timescale = 1'000)
{
    auto movie = begin_box(bytes, "moov");

    auto movie_header = begin_box(bytes, "mvhd");
    append_full_box_header(bytes);
    append_u64(bytes, 0);
    append_u32(bytes, movie_timescale);
    append_u32(bytes, 0);
    finish_box(bytes, movie_header);

    append_minimal_track(bytes, overflowing_sample_table_counts, nonzero_media_rate_fraction, track_timescale);
    append_movie_extends_box(bytes);
    finish_box(bytes, movie);
}

static void append_version_2_audio_movie(ByteBuffer& bytes)
{
    auto movie = begin_box(bytes, "moov");
    auto track = begin_box(bytes, "trak");

    auto track_header = begin_box(bytes, "tkhd");
    append_full_box_header(bytes, 0, 1);
    append_u64(bytes, 0);
    append_u32(bytes, 1);
    finish_box(bytes, track_header);

    auto media = begin_box(bytes, "mdia");
    auto handler = begin_box(bytes, "hdlr");
    append_full_box_header(bytes);
    append_u32(bytes, 0);
    append_four_cc(bytes, "soun");
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    bytes.append(0);
    finish_box(bytes, handler);

    auto media_information = begin_box(bytes, "minf");
    auto sample_table = begin_box(bytes, "stbl");
    auto sample_description = begin_box(bytes, "stsd");
    append_full_box_header(bytes);
    append_u32(bytes, 2);

    for (auto sample_rate : { 96'000.0, 48'000.0 }) {
        auto sample_entry = begin_box(bytes, "mp4a");
        append_u32(bytes, 0);
        append_u16(bytes, 0);
        append_u16(bytes, 1);
        append_u16(bytes, 2);
        append_u16(bytes, 0);
        append_u32(bytes, 0);
        append_u16(bytes, 3);
        append_u16(bytes, 16);
        append_u16(bytes, 0xfffe);
        append_u16(bytes, 0);
        append_u32(bytes, 1 << 16);
        append_u32(bytes, 72);
        append_u64(bytes, bit_cast<u64>(sample_rate));
        append_u32(bytes, 6);
        append_u32(bytes, 0x7f000000);
        append_u32(bytes, 24);
        append_u32(bytes, 0);
        append_u32(bytes, 0);
        append_u32(bytes, 0);
        finish_box(bytes, sample_entry);
    }

    finish_box(bytes, sample_description);
    finish_box(bytes, sample_table);
    finish_box(bytes, media_information);
    finish_box(bytes, media);
    finish_box(bytes, track);
    finish_box(bytes, movie);
}

static void append_movie_fragment_header(ByteBuffer& bytes)
{
    auto movie_fragment_header = begin_box(bytes, "mfhd");
    append_full_box_header(bytes);
    append_u32(bytes, 1);
    finish_box(bytes, movie_fragment_header);
}

static Media::ISOBMFF::TrackFragmentContexts fragment_contexts()
{
    Media::ISOBMFF::TrackFragmentContexts contexts;
    contexts.set(1, {
                        .timescale = 1'000,
                        .sample_description_count = 2,
                        .sample_defaults = {
                            .sample_description_index = 1,
                            .sample_duration = 1,
                            .sample_size = 1,
                        },
                    });
    return contexts;
}

}

TEST_CASE(rejects_file_type_box_with_partial_compatible_brand)
{
    ByteBuffer bytes;
    auto file_type = begin_box(bytes, "ftyp");
    append_four_cc(bytes, "iso5");
    append_u32(bytes, 0);
    bytes.append(0);
    finish_box(bytes, file_type);

    auto streamer = streamer_for(bytes);
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
    EXPECT(Media::ISOBMFF::Reader::parse_file_type_box(streamer, header).is_error());
}

TEST_CASE(rejects_child_box_smaller_than_parsed_fields)
{
    ByteBuffer bytes;
    auto movie = begin_box(bytes, "moov");
    append_u32(bytes, 12);
    append_four_cc(bytes, "mvhd");
    append_full_box_header(bytes);
    auto free = begin_box(bytes, "free");
    append_u64(bytes, 0);
    finish_box(bytes, free);
    finish_box(bytes, movie);

    auto streamer = streamer_for(bytes);
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
    EXPECT(Media::ISOBMFF::Reader::parse_movie_box(streamer, header).is_error());
}

TEST_CASE(rejects_nonzero_edit_list_media_rate_fraction)
{
    ByteBuffer bytes;
    append_movie_box(bytes, false, true);

    auto streamer = streamer_for(bytes);
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
    EXPECT(Media::ISOBMFF::Reader::parse_movie_box(streamer, header).is_error());
}

TEST_CASE(sample_table_entry_count_does_not_wrap)
{
    ByteBuffer bytes;
    append_movie_box(bytes, true);

    auto streamer = streamer_for(bytes);
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
    auto movie = MUST(Media::ISOBMFF::Reader::parse_movie_box(streamer, header));
    EXPECT_EQ(movie.tracks.get(1).value()->sample_table_entry_count, static_cast<u64>(NumericLimits<u32>::max()) + 1);
}

TEST_CASE(parses_nondefault_sample_description_index)
{
    ByteBuffer bytes;
    auto movie_fragment = begin_box(bytes, "moof");
    append_movie_fragment_header(bytes);
    auto track_fragment = begin_box(bytes, "traf");

    auto track_fragment_header = begin_box(bytes, "tfhd");
    append_full_box_header(bytes, 0, 0x000002);
    append_u32(bytes, 1);
    append_u32(bytes, 2);
    finish_box(bytes, track_fragment_header);

    auto decode_time = begin_box(bytes, "tfdt");
    append_full_box_header(bytes);
    append_u32(bytes, 0);
    finish_box(bytes, decode_time);

    auto track_run = begin_box(bytes, "trun");
    append_full_box_header(bytes, 0, 0x000001);
    append_u32(bytes, 1);
    append_u32(bytes, 0);
    finish_box(bytes, track_run);

    finish_box(bytes, track_fragment);
    finish_box(bytes, movie_fragment);

    auto streamer = streamer_for(bytes);
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
    auto fragment = MUST(Media::ISOBMFF::Reader::parse_movie_fragment_box(streamer, header, fragment_contexts()));
    EXPECT_EQ(fragment.total_sample_count(), 1u);
    Media::ISOBMFF::FragmentSampleIterator samples { move(fragment) };
    EXPECT(samples.has_next());
    EXPECT_EQ(samples.peek().sample_description_index, 2u);
}

TEST_CASE(resolves_track_run_samples_taken_entirely_from_defaults)
{
    ByteBuffer bytes;
    auto movie_fragment = begin_box(bytes, "moof");
    append_movie_fragment_header(bytes);
    auto track_fragment = begin_box(bytes, "traf");

    auto track_fragment_header = begin_box(bytes, "tfhd");
    append_full_box_header(bytes);
    append_u32(bytes, 1);
    finish_box(bytes, track_fragment_header);

    auto decode_time = begin_box(bytes, "tfdt");
    append_full_box_header(bytes);
    append_u32(bytes, 0);
    finish_box(bytes, decode_time);

    // Declaring no per-sample fields lets a handful of bytes describe four billion samples, so the run must be
    // stored as the box describes it rather than expanded into one entry per sample.
    auto track_run = begin_box(bytes, "trun");
    append_full_box_header(bytes, 0, 0x000001);
    append_u32(bytes, NumericLimits<u32>::max());
    append_u32(bytes, 0);
    finish_box(bytes, track_run);

    finish_box(bytes, track_fragment);
    finish_box(bytes, movie_fragment);

    auto streamer = streamer_for(bytes);
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
    auto fragment = MUST(Media::ISOBMFF::Reader::parse_movie_fragment_box(streamer, header, fragment_contexts()));
    EXPECT_EQ(fragment.track_runs.size(), 1u);
    EXPECT_EQ(fragment.total_sample_count(), static_cast<size_t>(NumericLimits<u32>::max()));

    Media::ISOBMFF::FragmentSampleIterator samples { move(fragment) };
    EXPECT(samples.has_next());
    auto first = samples.peek();
    samples.advance();
    EXPECT(samples.has_next());
    auto second = samples.peek();
    EXPECT_EQ(first.data_size, 1u);
    EXPECT_EQ(second.data_position, first.data_position + 1);
    EXPECT_EQ(second.decode_timestamp - first.decode_timestamp, AK::Duration::from_time_units(1, 1, 1'000));
}

TEST_CASE(rejects_overflowing_track_run_data_position)
{
    ByteBuffer bytes;
    auto movie_fragment = begin_box(bytes, "moof");
    append_movie_fragment_header(bytes);
    auto track_fragment = begin_box(bytes, "traf");

    auto track_fragment_header = begin_box(bytes, "tfhd");
    append_full_box_header(bytes, 0, 0x000001);
    append_u32(bytes, 1);
    append_u64(bytes, NumericLimits<u64>::max());
    finish_box(bytes, track_fragment_header);

    auto decode_time = begin_box(bytes, "tfdt");
    append_full_box_header(bytes);
    append_u32(bytes, 0);
    finish_box(bytes, decode_time);

    auto track_run = begin_box(bytes, "trun");
    append_full_box_header(bytes, 0, 0x000001);
    append_u32(bytes, 0);
    append_u32(bytes, NumericLimits<i32>::max());
    finish_box(bytes, track_run);

    finish_box(bytes, track_fragment);
    finish_box(bytes, movie_fragment);

    auto streamer = streamer_for(bytes);
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
    EXPECT(Media::ISOBMFF::Reader::parse_movie_fragment_box(streamer, header, fragment_contexts()).is_error());
}

TEST_CASE(rejects_conflicting_track_run_sample_flags)
{
    ByteBuffer bytes;
    auto movie_fragment = begin_box(bytes, "moof");
    append_movie_fragment_header(bytes);
    auto track_fragment = begin_box(bytes, "traf");

    auto track_fragment_header = begin_box(bytes, "tfhd");
    append_full_box_header(bytes, 0, 0x020000);
    append_u32(bytes, 1);
    finish_box(bytes, track_fragment_header);

    auto decode_time = begin_box(bytes, "tfdt");
    append_full_box_header(bytes);
    append_u32(bytes, 0);
    finish_box(bytes, decode_time);

    auto track_run = begin_box(bytes, "trun");
    append_full_box_header(bytes, 0, 0x000001 | 0x000004 | 0x000400);
    append_u32(bytes, 1);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    finish_box(bytes, track_run);

    finish_box(bytes, track_fragment);
    finish_box(bytes, movie_fragment);

    auto streamer = streamer_for(bytes);
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
    EXPECT(Media::ISOBMFF::Reader::parse_movie_fragment_box(streamer, header, fragment_contexts()).is_error());
}

TEST_CASE(rejects_unsupported_track_run_version)
{
    ByteBuffer bytes;
    auto movie_fragment = begin_box(bytes, "moof");
    append_movie_fragment_header(bytes);
    auto track_fragment = begin_box(bytes, "traf");

    auto track_fragment_header = begin_box(bytes, "tfhd");
    append_full_box_header(bytes, 0, 0x020000);
    append_u32(bytes, 1);
    finish_box(bytes, track_fragment_header);

    auto decode_time = begin_box(bytes, "tfdt");
    append_full_box_header(bytes);
    append_u32(bytes, 0);
    finish_box(bytes, decode_time);

    auto track_run = begin_box(bytes, "trun");
    append_full_box_header(bytes, 2, 0x000001);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    finish_box(bytes, track_run);

    finish_box(bytes, track_fragment);
    finish_box(bytes, movie_fragment);

    auto streamer = streamer_for(bytes);
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
    EXPECT(Media::ISOBMFF::Reader::parse_movie_fragment_box(streamer, header, fragment_contexts()).is_error());
}

TEST_CASE(rejects_movie_fragment_without_header)
{
    ByteBuffer bytes;
    auto movie_fragment = begin_box(bytes, "moof");
    auto track_fragment = begin_box(bytes, "traf");

    auto track_fragment_header = begin_box(bytes, "tfhd");
    append_full_box_header(bytes, 0, 0x020000);
    append_u32(bytes, 1);
    finish_box(bytes, track_fragment_header);

    auto decode_time = begin_box(bytes, "tfdt");
    append_full_box_header(bytes);
    append_u32(bytes, 0);
    finish_box(bytes, decode_time);

    finish_box(bytes, track_fragment);
    finish_box(bytes, movie_fragment);

    auto streamer = streamer_for(bytes);
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
    EXPECT(Media::ISOBMFF::Reader::parse_movie_fragment_box(streamer, header, fragment_contexts()).is_error());
}

TEST_CASE(rejects_zero_timescales)
{
    ByteBuffer zero_movie_timescale;
    append_movie_box(zero_movie_timescale, false, false, 0);
    auto movie_streamer = streamer_for(zero_movie_timescale);
    auto movie_header = MUST(Media::ISOBMFF::Reader::read_box_header(movie_streamer));
    EXPECT(Media::ISOBMFF::Reader::parse_movie_box(movie_streamer, movie_header).is_error());

    ByteBuffer zero_track_timescale;
    append_movie_box(zero_track_timescale, false, false, 1'000, 0);
    auto track_streamer = streamer_for(zero_track_timescale);
    auto track_header = MUST(Media::ISOBMFF::Reader::read_box_header(track_streamer));
    EXPECT(Media::ISOBMFF::Reader::parse_movie_box(track_streamer, track_header).is_error());
}

TEST_CASE(parses_version_2_audio_sample_entry_fields)
{
    ByteBuffer bytes;
    append_version_2_audio_movie(bytes);

    auto streamer = streamer_for(bytes);
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
    auto movie = MUST(Media::ISOBMFF::Reader::parse_movie_box(streamer, header));
    auto const& sample_entries = movie.tracks.get(1).value()->sample_entries;
    EXPECT_EQ(sample_entries.size(), 2u);
    auto const& audio = sample_entries[0].audio.value();
    EXPECT_EQ(audio.channel_count, 6);
    EXPECT_EQ(audio.bits_per_sample, 24);
    EXPECT_EQ(audio.sample_rate, 96'000u);
    EXPECT_EQ(sample_entries[1].audio->sample_rate, 48'000u);
}
