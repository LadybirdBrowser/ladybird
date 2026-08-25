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

void append_u16(ByteBuffer& bytes, u16 value)
{
    bytes.append(static_cast<u8>(value >> 8));
    bytes.append(static_cast<u8>(value));
}

void append_u32(ByteBuffer& bytes, u32 value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        bytes.append(static_cast<u8>(value >> shift));
}

void append_u64(ByteBuffer& bytes, u64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        bytes.append(static_cast<u8>(value >> shift));
}

void patch_u32(ByteBuffer& bytes, size_t position, u32 value)
{
    VERIFY(position + sizeof(value) <= bytes.size());
    for (size_t index = 0; index < sizeof(value); ++index)
        bytes[position + index] = static_cast<u8>(value >> ((sizeof(value) - index - 1) * 8));
}

void append_four_cc(ByteBuffer& bytes, char const (&type)[5])
{
    for (size_t index = 0; index < 4; ++index)
        bytes.append(type[index]);
}

BoxMarker begin_box(ByteBuffer& bytes, char const (&type)[5])
{
    auto start = bytes.size();
    append_u32(bytes, 0);
    append_four_cc(bytes, type);
    return { start };
}

void finish_box(ByteBuffer& bytes, BoxMarker box)
{
    auto size = bytes.size() - box.start;
    VERIFY(size <= NumericLimits<u32>::max());
    patch_u32(bytes, box.start, size);
}

void append_full_box_header(ByteBuffer& bytes, u8 version = 0, u32 flags = 0)
{
    append_u32(bytes, (static_cast<u32>(version) << 24) | flags);
}

Media::ISOBMFF::Streamer streamer_for(ByteBuffer const& bytes)
{
    return Media::ISOBMFF::Streamer(make_ref_counted<Media::ReadonlyBytesCursor>(bytes.bytes()));
}

void append_sample_count_box(ByteBuffer& bytes, char const (&type)[5], u32 entry_count)
{
    auto box = begin_box(bytes, type);
    append_full_box_header(bytes);
    append_u32(bytes, entry_count);
    finish_box(bytes, box);
}

void append_minimal_track(ByteBuffer& bytes, bool overflowing_sample_table_counts = false, bool nonzero_media_rate_fraction = false, u32 timescale = 1'000)
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

void append_movie_extends_box(ByteBuffer& bytes)
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

void append_movie_box(ByteBuffer& bytes, bool overflowing_sample_table_counts = false, bool nonzero_media_rate_fraction = false, u32 movie_timescale = 1'000, u32 track_timescale = 1'000)
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

void append_version_2_audio_movie(ByteBuffer& bytes)
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

void append_movie_fragment_header(ByteBuffer& bytes)
{
    auto movie_fragment_header = begin_box(bytes, "mfhd");
    append_full_box_header(bytes);
    append_u32(bytes, 1);
    finish_box(bytes, movie_fragment_header);
}

Media::ISOBMFF::TrackFragmentContexts fragment_contexts()
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

enum class ColourBoxOrder {
    None,
    BeforeConfiguration,
    AfterConfiguration,
};

void append_bt709_colour_information_box(ByteBuffer& bytes)
{
    auto colour_information = begin_box(bytes, "colr");
    append_four_cc(bytes, "nclx");
    append_u16(bytes, 1); // BT.709 primaries
    append_u16(bytes, 1); // BT.709 transfer characteristics
    append_u16(bytes, 1); // BT.709 matrix coefficients
    bytes.append(0);      // studio range
    finish_box(bytes, colour_information);
}

// An AV1 configuration record whose sequence header describes BT.2020 with the SMPTE 2084 transfer function.
void append_av1_configuration_box(ByteBuffer& bytes)
{
    static constexpr Array<u8, 21> record {
        0x81, 0x08, 0x0c, 0x00,
        0x0a, 0x0f, 0x00, 0x00, 0x00, 0x43, 0xfc, 0x1d, 0xfc, 0x10, 0xdd, 0xc2, 0x79, 0x90, 0x91, 0x00, 0x90
    };
    auto configuration = begin_box(bytes, "av1C");
    MUST(bytes.try_append(record.span()));
    finish_box(bytes, configuration);
}

void append_vp9_configuration_box(ByteBuffer& bytes)
{
    auto configuration = begin_box(bytes, "vpcC");
    append_full_box_header(bytes, 1);
    bytes.append(2);                        // profile
    bytes.append(31);                       // level
    bytes.append((10 << 4) | (1 << 1) | 0); // 10-bit, 4:2:0, studio range
    bytes.append(9);                        // BT.2020 primaries
    bytes.append(16);                       // SMPTE 2084 transfer characteristics
    bytes.append(9);                        // BT.2020 non-constant luminance matrix coefficients
    append_u16(bytes, 0);                   // codec initialization data size
    finish_box(bytes, configuration);
}

void append_video_movie(ByteBuffer& bytes, char const (&format)[5], void (*append_configuration_box)(ByteBuffer&), ColourBoxOrder colour_box_order)
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
    append_u32(bytes, 1);

    auto sample_entry = begin_box(bytes, format);
    append_u32(bytes, 0);
    append_u16(bytes, 0);
    append_u16(bytes, 1); // data_reference_index
    append_u32(bytes, 0); // pre_defined, reserved
    for (size_t index = 0; index < 3; ++index)
        append_u32(bytes, 0); // pre_defined
    append_u16(bytes, 1920);
    append_u16(bytes, 1080);
    append_u32(bytes, 0x00480000); // horizresolution
    append_u32(bytes, 0x00480000); // vertresolution
    append_u32(bytes, 0);          // reserved
    append_u16(bytes, 1);          // frame_count
    for (size_t index = 0; index < 32; ++index)
        bytes.append(0);       // compressorname
    append_u16(bytes, 24);     // depth
    append_u16(bytes, 0xffff); // pre_defined

    if (colour_box_order == ColourBoxOrder::BeforeConfiguration)
        append_bt709_colour_information_box(bytes);
    append_configuration_box(bytes);
    if (colour_box_order == ColourBoxOrder::AfterConfiguration)
        append_bt709_colour_information_box(bytes);

    finish_box(bytes, sample_entry);

    finish_box(bytes, sample_description);
    finish_box(bytes, sample_table);
    finish_box(bytes, media_information);
    finish_box(bytes, media);
    finish_box(bytes, track);
    finish_box(bytes, movie);
}

// SampleEntry owns its configuration data and so cannot be copied out of the movie.
struct SampleEntrySummary {
    Optional<Media::CodingIndependentCodePoints> cicp;
    Optional<Media::ParsedCodec> parsed_codec;
};

Media::DecoderErrorOr<Media::ISOBMFF::Movie> parse_movie(ByteBuffer& bytes)
{
    auto streamer = streamer_for(bytes);
    auto header = TRY(Media::ISOBMFF::Reader::read_box_header(streamer));
    return Media::ISOBMFF::Reader::parse_movie_box(streamer, header);
}

SampleEntrySummary parse_only_sample_entry(ByteBuffer& bytes)
{
    auto streamer = streamer_for(bytes);
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
    auto movie = MUST(Media::ISOBMFF::Reader::parse_movie_box(streamer, header));
    auto const& sample_entries = movie.tracks.get(1).value()->sample_entries;
    VERIFY(sample_entries.size() == 1);
    return { sample_entries[0].video->cicp, sample_entries[0].parsed_codec };
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

TEST_CASE(parses_vp_codec_configuration_box)
{
    ByteBuffer bytes;
    append_video_movie(bytes, "vp09", append_vp9_configuration_box, ColourBoxOrder::None);
    auto entry = parse_only_sample_entry(bytes);

    EXPECT(entry.parsed_codec.has_value());
    EXPECT_EQ(entry.parsed_codec->codec_id(), Media::CodecID::VP9);
    auto const& parameters = entry.parsed_codec->vp9_parameters().value();
    EXPECT_EQ(parameters.profile, 2);
    EXPECT_EQ(parameters.level, 31);
    EXPECT_EQ(parameters.bit_depth, 10);

    // Without a colour information box, the configuration record supplies the color.
    EXPECT(entry.cicp.has_value());
    EXPECT(entry.cicp->color_primaries() == Media::ColorPrimaries::BT2020);
    EXPECT(entry.cicp->transfer_characteristics() == Media::TransferCharacteristics::SMPTE2084);
}

TEST_CASE(colour_information_box_overrides_the_vp_configuration_box_in_either_order)
{
    for (auto order : { ColourBoxOrder::BeforeConfiguration, ColourBoxOrder::AfterConfiguration }) {
        ByteBuffer bytes;
        append_video_movie(bytes, "vp09", append_vp9_configuration_box, order);
        auto entry = parse_only_sample_entry(bytes);

        EXPECT(entry.cicp.has_value());
        EXPECT(entry.cicp->color_primaries() == Media::ColorPrimaries::BT709);
        EXPECT(entry.cicp->transfer_characteristics() == Media::TransferCharacteristics::BT709);

        // The configuration record is still read for the codec parameters.
        EXPECT(entry.parsed_codec.has_value());
        EXPECT_EQ(entry.parsed_codec->vp9_parameters()->profile, 2);
    }
}

TEST_CASE(parses_av1_configuration_box_color_from_its_sequence_header)
{
    ByteBuffer bytes;
    append_video_movie(bytes, "av01", append_av1_configuration_box, ColourBoxOrder::None);
    auto entry = parse_only_sample_entry(bytes);

    EXPECT(entry.parsed_codec.has_value());
    EXPECT_EQ(entry.parsed_codec->codec_id(), Media::CodecID::AV1);
    EXPECT_EQ(entry.parsed_codec->av1_parameters()->level, 8);

    EXPECT(entry.cicp.has_value());
    EXPECT(entry.cicp->color_primaries() == Media::ColorPrimaries::BT2020);
    EXPECT(entry.cicp->transfer_characteristics() == Media::TransferCharacteristics::SMPTE2084);
}

TEST_CASE(colour_information_box_overrides_the_av1_configuration_box)
{
    ByteBuffer bytes;
    append_video_movie(bytes, "av01", append_av1_configuration_box, ColourBoxOrder::BeforeConfiguration);
    auto entry = parse_only_sample_entry(bytes);

    EXPECT(entry.cicp.has_value());
    EXPECT(entry.cicp->color_primaries() == Media::ColorPrimaries::BT709);
    EXPECT(entry.cicp->transfer_characteristics() == Media::TransferCharacteristics::BT709);
}

TEST_CASE(rejects_a_configuration_box_that_disagrees_with_the_sample_entry_format)
{
    // An AV1 configuration record under a VP9 sample entry, and the reverse.
    ByteBuffer vp9_entry_with_av1_configuration;
    append_video_movie(vp9_entry_with_av1_configuration, "vp09", append_av1_configuration_box, ColourBoxOrder::None);
    EXPECT(parse_movie(vp9_entry_with_av1_configuration).is_error());

    ByteBuffer av1_entry_with_vp9_configuration;
    append_video_movie(av1_entry_with_vp9_configuration, "av01", append_vp9_configuration_box, ColourBoxOrder::None);
    EXPECT(parse_movie(av1_entry_with_vp9_configuration).is_error());
}

TEST_CASE(accepts_a_vp_configuration_box_under_a_vp8_sample_entry)
{
    // The VP configuration box is shared by VP8 and VP9, so it is not a mismatch under either.
    ByteBuffer bytes;
    append_video_movie(bytes, "vp08", append_vp9_configuration_box, ColourBoxOrder::None);
    EXPECT(!parse_movie(bytes).is_error());
}
