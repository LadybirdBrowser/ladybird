/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/BitCast.h>
#include <AK/ByteBuffer.h>
#include <AK/NumericLimits.h>
#include <LibCore/File.h>
#include <LibMedia/Containers/ISOBMFF/FragmentSampleIterator.h>
#include <LibMedia/Containers/ISOBMFF/ISOBMFFDemuxer.h>
#include <LibMedia/Containers/ISOBMFF/Reader.h>
#include <LibMedia/Containers/ISOBMFF/Streamer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/ReadonlyBytesCursor.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

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

void append_video_movie(ByteBuffer& bytes, char const (&format)[5], void (*append_configuration_box)(ByteBuffer&), ColourBoxOrder colour_box_order, Function<void(ByteBuffer&)> const& append_tables = {}, bool fragmented = false)
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
    if (append_tables)
        append_tables(bytes);
    finish_box(bytes, sample_table);
    finish_box(bytes, media_information);
    finish_box(bytes, media);
    finish_box(bytes, track);
    if (fragmented)
        append_movie_extends_box(bytes);
    finish_box(bytes, movie);
}

void append_audio_movie(ByteBuffer& bytes, u32 timescale, u32 sample_rate, u16 channel_count, void (*append_configuration_box)(ByteBuffer&) = nullptr)
{
    auto movie = begin_box(bytes, "moov");
    auto track = begin_box(bytes, "trak");

    auto track_header = begin_box(bytes, "tkhd");
    append_full_box_header(bytes, 0, 1);
    append_u64(bytes, 0);
    append_u32(bytes, 1);
    finish_box(bytes, track_header);

    auto media = begin_box(bytes, "mdia");

    auto media_header = begin_box(bytes, "mdhd");
    append_full_box_header(bytes);
    append_u64(bytes, 0); // creation_time and modification_time
    append_u32(bytes, timescale);
    append_u32(bytes, 0);      // duration
    append_u16(bytes, 0x55c4); // und
    append_u16(bytes, 0);      // pre_defined
    finish_box(bytes, media_header);

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
    append_u32(bytes, 1);

    auto sample_entry = begin_box(bytes, "mp4a");
    append_u32(bytes, 0);
    append_u16(bytes, 0);
    append_u16(bytes, 1); // data_reference_index
    append_u16(bytes, 0); // version
    append_u32(bytes, 0);
    append_u16(bytes, 0); // reserved
    append_u16(bytes, channel_count);
    append_u16(bytes, 16); // bits_per_sample
    append_u16(bytes, 0);  // pre_defined
    append_u16(bytes, 0);  // reserved
    append_u32(bytes, sample_rate << 16);
    if (append_configuration_box)
        append_configuration_box(bytes);
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

static Optional<FixedArray<u8>> first_frame_codec_configuration(ByteBuffer const& data)
{
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(data);
    auto demuxer = MUST(Media::ISOBMFF::ISOBMFFDemuxer::from_stream(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Audio));
    VERIFY(track.has_value());
    MUST(demuxer->create_context_for_track(*track));

    auto frame = MUST(demuxer->get_next_sample_for_track(*track));
    auto configuration = frame.new_codec_configuration();
    if (!configuration.has_value())
        return {};
    return MUST(FixedArray<u8>::create(*configuration));
}

TEST_CASE(aac_track_keeps_the_configuration_its_container_carries)
{
    auto file = MUST(Core::File::open("./hevc_10bit.mp4"sv, Core::File::OpenMode::Read));
    auto configuration = first_frame_codec_configuration(MUST(file->read_until_eof()));

    // The track describes its own codec, so nothing is invented for it.
    EXPECT(configuration.has_value());
    EXPECT(configuration->size() >= 2);
}

TEST_CASE(aac_track_without_a_configuration_is_described_by_its_container)
{
    auto file = MUST(Core::File::open("./hevc_10bit.mp4"sv, Core::File::OpenMode::Read));
    auto data = MUST(file->read_until_eof());

    // Renaming the Elementary Stream Descriptor Box leaves a track that states its sample rate and
    // channel count but never describes the codec that decodes it.
    Optional<size_t> esds;
    for (size_t index = 0; index + 4 <= data.size(); index++) {
        if (StringView { data.bytes().slice(index, 4) } == "esds"sv) {
            EXPECT(!esds.has_value());
            esds = index;
        }
    }
    VERIFY(esds.has_value());
    data.overwrite(*esds, "free", 4);

    auto configuration = first_frame_codec_configuration(data);
    EXPECT(configuration.has_value());

    // An Audio Specific Config of object type 2, sampling frequency index 3 and channel
    // configuration 1: Low Complexity AAC at the 48000 Hz mono the container declares.
    EXPECT_EQ(configuration->size(), 2u);
    EXPECT_EQ((*configuration)[0], 0x11);
    EXPECT_EQ((*configuration)[1], 0x88);
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

TEST_CASE(preserves_mp3_in_mp4a_without_decoder_specific_configuration)
{
    for (u8 object_type : { 0x69, 0x6b }) {
        ByteBuffer bytes;
        append_audio_movie(bytes, 48'000, 48'000, 2, [](ByteBuffer& bytes) {
            auto esds = begin_box(bytes, "esds");
            append_full_box_header(bytes);
            bytes.append(3); // ES_Descriptor
            bytes.append(18);
            append_u16(bytes, 1); // ES_ID
            bytes.append(0);      // flags
            bytes.append(4);      // DecoderConfigDescriptor
            bytes.append(13);
            bytes.append(0x6b); // MPEG-1 Audio
            bytes.append(0x15); // audio stream type, reserved bit
            bytes.append(0);
            append_u16(bytes, 0); // bufferSizeDB
            append_u32(bytes, 160'000);
            append_u32(bytes, 160'000);
            finish_box(bytes, esds);
        });
        // The final DecoderConfigDescriptor has a 13-byte payload beginning with the object type.
        bytes[bytes.size() - 13] = object_type;
        auto movie = MUST(parse_movie(bytes));
        auto const& entry = movie.tracks.get(1).value()->sample_entries.first();
        EXPECT_EQ(entry.codec_id, Media::CodecID::MP3);
        EXPECT(entry.codec_initialization_data.is_empty());
        auto track = Media::ISOBMFF::track_from_track_entry(*movie.tracks.get(1).value(), true);
        EXPECT_EQ(track.parsed_codec().codec_id(), Media::CodecID::MP3);
    }
}

static void append_seek_test_tables(ByteBuffer& bytes, u8 size_width = 0, u32 data_offset = 8, bool mark_sync_samples = true)
{
    auto times = begin_box(bytes, "stts");
    append_full_box_header(bytes);
    append_u32(bytes, 1);
    append_u32(bytes, 6);
    append_u32(bytes, 1);
    finish_box(bytes, times);

    auto compositions = begin_box(bytes, "ctts");
    append_full_box_header(bytes, 1);
    append_u32(bytes, 6);
    for (i32 offset : { 0, 2, -1, -1, 2, -1 }) {
        append_u32(bytes, 1);
        append_u32(bytes, static_cast<u32>(offset));
    }
    finish_box(bytes, compositions);

    auto chunks = begin_box(bytes, "stsc");
    append_full_box_header(bytes);
    append_u32(bytes, 2);
    for (u32 chunk = 1; chunk <= 2; ++chunk) {
        append_u32(bytes, chunk);
        append_u32(bytes, 3);
        append_u32(bytes, 1);
    }
    finish_box(bytes, chunks);

    bool compact = size_width != 0 && size_width != 32;
    auto sizes = begin_box(bytes, compact ? "stz2" : "stsz");
    append_full_box_header(bytes);
    append_u32(bytes, compact ? size_width : size_width == 0 ? 1
                                                             : 0);
    append_u32(bytes, 6);
    if (size_width == 4) {
        bytes.append(0x12);
        bytes.append(0x34);
        bytes.append(0x56);
    } else if (size_width != 0) {
        for (u32 size = 1; size <= 6; ++size) {
            if (size_width == 8)
                bytes.append(size);
            else if (size_width == 16)
                append_u16(bytes, size);
            else
                append_u32(bytes, size);
        }
    }
    finish_box(bytes, sizes);

    auto offsets = begin_box(bytes, size_width == 0 ? "stco" : "co64");
    append_full_box_header(bytes);
    append_u32(bytes, 2);
    if (size_width == 0) {
        append_u32(bytes, data_offset);
        append_u32(bytes, data_offset + 3);
    } else {
        append_u64(bytes, 0x1'0000'0000);
        append_u64(bytes, 0x2'0000'0000);
    }
    finish_box(bytes, offsets);

    auto sync = begin_box(bytes, "stss");
    append_full_box_header(bytes);
    append_u32(bytes, mark_sync_samples ? 2 : 0);
    if (mark_sync_samples) {
        append_u32(bytes, 1);
        append_u32(bytes, 5);
    }
    finish_box(bytes, sync);
}

TEST_CASE(demuxes_quicktime_with_a_leading_wide_atom)
{
    ByteBuffer bytes;
    auto wide = begin_box(bytes, "wide");
    finish_box(bytes, wide);
    auto data = begin_box(bytes, "mdat");
    for (u8 i = 0; i < 6; ++i)
        bytes.append(i);
    finish_box(bytes, data);
    append_video_movie(bytes, "vp09", append_vp9_configuration_box, ColourBoxOrder::None, [](ByteBuffer& bytes) { append_seek_test_tables(bytes, 0, 16); });

    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(bytes);
    auto demuxer = MUST(Media::ISOBMFF::ISOBMFFDemuxer::from_stream(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).value();
    MUST(demuxer->create_context_for_track(track));
    for (u8 i = 0; i < 6; ++i) {
        auto sample = MUST(demuxer->get_next_sample_for_track(track));
        EXPECT_EQ(sample.data().size(), 1u);
        EXPECT_EQ(sample.data()[0], i);
    }
    auto end = demuxer->get_next_sample_for_track(track);
    EXPECT(end.is_error());
    EXPECT_EQ(end.error().category(), Media::DecoderErrorCategory::EndOfStream);
}

static ByteBuffer seek_test_file(bool fragmented = false, Optional<u32> fragment_decode_time = {}, bool movie_has_samples = true)
{
    ByteBuffer bytes;
    auto data = begin_box(bytes, "mdat");
    for (u8 i = 0; i < 6; ++i)
        bytes.append(i);
    finish_box(bytes, data);
    append_video_movie(bytes, "vp09", append_vp9_configuration_box, ColourBoxOrder::None, [&](ByteBuffer& bytes) {
        if (movie_has_samples)
            append_seek_test_tables(bytes); }, fragmented);
    if (fragmented) {
        auto fragment = begin_box(bytes, "moof");
        auto fragment_header = begin_box(bytes, "mfhd");
        append_full_box_header(bytes);
        append_u32(bytes, 1);
        finish_box(bytes, fragment_header);
        auto traf = begin_box(bytes, "traf");
        auto header = begin_box(bytes, "tfhd");
        append_full_box_header(bytes, 0, 0x020018); // moof-relative offsets, default duration and size
        append_u32(bytes, 1);
        append_u32(bytes, 1);
        append_u32(bytes, 1);
        finish_box(bytes, header);
        // Without tfdt, decoding continues after the samples in moov or the preceding fragment.
        if (fragment_decode_time.has_value()) {
            auto decode_time = begin_box(bytes, "tfdt");
            append_full_box_header(bytes);
            append_u32(bytes, fragment_decode_time.value());
            finish_box(bytes, decode_time);
        }
        auto run = begin_box(bytes, "trun");
        append_full_box_header(bytes, 1, 0x000c01); // data offset, flags, composition offsets
        append_u32(bytes, 2);
        auto data_offset_position = bytes.size();
        append_u32(bytes, 0);
        append_u32(bytes, 0);
        append_u32(bytes, 2);
        append_u32(bytes, Media::ISOBMFF::SAMPLE_IS_NON_SYNC_SAMPLE);
        append_u32(bytes, 0);
        finish_box(bytes, run);
        finish_box(bytes, traf);
        finish_box(bytes, fragment);
        patch_u32(bytes, data_offset_position, bytes.size() + 8 - fragment.start);
        auto fragment_data = begin_box(bytes, "mdat");
        bytes.append(6);
        bytes.append(7);
        finish_box(bytes, fragment_data);
    }
    return bytes;
}

TEST_CASE(demuxes_sample_tables_in_decode_order_with_signed_composition_offsets)
{
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(seek_test_file());
    auto demuxer = MUST(Media::ISOBMFF::ISOBMFFDemuxer::from_stream(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).value();
    MUST(demuxer->create_context_for_track(track));
    Array<i64, 6> timestamps { 0, 3, 1, 2, 6, 4 };
    for (size_t index = 0; index < timestamps.size(); ++index) {
        auto sample = MUST(demuxer->get_next_sample_for_track(track));
        EXPECT_EQ(sample.decode_timestamp(), AK::Duration::from_seconds(index));
        EXPECT_EQ(sample.presentation_timestamp(), AK::Duration::from_seconds(timestamps[index]));
        EXPECT_EQ(sample.data()[0], index);
        EXPECT_EQ(sample.is_keyframe(), index == 0 || index == 4);
        EXPECT_EQ(sample.new_codec_configuration().has_value(), index == 0);
    }
    EXPECT_EQ(demuxer->get_next_sample_for_track(track).error().category(), Media::DecoderErrorCategory::EndOfStream);
}

TEST_CASE(seeks_by_pts_and_skips_seeks_that_do_not_improve_position)
{
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(seek_test_file());
    auto demuxer = MUST(Media::ISOBMFF::ISOBMFFDemuxer::from_stream(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).value();
    MUST(demuxer->create_context_for_track(track));
    auto target = AK::Duration::from_seconds(5);
    EXPECT_EQ(MUST(demuxer->seek_to_most_recent_keyframe(track, target)), Media::DemuxerSeekResult::MovedPosition);
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).data()[0], 0);
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).data()[0], 1);
    EXPECT_EQ(MUST(demuxer->seek_to_most_recent_keyframe(track, target, Media::DemuxerSeekOptions::NeedCodecConfiguration)), Media::DemuxerSeekResult::KeptCurrentPosition);
    auto next = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(next.data()[0], 2);
    EXPECT(next.new_codec_configuration().has_value());
    EXPECT_EQ(MUST(demuxer->seek_to_most_recent_keyframe(track, target, Media::DemuxerSeekOptions::Force)), Media::DemuxerSeekResult::MovedPosition);
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).data()[0], 0);
    EXPECT_EQ(demuxer->select_fast_seek_target_for_track(track, target, Media::SeekMode::FastBefore), AK::Duration::zero());
    EXPECT_EQ(demuxer->select_fast_seek_target_for_track(track, target, Media::SeekMode::FastAfter), AK::Duration::from_seconds(6));
}

TEST_CASE(seeks_across_movie_and_fragment_samples_in_both_directions)
{
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(seek_test_file(true));
    auto demuxer = MUST(Media::ISOBMFF::ISOBMFFDemuxer::from_stream(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).value();
    MUST(demuxer->create_context_for_track(track));
    for (u8 index = 0; index < 8; ++index) {
        auto sample = MUST(demuxer->get_next_sample_for_track(track));
        EXPECT_EQ(sample.data()[0], index);
        EXPECT_EQ(sample.decode_timestamp(), AK::Duration::from_seconds(index));
    }
    struct SeekExpectation {
        i64 target;
        u8 sample_index;
    };
    for (auto [target, sample_index] : { SeekExpectation { 8, 6 }, SeekExpectation { 5, 0 }, SeekExpectation { 7, 4 }, SeekExpectation { 10, 6 } }) {
        MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_seconds(target), Media::DemuxerSeekOptions::Force));
        EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).data()[0], sample_index);
    }
}

TEST_CASE(sample_tables_remain_available_after_movie_bytes_are_evicted)
{
    auto bytes = seek_test_file();
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(bytes);
    auto demuxer = MUST(Media::ISOBMFF::ISOBMFFDemuxer::from_stream(stream));
    // Retain only the mdat. Creating iterators and seeking must use the owned table bytes.
    stream->remove_byte_range(14, bytes.size());
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).value();
    MUST(demuxer->create_context_for_track(track));
    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_seconds(6)));
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).data()[0], 4);
}

TEST_CASE(rejects_sample_table_counts_larger_than_box_before_allocating)
{
    constexpr char types[][5] = { "stts", "ctts", "stsc", "stco", "co64", "stss", "stsz", "stz2" };
    for (auto const& type : types) {
        ByteBuffer bytes;
        append_video_movie(bytes, "vp09", append_vp9_configuration_box, ColourBoxOrder::None, [&](ByteBuffer& bytes) {
            auto box = begin_box(bytes, type);
            append_full_box_header(bytes);
            if (StringView(type, 4) == "stsz"sv)
                append_u32(bytes, 0); // variable sample sizes
            else if (StringView(type, 4) == "stz2"sv)
                append_u32(bytes, 4); // four-bit sample sizes
            append_u32(bytes, NumericLimits<u32>::max());
            finish_box(bytes, box);
        });
        EXPECT(parse_movie(bytes).is_error());
    }
}

TEST_CASE(iterates_compact_and_variable_sample_sizes_with_large_chunk_offsets)
{
    for (u8 width : { 4, 8, 16, 32 }) {
        ByteBuffer bytes;
        append_video_movie(bytes, "vp09", append_vp9_configuration_box, ColourBoxOrder::None, [&](ByteBuffer& bytes) {
            append_seek_test_tables(bytes, width);
        });
        auto movie = MUST(parse_movie(bytes));
        auto& track = *movie.tracks.get(1).value();
        EXPECT_EQ(track.sample_tables->sample_sizes->sizes.size(), 6u);
        track.sample_tables->sync_samples.clear();
        auto iterator = MUST(Media::ISOBMFF::SampleTableIterator::create(track));
        size_t expected_position = 0x1'0000'0000;
        for (u32 index = 0; index < 6; ++index) {
            if (index == 3)
                expected_position = 0x2'0000'0000;
            auto sample = MUST(iterator.next(track)).value();
            EXPECT_EQ(sample.data_size, index + 1);
            EXPECT_EQ(sample.data_position, expected_position);
            EXPECT(sample.is_sync_sample);
            expected_position += index + 1;
        }
        EXPECT(!MUST(iterator.next(track)).has_value());
    }
}

TEST_CASE(normalizes_composition_offset_versions_without_losing_unsigned_values)
{
    for (u8 version : { 0, 1 }) {
        ByteBuffer bytes;
        append_video_movie(bytes, "vp09", append_vp9_configuration_box, ColourBoxOrder::None, [&](ByteBuffer& bytes) {
            auto box = begin_box(bytes, "ctts");
            append_full_box_header(bytes, version);
            append_u32(bytes, 1);
            append_u32(bytes, 100);
            append_u32(bytes, 0xffff'ffff);
            finish_box(bytes, box);
        });
        auto movie = MUST(parse_movie(bytes));
        auto const& offsets = movie.tracks.get(1).value()->sample_tables->composition_offsets.value();
        EXPECT_EQ(offsets.size(), 1u);
        EXPECT_EQ(offsets[0].sample_count, 100u);
        EXPECT_EQ(offsets[0].sample_offset, version == 0 ? 0xffff'ffffLL : -1LL);
    }
}

TEST_CASE(parses_odd_compact_sample_count_and_keeps_constant_sample_sizes_compressed)
{
    for (bool compact : { false, true }) {
        ByteBuffer bytes;
        append_video_movie(bytes, "vp09", append_vp9_configuration_box, ColourBoxOrder::None, [&](ByteBuffer& bytes) {
            auto box = begin_box(bytes, compact ? "stz2" : "stsz");
            append_full_box_header(bytes);
            append_u32(bytes, compact ? 4 : 123);
            append_u32(bytes, compact ? 3 : NumericLimits<u32>::max());
            if (compact) {
                bytes.append(0x12);
                bytes.append(0x3f); // The unused low nibble must not become another sample.
            }
            finish_box(bytes, box);
        });
        auto movie = MUST(parse_movie(bytes));
        auto const& sizes = movie.tracks.get(1).value()->sample_tables->sample_sizes.value();
        if (compact) {
            EXPECT_EQ(sizes.sizes.size(), 3u);
            EXPECT_EQ(sizes.sample_count, 3u);
            for (u32 index = 0; index < 3; ++index)
                EXPECT_EQ(sizes.size_of_sample(index), index + 1);
        } else {
            EXPECT(sizes.sizes.is_empty());
            EXPECT_EQ(sizes.sample_count, NumericLimits<u32>::max());
            EXPECT_EQ(sizes.size_of_sample(sizes.sample_count - 1), 123u);
        }
    }
}

TEST_CASE(retries_incomplete_fragment_metadata_without_losing_samples)
{
    auto bytes = seek_test_file(true);
    auto reader = streamer_for(bytes);
    MUST(reader.seek_to_position(14));
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(reader));
    auto movie = MUST(Media::ISOBMFF::Reader::parse_movie_box(reader, header));
    auto movie_end = header.end_position().value();
    auto source = Media::IncrementallyPopulatedStream::create_empty();
    source->set_expected_size(bytes.size());
    source->add_chunk_at(0, bytes.bytes().trim(movie_end + 16));
    auto cursor = source->create_cursor();
    cursor->set_is_blocking(false);
    auto iterator = MUST(Media::ISOBMFF::SampleIterator::create(cursor, movie, 1, movie_end));
    for (u32 index = 0; index < 6; ++index)
        EXPECT_EQ(MUST(iterator.next())->decode_timestamp, AK::Duration::from_seconds(index));
    EXPECT_EQ(iterator.next().error().category(), Media::DecoderErrorCategory::NeedsMoreInput);
    source->add_chunk_at(movie_end + 16, bytes.bytes().slice(movie_end + 16));
    for (u32 index = 6; index < 8; ++index)
        EXPECT_EQ(MUST(iterator.next())->decode_timestamp, AK::Duration::from_seconds(index));
    EXPECT(!MUST(iterator.next()).has_value());
}

TEST_CASE(seeking_movie_samples_does_not_wait_for_later_fragments)
{
    auto bytes = seek_test_file(true);
    auto reader = streamer_for(bytes);
    MUST(reader.seek_to_position(14));
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(reader));
    auto source = Media::IncrementallyPopulatedStream::create_empty();
    source->set_expected_size(bytes.size());
    source->add_chunk_at(0, bytes.bytes().trim(header.end_position().value() + 16));
    auto demuxer = MUST(Media::ISOBMFF::ISOBMFFDemuxer::from_stream(source));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).value();
    MUST(demuxer->create_context_for_track(track));
    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_seconds(5)));
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).data()[0], 0);
}

TEST_CASE(aborted_sample_iteration_can_be_resumed)
{
    auto source = Media::IncrementallyPopulatedStream::create_from_buffer(seek_test_file(true));
    auto demuxer = MUST(Media::ISOBMFF::ISOBMFFDemuxer::from_stream(source));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).value();
    MUST(demuxer->create_context_for_track(track));
    for (u32 index = 0; index < 8; ++index) {
        demuxer->set_blocking_reads_aborted_for_track(track);
        EXPECT_EQ(demuxer->get_next_sample_for_track(track).error().category(), Media::DecoderErrorCategory::Aborted);
        demuxer->reset_blocking_reads_aborted_for_track(track);
        EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).data()[0], index);
    }
}

TEST_CASE(fragment_seeks_block_until_the_target_metadata_arrives_and_can_be_aborted)
{
    for (bool movie_has_samples : { false, true }) {
        auto bytes = seek_test_file(true, {}, movie_has_samples);
        auto reader = streamer_for(bytes);
        MUST(reader.seek_to_position(14));
        auto header = MUST(Media::ISOBMFF::Reader::read_box_header(reader));
        auto available_end = header.end_position().value() + 16;
        auto source = Media::IncrementallyPopulatedStream::create_empty();
        // Leave a later box unavailable, so seeking must stop before reaching the end of the file.
        source->set_expected_size(bytes.size() + 8);
        source->add_chunk_at(0, bytes.bytes().trim(available_end));
        auto demuxer = MUST(Media::ISOBMFF::ISOBMFFDemuxer::from_stream(source));
        auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).value();
        MUST(demuxer->create_context_for_track(track));
        auto target = AK::Duration::from_milliseconds(movie_has_samples ? 8500 : 2500);
        EXPECT_EQ(demuxer->select_fast_seek_target_for_track(track, target, Media::SeekMode::FastBefore), movie_has_samples ? AK::Duration::from_seconds(6) : target);

        IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> blocked { false };
        IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> completed { false };
        demuxer->set_read_blocked_change_handler_for_track(track, [&](Media::ReadBlocked state) {
            blocked = state == Media::ReadBlocked::Yes;
        });
        for (bool abort : { true, false }) {
            completed = false;
            demuxer->reset_blocking_reads_aborted_for_track(track);
            auto thread = Threading::Thread::construct("TestSeek"sv, [&, demuxer, track, target, abort]() -> intptr_t {
                auto result = demuxer->seek_to_most_recent_keyframe(track, target);
                if (abort) {
                    EXPECT(result.is_error());
                    if (result.is_error())
                        EXPECT_EQ(result.error().category(), Media::DecoderErrorCategory::Aborted);
                } else {
                    EXPECT(!result.is_error());
                }
                completed = true;
                return 0;
            });
            thread->start();
            auto deadline = MonotonicTime::now() + AK::Duration::from_seconds(5);
            while (!blocked.load() && !completed.load() && MonotonicTime::now() < deadline) { }
            EXPECT(blocked.load());
            EXPECT(!completed.load());
            if (abort)
                demuxer->set_blocking_reads_aborted_for_track(track);
            else
                source->add_chunk_at(available_end, bytes.bytes().slice(available_end));
            MUST(thread->join());
        }
        EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).data()[0], 6);
        demuxer->set_read_blocked_change_handler_for_track(track, {});
    }
}

TEST_CASE(seeking_uses_the_first_sample_when_no_samples_are_marked_sync)
{
    ByteBuffer bytes;
    auto data = begin_box(bytes, "mdat");
    for (u8 i = 0; i < 6; ++i)
        bytes.append(i);
    finish_box(bytes, data);
    append_video_movie(bytes, "vp09", append_vp9_configuration_box, ColourBoxOrder::None, [](ByteBuffer& bytes) { append_seek_test_tables(bytes, 0, 8, false); });
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(bytes);
    auto demuxer = MUST(Media::ISOBMFF::ISOBMFFDemuxer::from_stream(stream));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).value();
    MUST(demuxer->create_context_for_track(track));
    for (auto target : { 0, 2, 5, 10 }) {
        MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_seconds(target), Media::DemuxerSeekOptions::Force));
        EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).data()[0], 0);
    }
}

TEST_CASE(explicit_fragment_decode_time_overrides_movie_duration)
{
    auto source = Media::IncrementallyPopulatedStream::create_from_buffer(seek_test_file(true, 100));
    auto demuxer = MUST(Media::ISOBMFF::ISOBMFFDemuxer::from_stream(source));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).value();
    MUST(demuxer->create_context_for_track(track));
    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_seconds(101), Media::DemuxerSeekOptions::Force));
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).data()[0], 4);
    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_seconds(102), Media::DemuxerSeekOptions::Force));
    auto sample = MUST(demuxer->get_next_sample_for_track(track));
    EXPECT_EQ(sample.decode_timestamp(), AK::Duration::from_seconds(100));
    EXPECT_EQ(sample.presentation_timestamp(), AK::Duration::from_seconds(102));
}

TEST_CASE(fragment_decode_time_continues_across_successive_fragments)
{
    auto bytes = seek_test_file(true);
    auto reader = streamer_for(bytes);
    MUST(reader.seek_to_position(14));
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(reader));
    auto fragment_bytes = MUST(ByteBuffer::copy(bytes.bytes().slice(header.end_position().value())));
    patch_u32(fragment_bytes, 20, 2); // mfhd sequence_number
    MUST(bytes.try_append(fragment_bytes.bytes()));
    auto source = Media::IncrementallyPopulatedStream::create_from_buffer(bytes);
    auto demuxer = MUST(Media::ISOBMFF::ISOBMFFDemuxer::from_stream(source));
    auto track = MUST(demuxer->get_preferred_track_for_type(Media::TrackType::Video)).value();
    MUST(demuxer->create_context_for_track(track));
    for (u32 index = 0; index < 10; ++index)
        EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).decode_timestamp(), AK::Duration::from_seconds(index));
    MUST(demuxer->seek_to_most_recent_keyframe(track, AK::Duration::from_seconds(10), Media::DemuxerSeekOptions::Force));
    EXPECT_EQ(MUST(demuxer->get_next_sample_for_track(track)).decode_timestamp(), AK::Duration::from_seconds(8));
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

TEST_CASE(rejects_oversized_sample_table_entry_count)
{
    ByteBuffer bytes;
    append_movie_box(bytes, true);

    auto streamer = streamer_for(bytes);
    auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
    EXPECT(Media::ISOBMFF::Reader::parse_movie_box(streamer, header).is_error());
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

TEST_CASE(uses_first_edit_list_entry_for_presentation_offset)
{
    for (u8 version : { 0, 1 }) {
        for (i64 first_media_time : { -1, 123 }) {
            ByteBuffer bytes;
            auto movie_box = begin_box(bytes, "moov");
            auto track_start = bytes.size();
            append_minimal_track(bytes);
            auto edit = begin_box(bytes, "edts");
            auto edit_list = begin_box(bytes, "elst");
            append_full_box_header(bytes, version);
            append_u32(bytes, 2);
            for (i64 media_time : { first_media_time, static_cast<i64>(456) }) {
                if (version == 0) {
                    append_u32(bytes, 125);
                    append_u32(bytes, static_cast<u32>(media_time));
                } else {
                    append_u64(bytes, 125);
                    append_u64(bytes, static_cast<u64>(media_time));
                }
                append_u16(bytes, 1);
                append_u16(bytes, 0);
            }
            finish_box(bytes, edit_list);
            finish_box(bytes, edit);
            patch_u32(bytes, track_start, bytes.size() - track_start);
            finish_box(bytes, movie_box);

            auto streamer = streamer_for(bytes);
            auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
            auto movie = MUST(Media::ISOBMFF::Reader::parse_movie_box(streamer, header));
            auto offset = movie.tracks.get(1).value()->composition_to_presentation_offset;
            EXPECT_EQ(offset, first_media_time < 0 ? 0 : -first_media_time);
            EXPECT_EQ(streamer.position(), bytes.size());
        }
    }
}

TEST_CASE(rejects_truncated_multi_entry_edit_list)
{
    for (u8 version : { 0, 1 }) {
        ByteBuffer bytes;
        auto movie_box = begin_box(bytes, "moov");
        auto track_start = bytes.size();
        append_minimal_track(bytes);
        auto edit = begin_box(bytes, "edts");
        auto edit_list = begin_box(bytes, "elst");
        append_full_box_header(bytes, version);
        append_u32(bytes, 2);
        append_u64(bytes, 0);
        append_u32(bytes, 0);
        finish_box(bytes, edit_list);
        finish_box(bytes, edit);
        patch_u32(bytes, track_start, bytes.size() - track_start);
        finish_box(bytes, movie_box);

        auto streamer = streamer_for(bytes);
        auto header = MUST(Media::ISOBMFF::Reader::read_box_header(streamer));
        EXPECT(Media::ISOBMFF::Reader::parse_movie_box(streamer, header).is_error());
    }
}

TEST_CASE(an_audio_track_carrying_no_codec_configuration_is_described_by_its_own_fields)
{
    // A track whose Elementary Stream Descriptor Box is absent states its sample rate and channel
    // count and nothing of the codec that decodes it.
    ByteBuffer bytes;
    append_audio_movie(bytes, 48'000, 48'000, 2);
    auto movie = MUST(parse_movie(bytes));

    auto const& entries = movie.tracks.get(1).value()->sample_entries;
    EXPECT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].codec_id, Media::CodecID::AAC);

    // An Audio Specific Config of object type 2, sampling frequency index 3 and channel
    // configuration 2: Low Complexity AAC at the 48000 Hz stereo the track declares.
    EXPECT_EQ(entries[0].codec_initialization_data.size(), 2u);
    EXPECT_EQ(entries[0].codec_initialization_data[0], 0x11);
    EXPECT_EQ(entries[0].codec_initialization_data[1], 0x90);
}

TEST_CASE(an_audio_track_naming_twice_its_timescale_is_described_as_high_efficiency)
{
    // Every AAC frame carries 1024 samples of the rate its core is coded at, and a media timescale
    // counts in those samples, so this track decodes to twice the rate it is coded at.
    ByteBuffer bytes;
    append_audio_movie(bytes, 24'000, 48'000, 2);
    auto movie = MUST(parse_movie(bytes));

    auto const& entries = movie.tracks.get(1).value()->sample_entries;
    EXPECT_EQ(entries.size(), 1u);

    // A 24000 Hz core, followed by the sync extension naming Spectral Band Replication at 48000 Hz.
    EXPECT_EQ(entries[0].codec_initialization_data.size(), 5u);
    EXPECT_EQ(entries[0].codec_initialization_data[0], 0x13);
    EXPECT_EQ(entries[0].codec_initialization_data[1], 0x10);
    EXPECT_EQ(entries[0].codec_initialization_data[2], 0x56);
    EXPECT_EQ(entries[0].codec_initialization_data[3], 0xe5);
    EXPECT_EQ(entries[0].codec_initialization_data[4], 0x98);
}
