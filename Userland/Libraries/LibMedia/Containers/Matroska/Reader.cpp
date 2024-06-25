/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022-2023, Gregory Bertilson <Zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Function.h>
#include <AK/Math.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Utf8View.h>
#include <LibCore/MappedFile.h>

#include "Reader.h"

namespace Media::Matroska {

#define TRY_READ(expression) DECODER_TRY(DecoderErrorCategory::Corrupted, expression)

// RFC 8794 - Extensible Binary Meta Language
// https://datatracker.ietf.org/doc/html/rfc8794
constexpr u32 EBML_MASTER_ELEMENT_ID = 0x1A45DFA3;
constexpr u32 EBML_CRC32_ELEMENT_ID = 0xBF;
constexpr u32 EBML_VOID_ELEMENT_ID = 0xEC;

// Matroska elements' IDs and types are listed at this URL:
// https://www.matroska.org/technical/elements.html
constexpr u32 SEGMENT_ELEMENT_ID = 0x18538067;
constexpr u32 DOCTYPE_ELEMENT_ID = 0x4282;
constexpr u32 DOCTYPE_VERSION_ELEMENT_ID = 0x4287;

constexpr u32 SEEK_HEAD_ELEMENT_ID = 0x114D9B74;
constexpr u32 SEEK_ELEMENT_ID = 0x4DBB;
constexpr u32 SEEK_ID_ELEMENT_ID = 0x53AB;
constexpr u32 SEEK_POSITION_ELEMENT_ID = 0x53AC;

constexpr u32 SEGMENT_INFORMATION_ELEMENT_ID = 0x1549A966;
constexpr u32 TRACK_ELEMENT_ID = 0x1654AE6B;
constexpr u32 CLUSTER_ELEMENT_ID = 0x1F43B675;
constexpr u32 TIMESTAMP_SCALE_ID = 0x2AD7B1;
constexpr u32 MUXING_APP_ID = 0x4D80;
constexpr u32 WRITING_APP_ID = 0x5741;
constexpr u32 DURATION_ID = 0x4489;

// Tracks
constexpr u32 TRACK_ENTRY_ID = 0xAE;
constexpr u32 TRACK_NUMBER_ID = 0xD7;
constexpr u32 TRACK_UID_ID = 0x73C5;
constexpr u32 TRACK_TYPE_ID = 0x83;
constexpr u32 TRACK_LANGUAGE_ID = 0x22B59C;
constexpr u32 TRACK_CODEC_ID = 0x86;
constexpr u32 TRACK_CODEC_PRIVATE = 0x63A2;
constexpr u32 TRACK_TIMESTAMP_SCALE_ID = 0x23314F;
constexpr u32 TRACK_OFFSET_ID = 0x537F;
constexpr u32 TRACK_VIDEO_ID = 0xE0;
constexpr u32 TRACK_AUDIO_ID = 0xE1;

// Video
constexpr u32 PIXEL_WIDTH_ID = 0xB0;
constexpr u32 PIXEL_HEIGHT_ID = 0xBA;
constexpr u32 COLOR_ENTRY_ID = 0x55B0;
constexpr u32 PRIMARIES_ID = 0x55BB;
constexpr u32 TRANSFER_CHARACTERISTICS_ID = 0x55BA;
constexpr u32 MATRIX_COEFFICIENTS_ID = 0x55B1;
constexpr u32 RANGE_ID = 0x55B9;
constexpr u32 BITS_PER_CHANNEL_ID = 0x55B2;

// Audio
constexpr u32 CHANNELS_ID = 0x9F;
constexpr u32 BIT_DEPTH_ID = 0x6264;

// Clusters
constexpr u32 SIMPLE_BLOCK_ID = 0xA3;
constexpr u32 TIMESTAMP_ID = 0xE7;

// Cues
constexpr u32 CUES_ID = 0x1C53BB6B;
constexpr u32 CUE_POINT_ID = 0xBB;
constexpr u32 CUE_TIME_ID = 0xB3;
constexpr u32 CUE_TRACK_POSITIONS_ID = 0xB7;
constexpr u32 CUE_TRACK_ID = 0xF7;
constexpr u32 CUE_CLUSTER_POSITION_ID = 0xF1;
constexpr u32 CUE_RELATIVE_POSITION_ID = 0xF0;
constexpr u32 CUE_CODEC_STATE_ID = 0xEA;
constexpr u32 CUE_REFERENCE_ID = 0xDB;

DecoderErrorOr<Reader> Reader::from_file(StringView path)
{
    auto mapped_file = DECODER_TRY(DecoderErrorCategory::IO, Core::MappedFile::map(path));
    return from_mapped_file(move(mapped_file));
}

DecoderErrorOr<Reader> Reader::from_mapped_file(NonnullOwnPtr<Core::MappedFile> mapped_file)
{
    auto reader = TRY(from_data(mapped_file->bytes()));
    reader.m_mapped_file = make_ref_counted<Core::SharedMappedFile>(move(mapped_file));
    return reader;
}

DecoderErrorOr<Reader> Reader::from_data(ReadonlyBytes data)
{
    Reader reader(data);
    TRY(reader.parse_initial_data());
    return reader;
}

// Returns the position of the first element that is read from this master element.
static DecoderErrorOr<size_t> parse_master_element(Streamer& streamer, [[maybe_unused]] StringView element_name, Function<DecoderErrorOr<IterationDecision>(u64)> element_consumer)
{
    auto element_data_size = TRY_READ(streamer.read_variable_size_integer());
    dbgln_if(MATROSKA_DEBUG, "{} has {} octets of data.", element_name, element_data_size);

    bool first_element = true;
    auto first_element_position = streamer.position();

    streamer.push_octets_read();
    while (streamer.octets_read() < element_data_size) {
        dbgln_if(MATROSKA_TRACE_DEBUG, "====== Reading  element ======");
        auto element_id = TRY_READ(streamer.read_variable_size_integer(false));
        dbgln_if(MATROSKA_TRACE_DEBUG, "{:s} element ID is {:#010x}", element_name, element_id);

        if (element_id == EBML_CRC32_ELEMENT_ID) {
            // The CRC-32 Element contains a 32-bit Cyclic Redundancy Check value of all the
            // Element Data of the Parent Element as stored except for the CRC-32 Element itself.
            // When the CRC-32 Element is present, the CRC-32 Element MUST be the first ordered
            // EBML Element within its Parent Element for easier reading.
            if (!first_element)
                return DecoderError::corrupted("CRC32 element must be the first child"sv);

            // All Top-Level Elements of an EBML Document that are Master Elements SHOULD include a
            // CRC-32 Element as a Child Element. The CRC in use is the IEEE-CRC-32 algorithm as used
            // in the [ISO3309] standard and in Section 8.1.1.6.2 of [ITU.V42], with initial value of
            // 0xFFFFFFFF. The CRC value MUST be computed on a little-endian bytestream and MUST use
            // little-endian storage.

            // FIXME: Currently we skip the CRC-32 Element instead of checking it. It may be worth
            //        verifying the contents of the SeekHead, Segment Info, and Tracks Elements.
            //        Note that Cluster Elements tend to be quite large, so verifying their integrity
            //        will result in longer buffering times in streamed contexts, so it may not be
            //        worth the effort checking those. It would also prevent error correction in
            //        video codecs from taking effect.
            TRY_READ(streamer.read_unknown_element());
            continue;
        }
        if (element_id == EBML_VOID_ELEMENT_ID) {
            // Used to void data or to avoid unexpected behaviors when using damaged data.
            // The content is discarded. Also used to reserve space in a subelement for later use.
            TRY_READ(streamer.read_unknown_element());
            continue;
        }

        auto result = element_consumer(element_id);
        if (result.is_error())
            return DecoderError::format(result.error().category(), "{} -> {}", element_name, result.error().description());
        if (result.release_value() == IterationDecision::Break)
            break;

        dbgln_if(MATROSKA_TRACE_DEBUG, "Read {} octets of the {} so far.", streamer.octets_read(), element_name);
        first_element = false;
    }
    streamer.pop_octets_read();

    return first_element_position;
}

static DecoderErrorOr<EBMLHeader> parse_ebml_header(Streamer& streamer)
{
    EBMLHeader header;
    TRY(parse_master_element(streamer, "Header"sv, [&](u64 element_id) -> DecoderErrorOr<IterationDecision> {
        switch (element_id) {
        case DOCTYPE_ELEMENT_ID:
            header.doc_type = TRY_READ(streamer.read_string());
            dbgln_if(MATROSKA_DEBUG, "Read DocType attribute: {}", header.doc_type);
            break;
        case DOCTYPE_VERSION_ELEMENT_ID:
            header.doc_type_version = TRY_READ(streamer.read_u64());
            dbgln_if(MATROSKA_DEBUG, "Read DocTypeVersion attribute: {}", header.doc_type_version);
            break;
        default:
            TRY_READ(streamer.read_unknown_element());
        }

        return IterationDecision::Continue;
    }));

    return header;
}

DecoderErrorOr<void> Reader::parse_initial_data()
{
    Streamer streamer { m_data };
    auto first_element_id = TRY_READ(streamer.read_variable_size_integer(false));
    dbgln_if(MATROSKA_TRACE_DEBUG, "First element ID is {:#010x}\n", first_element_id);
    if (first_element_id != EBML_MASTER_ELEMENT_ID)
        return DecoderError::corrupted("First element was not an EBML header"sv);

    m_header = TRY(parse_ebml_header(streamer));
    dbgln_if(MATROSKA_DEBUG, "Parsed EBML header");

    auto root_element_id = TRY_READ(streamer.read_variable_size_integer(false));
    if (root_element_id != SEGMENT_ELEMENT_ID)
        return DecoderError::corrupted("Second element was not a segment element"sv);

    m_segment_contents_size = TRY_READ(streamer.read_variable_size_integer());
    m_segment_contents_position = streamer.position();
    dbgln_if(MATROSKA_TRACE_DEBUG, "Segment is at {} with size {}, available size is {}", m_segment_contents_position, m_segment_contents_size, m_data.size() - m_segment_contents_position);
    m_segment_contents_size = min(m_segment_contents_size, m_data.size() - m_segment_contents_position);
    return {};
}

static DecoderErrorOr<void> parse_seek_head(Streamer& streamer, size_t base_position, HashMap<u32, size_t>& table)
{
    TRY(parse_master_element(streamer, "SeekHead"sv, [&](u64 seek_head_child_id) -> DecoderErrorOr<IterationDecision> {
        if (seek_head_child_id == SEEK_ELEMENT_ID) {
            Optional<u64> seek_id;
            Optional<u64> seek_position;
            TRY(parse_master_element(streamer, "Seek"sv, [&](u64 seek_entry_child_id) -> DecoderErrorOr<IterationDecision> {
                switch (seek_entry_child_id) {
                case SEEK_ID_ELEMENT_ID:
                    seek_id = TRY_READ(streamer.read_u64());
                    dbgln_if(MATROSKA_TRACE_DEBUG, "Read Seek Element ID value {:#010x}", seek_id.value());
                    break;
                case SEEK_POSITION_ELEMENT_ID:
                    seek_position = TRY_READ(streamer.read_u64());
                    dbgln_if(MATROSKA_TRACE_DEBUG, "Read Seek Position value {}", seek_position.value());
                    break;
                default:
                    TRY_READ(streamer.read_unknown_element());
                }

                return IterationDecision::Continue;
            }));

            if (!seek_id.has_value())
                return DecoderError::corrupted("Seek entry is missing the element ID"sv);
            if (!seek_position.has_value())
                return DecoderError::corrupted("Seek entry is missing the seeking position"sv);
            if (seek_id.value() > NumericLimits<u32>::max())
                return DecoderError::corrupted("Seek entry's element ID is too large"sv);

            dbgln_if(MATROSKA_TRACE_DEBUG, "Seek entry found with ID {:#010x} and position {} offset from SeekHead at {}", seek_id.value(), seek_position.value(), base_position);
            // FIXME: SeekHead can reference another SeekHead, we should recursively parse all SeekHeads.

            if (table.contains(seek_id.value())) {
                dbgln_if(MATROSKA_DEBUG, "Warning: Duplicate seek entry with ID {:#010x} at position {}", seek_id.value(), seek_position.value());
                return IterationDecision::Continue;
            }

            DECODER_TRY_ALLOC(table.try_set(seek_id.release_value(), base_position + seek_position.release_value()));
        } else {
            dbgln_if(MATROSKA_TRACE_DEBUG, "Unknown SeekHead child element ID {:#010x}", seek_head_child_id);
        }

        return IterationDecision::Continue;
    }));
    return {};
}

DecoderErrorOr<Optional<size_t>> Reader::find_first_top_level_element_with_id([[maybe_unused]] StringView element_name, u32 element_id)
{
    dbgln_if(MATROSKA_DEBUG, "====== Finding element {} with ID {:#010x} ======", element_name, element_id);

    if (m_seek_entries.contains(element_id)) {
        dbgln_if(MATROSKA_TRACE_DEBUG, "Cache hit!");
        return m_seek_entries.get(element_id).release_value();
    }

    Streamer streamer { m_data };
    if (m_last_top_level_element_position != 0)
        TRY_READ(streamer.seek_to_position(m_last_top_level_element_position));
    else
        TRY_READ(streamer.seek_to_position(m_segment_contents_position));

    Optional<size_t> position;

    while (streamer.position() < m_segment_contents_position + m_segment_contents_size) {
        auto found_element_id = TRY_READ(streamer.read_variable_size_integer(false));
        auto found_element_position = streamer.position();
        dbgln_if(MATROSKA_TRACE_DEBUG, "Found element ID {:#010x} with position {}.", found_element_id, found_element_position);

        if (found_element_id == SEEK_HEAD_ELEMENT_ID) {
            dbgln_if(MATROSKA_TRACE_DEBUG, "Found SeekHead, parsing it into the lookup table.");
            m_seek_entries.clear();
            TRY(parse_seek_head(streamer, found_element_position, m_seek_entries));
            m_last_top_level_element_position = 0;
            if (m_seek_entries.contains(element_id)) {
                dbgln_if(MATROSKA_TRACE_DEBUG, "SeekHead hit!");
                position = m_seek_entries.get(element_id).release_value();
                break;
            }
            continue;
        }

        auto result = streamer.read_unknown_element();
        if (result.is_error())
            return DecoderError::format(DecoderErrorCategory::Corrupted, "While seeking to {}: {}", element_name, result.release_error().string_literal());

        m_last_top_level_element_position = streamer.position();

        DECODER_TRY_ALLOC(m_seek_entries.try_set(found_element_id, found_element_position));

        if (found_element_id == element_id) {
            position = found_element_position;
            break;
        }

        dbgln_if(MATROSKA_TRACE_DEBUG, "Skipped to position {}.", m_last_top_level_element_position);
    }

    return position;
}

static DecoderErrorOr<SegmentInformation> parse_information(Streamer& streamer)
{
    SegmentInformation segment_information;
    TRY(parse_master_element(streamer, "Segment Information"sv, [&](u64 element_id) -> DecoderErrorOr<IterationDecision> {
        switch (element_id) {
        case TIMESTAMP_SCALE_ID:
            segment_information.set_timestamp_scale(TRY_READ(streamer.read_u64()));
            dbgln_if(MATROSKA_DEBUG, "Read TimestampScale attribute: {}", segment_information.timestamp_scale());
            break;
        case MUXING_APP_ID:
            segment_information.set_muxing_app(TRY_READ(streamer.read_string()));
            dbgln_if(MATROSKA_DEBUG, "Read MuxingApp attribute: {}", segment_information.muxing_app().as_string());
            break;
        case WRITING_APP_ID:
            segment_information.set_writing_app(TRY_READ(streamer.read_string()));
            dbgln_if(MATROSKA_DEBUG, "Read WritingApp attribute: {}", segment_information.writing_app().as_string());
            break;
        case DURATION_ID:
            segment_information.set_duration_unscaled(TRY_READ(streamer.read_float()));
            dbgln_if(MATROSKA_DEBUG, "Read Duration attribute: {}", segment_information.duration_unscaled().value());
            break;
        default:
            TRY_READ(streamer.read_unknown_element());
        }

        return IterationDecision::Continue;
    }));

    return segment_information;
}

DecoderErrorOr<SegmentInformation> Reader::segment_information()
{
    if (m_segment_information.has_value())
        return m_segment_information.value();

    auto position = TRY(find_first_top_level_element_with_id("Segment Information"sv, SEGMENT_INFORMATION_ELEMENT_ID));
    if (!position.has_value())
        return DecoderError::corrupted("No Segment Information element found"sv);
    Streamer streamer { m_data };
    TRY_READ(streamer.seek_to_position(position.release_value()));
    m_segment_information = TRY(parse_information(streamer));
    return m_segment_information.value();
}

DecoderErrorOr<void> Reader::ensure_tracks_are_parsed()
{
    if (!m_tracks.is_empty())
        return {};
    auto position = TRY(find_first_top_level_element_with_id("Tracks"sv, TRACK_ELEMENT_ID));
    if (!position.has_value())
        return DecoderError::corrupted("No Tracks element found"sv);
    Streamer streamer { m_data };
    TRY_READ(streamer.seek_to_position(position.release_value()));
    TRY(parse_tracks(streamer));
    return {};
}

static DecoderErrorOr<TrackEntry::ColorFormat> parse_video_color_information(Streamer& streamer)
{
    TrackEntry::ColorFormat color_format {};

    TRY(parse_master_element(streamer, "Colour"sv, [&](u64 element_id) -> DecoderErrorOr<IterationDecision> {
        switch (element_id) {
        case PRIMARIES_ID:
            color_format.color_primaries = static_cast<ColorPrimaries>(TRY_READ(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Colour's Primaries attribute: {}", color_primaries_to_string(color_format.color_primaries));
            break;
        case TRANSFER_CHARACTERISTICS_ID:
            color_format.transfer_characteristics = static_cast<TransferCharacteristics>(TRY_READ(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Colour's TransferCharacteristics attribute: {}", transfer_characteristics_to_string(color_format.transfer_characteristics));
            break;
        case MATRIX_COEFFICIENTS_ID:
            color_format.matrix_coefficients = static_cast<MatrixCoefficients>(TRY_READ(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Colour's MatrixCoefficients attribute: {}", matrix_coefficients_to_string(color_format.matrix_coefficients));
            break;
        case RANGE_ID:
            color_format.range = static_cast<TrackEntry::ColorRange>(TRY_READ(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Colour's Range attribute: {}", to_underlying(color_format.range));
            break;
        case BITS_PER_CHANNEL_ID:
            color_format.bits_per_channel = TRY_READ(streamer.read_u64());
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Colour's BitsPerChannel attribute: {}", color_format.bits_per_channel);
            break;
        default:
            TRY_READ(streamer.read_unknown_element());
        }

        return IterationDecision::Continue;
    }));

    return color_format;
}

static DecoderErrorOr<TrackEntry::VideoTrack> parse_video_track_information(Streamer& streamer)
{
    TrackEntry::VideoTrack video_track {};

    TRY(parse_master_element(streamer, "VideoTrack"sv, [&](u64 element_id) -> DecoderErrorOr<IterationDecision> {
        switch (element_id) {
        case PIXEL_WIDTH_ID:
            video_track.pixel_width = TRY_READ(streamer.read_u64());
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read VideoTrack's PixelWidth attribute: {}", video_track.pixel_width);
            break;
        case PIXEL_HEIGHT_ID:
            video_track.pixel_height = TRY_READ(streamer.read_u64());
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read VideoTrack's PixelHeight attribute: {}", video_track.pixel_height);
            break;
        case COLOR_ENTRY_ID:
            video_track.color_format = TRY(parse_video_color_information(streamer));
            break;
        default:
            TRY_READ(streamer.read_unknown_element());
        }

        return IterationDecision::Continue;
    }));

    return video_track;
}

static DecoderErrorOr<TrackEntry::AudioTrack> parse_audio_track_information(Streamer& streamer)
{
    TrackEntry::AudioTrack audio_track {};

    TRY(parse_master_element(streamer, "AudioTrack"sv, [&](u64 element_id) -> DecoderErrorOr<IterationDecision> {
        switch (element_id) {
        case CHANNELS_ID:
            audio_track.channels = TRY_READ(streamer.read_u64());
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read AudioTrack's Channels attribute: {}", audio_track.channels);
            break;
        case BIT_DEPTH_ID:
            audio_track.bit_depth = TRY_READ(streamer.read_u64());
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read AudioTrack's BitDepth attribute: {}", audio_track.bit_depth);
            break;
        default:
            TRY_READ(streamer.read_unknown_element());
        }

        return IterationDecision::Continue;
    }));

    return audio_track;
}

static DecoderErrorOr<NonnullRefPtr<TrackEntry>> parse_track_entry(Streamer& streamer)
{
    auto track_entry = DECODER_TRY_ALLOC(try_make_ref_counted<TrackEntry>());
    TRY(parse_master_element(streamer, "Track"sv, [&](u64 element_id) -> DecoderErrorOr<IterationDecision> {
        switch (element_id) {
        case TRACK_NUMBER_ID:
            track_entry->set_track_number(TRY_READ(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read TrackNumber attribute: {}", track_entry->track_number());
            break;
        case TRACK_UID_ID:
            track_entry->set_track_uid(TRY_READ(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read TrackUID attribute: {}", track_entry->track_uid());
            break;
        case TRACK_TYPE_ID:
            track_entry->set_track_type(static_cast<TrackEntry::TrackType>(TRY_READ(streamer.read_u64())));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read TrackType attribute: {}", to_underlying(track_entry->track_type()));
            break;
        case TRACK_LANGUAGE_ID:
            track_entry->set_language(DECODER_TRY_ALLOC(String::from_byte_string(TRY_READ(streamer.read_string()))));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's Language attribute: {}", track_entry->language());
            break;
        case TRACK_CODEC_ID:
            track_entry->set_codec_id(DECODER_TRY_ALLOC(String::from_byte_string(TRY_READ(streamer.read_string()))));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's CodecID attribute: {}", track_entry->codec_id());
            break;
        case TRACK_CODEC_PRIVATE: {
            auto codec_private_data = TRY_READ(streamer.read_raw_octets(TRY_READ(streamer.read_variable_size_integer())));
            DECODER_TRY_ALLOC(track_entry->set_codec_private_data(codec_private_data));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's CodecID attribute: {}", track_entry->codec_id());
            break;
        }
        case TRACK_TIMESTAMP_SCALE_ID:
            track_entry->set_timestamp_scale(TRY_READ(streamer.read_float()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's TrackTimestampScale attribute: {}", track_entry->timestamp_scale());
            break;
        case TRACK_OFFSET_ID:
            track_entry->set_timestamp_offset(TRY_READ(streamer.read_variable_size_signed_integer()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's TrackOffset attribute: {}", track_entry->timestamp_offset());
            break;
        case TRACK_VIDEO_ID:
            track_entry->set_video_track(TRY(parse_video_track_information(streamer)));
            break;
        case TRACK_AUDIO_ID:
            track_entry->set_audio_track(TRY(parse_audio_track_information(streamer)));
            break;
        default:
            TRY_READ(streamer.read_unknown_element());
        }

        return IterationDecision::Continue;
    }));

    return track_entry;
}

DecoderErrorOr<void> Reader::parse_tracks(Streamer& streamer)
{
    TRY(parse_master_element(streamer, "Tracks"sv, [&](u64 element_id) -> DecoderErrorOr<IterationDecision> {
        if (element_id == TRACK_ENTRY_ID) {
            auto track_entry = TRY(parse_track_entry(streamer));
            dbgln_if(MATROSKA_DEBUG, "Parsed track {}", track_entry->track_number());
            DECODER_TRY_ALLOC(m_tracks.try_set(track_entry->track_number(), track_entry));
        } else {
            TRY_READ(streamer.read_unknown_element());
        }

        return IterationDecision::Continue;
    }));
    return {};
}

DecoderErrorOr<void> Reader::for_each_track(TrackEntryCallback callback)
{
    TRY(ensure_tracks_are_parsed());
    for (auto const& track_entry : m_tracks) {
        auto decision = TRY(callback(track_entry.value));
        if (decision == IterationDecision::Break)
            break;
    }
    return {};
}

DecoderErrorOr<void> Reader::for_each_track_of_type(TrackEntry::TrackType type, TrackEntryCallback callback)
{
    return for_each_track([&](TrackEntry const& track_entry) -> DecoderErrorOr<IterationDecision> {
        if (track_entry.track_type() != type)
            return IterationDecision::Continue;
        return callback(track_entry);
    });
}

DecoderErrorOr<NonnullRefPtr<TrackEntry>> Reader::track_for_track_number(u64 track_number)
{
    TRY(ensure_tracks_are_parsed());
    auto optional_track_entry = m_tracks.get(track_number);
    if (!optional_track_entry.has_value())
        return DecoderError::format(DecoderErrorCategory::Invalid, "No track found with number {}", track_number);
    return *optional_track_entry.release_value();
}

DecoderErrorOr<size_t> Reader::track_count()
{
    TRY(ensure_tracks_are_parsed());
    return m_tracks.size();
}

constexpr size_t get_element_id_size(u32 element_id)
{
    return sizeof(element_id) - (count_leading_zeroes(element_id) / 8);
}

static DecoderErrorOr<Cluster> parse_cluster(Streamer& streamer, u64 timestamp_scale)
{
    Optional<u64> timestamp;

    auto first_element_position = TRY(parse_master_element(streamer, "Cluster"sv, [&](u64 element_id) -> DecoderErrorOr<IterationDecision> {
        switch (element_id) {
        case TIMESTAMP_ID:
            timestamp = TRY_READ(streamer.read_u64());
            return IterationDecision::Break;
        default:
            TRY_READ(streamer.read_unknown_element());
        }

        return IterationDecision::Continue;
    }));

    if (!timestamp.has_value())
        return DecoderError::corrupted("Cluster was missing a timestamp"sv);
    if (first_element_position == 0)
        return DecoderError::corrupted("Cluster had no children"sv);

    dbgln_if(MATROSKA_TRACE_DEBUG, "Seeking back to position {}", first_element_position);
    TRY_READ(streamer.seek_to_position(first_element_position));

    Cluster cluster;
    cluster.set_timestamp(Duration::from_nanoseconds(timestamp.release_value() * timestamp_scale));
    return cluster;
}

static DecoderErrorOr<Block> parse_simple_block(Streamer& streamer, Duration cluster_timestamp, u64 segment_timestamp_scale, TrackEntry const& track)
{
    Block block;

    auto content_size = TRY_READ(streamer.read_variable_size_integer());

    auto position_before_track_number = streamer.position();
    block.set_track_number(TRY_READ(streamer.read_variable_size_integer()));

    // https://www.matroska.org/technical/notes.html
    // Block Timestamps:
    //     The Block Element and SimpleBlock Element store their timestamps as signed integers,
    //     relative to the Cluster\Timestamp value of the Cluster they are stored in. To get the
    //     timestamp of a Block or SimpleBlock in nanoseconds you have to use the following formula:
    //         `( Cluster\Timestamp + ( block timestamp * TrackTimestampScale ) ) * TimestampScale`
    //
    //     When a CodecDelay Element is set, its value MUST be subtracted from each Block timestamp
    //     of that track. To get the timestamp in nanoseconds of the first frame in a Block or
    //     SimpleBlock, the formula becomes:
    //         `( ( Cluster\Timestamp + ( block timestamp * TrackTimestampScale ) ) * TimestampScale ) - CodecDelay`
    auto raw_timestamp_offset = TRY_READ(streamer.read_i16());
    Checked<i64> timestamp_offset_ns = AK::clamp_to<i64>(static_cast<double>(raw_timestamp_offset * AK::clamp_to<i64>(segment_timestamp_scale)) * track.timestamp_scale());
    timestamp_offset_ns.saturating_sub(AK::clamp_to<i64>(track.codec_delay()));
    // This is only mentioned in the elements specification under TrackOffset.
    // https://www.matroska.org/technical/elements.html
    timestamp_offset_ns.saturating_add(AK::clamp_to<i64>(track.timestamp_offset()));
    Duration timestamp_offset = Duration::from_nanoseconds(timestamp_offset_ns.value());
    block.set_timestamp(cluster_timestamp + timestamp_offset);

    auto flags = TRY_READ(streamer.read_octet());
    block.set_only_keyframes((flags & (1u << 7u)) != 0);
    block.set_invisible((flags & (1u << 3u)) != 0);
    block.set_lacing(static_cast<Block::Lacing>((flags & 0b110u) >> 1u));
    block.set_discardable((flags & 1u) != 0);

    auto total_frame_content_size = content_size - (streamer.position() - position_before_track_number);

    Vector<ReadonlyBytes> frames;

    if (block.lacing() == Block::Lacing::EBML) {
        auto octets_read_before_frame_sizes = streamer.octets_read();
        auto frame_count = TRY_READ(streamer.read_octet()) + 1;
        Vector<u64> frame_sizes;
        frame_sizes.ensure_capacity(frame_count);

        u64 frame_size_sum = 0;
        u64 previous_frame_size;
        auto first_frame_size = TRY_READ(streamer.read_variable_size_integer());
        frame_sizes.append(first_frame_size);
        frame_size_sum += first_frame_size;
        previous_frame_size = first_frame_size;

        for (int i = 0; i < frame_count - 2; i++) {
            auto frame_size_difference = TRY_READ(streamer.read_variable_size_signed_integer());
            u64 frame_size;
            // FIXME: x - (-y) == x + y?
            if (frame_size_difference < 0)
                frame_size = previous_frame_size - (-frame_size_difference);
            else
                frame_size = previous_frame_size + frame_size_difference;
            frame_sizes.append(frame_size);
            frame_size_sum += frame_size;
            previous_frame_size = frame_size;
        }
        frame_sizes.append(total_frame_content_size - frame_size_sum - (streamer.octets_read() - octets_read_before_frame_sizes));

        for (int i = 0; i < frame_count; i++) {
            // FIXME: ReadonlyBytes instead of copying the frame data?
            auto current_frame_size = frame_sizes.at(i);
            frames.append(TRY_READ(streamer.read_raw_octets(current_frame_size)));
        }
    } else if (block.lacing() == Block::Lacing::FixedSize) {
        auto frame_count = TRY_READ(streamer.read_octet()) + 1;
        auto individual_frame_size = total_frame_content_size / frame_count;
        for (int i = 0; i < frame_count; i++)
            frames.append(TRY_READ(streamer.read_raw_octets(individual_frame_size)));
    } else {
        frames.append(TRY_READ(streamer.read_raw_octets(total_frame_content_size)));
    }
    block.set_frames(move(frames));
    return block;
}

DecoderErrorOr<SampleIterator> Reader::create_sample_iterator(u64 track_number)
{
    auto optional_position = TRY(find_first_top_level_element_with_id("Cluster"sv, CLUSTER_ELEMENT_ID));
    if (!optional_position.has_value())
        return DecoderError::corrupted("No clusters are present in the segment"sv);
    ReadonlyBytes segment_view = m_data.slice(m_segment_contents_position, m_segment_contents_size);

    // We need to have the element ID included so that the iterator knows where it is.
    auto position = optional_position.value() - get_element_id_size(CLUSTER_ELEMENT_ID) - m_segment_contents_position;

    dbgln_if(MATROSKA_DEBUG, "Creating sample iterator starting at {} relative to segment at {}", position, m_segment_contents_position);
    return SampleIterator(this->m_mapped_file, segment_view, TRY(track_for_track_number(track_number)), TRY(segment_information()).timestamp_scale(), position);
}

static DecoderErrorOr<CueTrackPosition> parse_cue_track_position(Streamer& streamer)
{
    CueTrackPosition track_position;

    bool had_cluster_position = false;

    TRY_READ(parse_master_element(streamer, "CueTrackPositions"sv, [&](u64 element_id) -> DecoderErrorOr<IterationDecision> {
        switch (element_id) {
        case CUE_TRACK_ID:
            track_position.set_track_number(TRY_READ(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read CueTrackPositions track number {}", track_position.track_number());
            break;
        case CUE_CLUSTER_POSITION_ID:
            track_position.set_cluster_position(TRY_READ(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read CueTrackPositions cluster position {}", track_position.cluster_position());
            had_cluster_position = true;
            break;
        case CUE_RELATIVE_POSITION_ID:
            track_position.set_block_offset(TRY_READ(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read CueTrackPositions relative position {}", track_position.block_offset());
            break;
        case CUE_CODEC_STATE_ID:
            // Mandatory in spec, but not present in files? 0 means use TrackEntry's codec state.
            // FIXME: Do something with this value.
            dbgln_if(MATROSKA_DEBUG, "Found CodecState, skipping");
            TRY_READ(streamer.read_unknown_element());
            break;
        case CUE_REFERENCE_ID:
            return DecoderError::not_implemented();
        default:
            TRY_READ(streamer.read_unknown_element());
            break;
        }

        return IterationDecision::Continue;
    }));

    if (track_position.track_number() == 0)
        return DecoderError::corrupted("Track number was not present or 0"sv);

    if (!had_cluster_position)
        return DecoderError::corrupted("Cluster was missing the cluster position"sv);

    return track_position;
}

static DecoderErrorOr<CuePoint> parse_cue_point(Streamer& streamer, u64 timestamp_scale)
{
    CuePoint cue_point;

    TRY(parse_master_element(streamer, "CuePoint"sv, [&](u64 element_id) -> DecoderErrorOr<IterationDecision> {
        switch (element_id) {
        case CUE_TIME_ID: {
            // On https://www.matroska.org/technical/elements.html, spec says of the CueTime element:
            // > Absolute timestamp of the seek point, expressed in Matroska Ticks -- ie in nanoseconds; see timestamp-ticks.
            // Matroska Ticks are specified in https://www.matroska.org/technical/notes.html:
            // > For such elements, the timestamp value is stored directly in nanoseconds.
            // However, my test files appear to use Segment Ticks, which uses the segment's timestamp scale, and Mozilla's nestegg parser agrees:
            // https://github.com/mozilla/nestegg/tree/ec6adfbbf979678e3058cc4695257366f39e290b/src/nestegg.c#L1941
            // https://github.com/mozilla/nestegg/tree/ec6adfbbf979678e3058cc4695257366f39e290b/src/nestegg.c#L2411-L2416
            // https://github.com/mozilla/nestegg/tree/ec6adfbbf979678e3058cc4695257366f39e290b/src/nestegg.c#L1383-L1392
            // Other fields that specify Matroska Ticks may also use Segment Ticks instead, who knows :^(
            auto timestamp = Duration::from_nanoseconds(static_cast<i64>(TRY_READ(streamer.read_u64()) * timestamp_scale));
            cue_point.set_timestamp(timestamp);
            dbgln_if(MATROSKA_DEBUG, "Read CuePoint timestamp {}ms", cue_point.timestamp().to_milliseconds());
            break;
        }
        case CUE_TRACK_POSITIONS_ID: {
            auto track_position = TRY_READ(parse_cue_track_position(streamer));
            DECODER_TRY_ALLOC(cue_point.track_positions().try_set(track_position.track_number(), track_position));
            break;
        }
        default:
            TRY_READ(streamer.read_unknown_element());
            break;
        }

        return IterationDecision::Continue;
    }));

    if (cue_point.timestamp().is_negative())
        return DecoderError::corrupted("CuePoint was missing a timestamp"sv);

    if (cue_point.track_positions().is_empty())
        return DecoderError::corrupted("CuePoint was missing track positions"sv);

    return cue_point;
}

DecoderErrorOr<void> Reader::parse_cues(Streamer& streamer)
{
    m_cues.clear();

    TRY(parse_master_element(streamer, "Cues"sv, [&](u64 element_id) -> DecoderErrorOr<IterationDecision> {
        switch (element_id) {
        case CUE_POINT_ID: {
            auto cue_point = TRY(parse_cue_point(streamer, TRY(segment_information()).timestamp_scale()));

            // FIXME: Verify that these are already in order of timestamp. If they are not, return a corrupted error for now,
            //        but if it turns out that Matroska files with out-of-order cue points are valid, sort them instead.

            for (auto track_position_entry : cue_point.track_positions()) {
                if (!m_cues.contains(track_position_entry.key))
                    DECODER_TRY_ALLOC(m_cues.try_set(track_position_entry.key, Vector<CuePoint>()));
                Vector<CuePoint>& cue_points_for_track = m_cues.get(track_position_entry.key).release_value();
                cue_points_for_track.append(cue_point);
            }
            break;
        }
        default:
            return DecoderError::format(DecoderErrorCategory::Corrupted, "Unknown Cues child ID {:#010x}", element_id);
        }

        return IterationDecision::Continue;
    }));

    return {};
}

DecoderErrorOr<void> Reader::ensure_cues_are_parsed()
{
    if (m_cues_have_been_parsed)
        return {};
    auto position = TRY(find_first_top_level_element_with_id("Cues"sv, CUES_ID));
    if (!position.has_value())
        return DecoderError::corrupted("No Tracks element found"sv);
    Streamer streamer { m_data };
    TRY_READ(streamer.seek_to_position(position.release_value()));
    TRY(parse_cues(streamer));
    m_cues_have_been_parsed = true;
    return {};
}

DecoderErrorOr<void> Reader::seek_to_cue_for_timestamp(SampleIterator& iterator, Duration const& timestamp)
{
    auto const& cue_points = MUST(cue_points_for_track(iterator.m_track->track_number())).release_value();

    // Take a guess at where in the cues the timestamp will be and correct from there.
    auto duration = TRY(segment_information()).duration();
    size_t index = 0;
    if (duration.has_value())
        index = clamp(((timestamp.to_nanoseconds() * cue_points.size()) / TRY(segment_information()).duration()->to_nanoseconds()), 0, cue_points.size() - 1);

    CuePoint const* prev_cue_point = &cue_points[index];
    dbgln_if(MATROSKA_DEBUG, "Finding Matroska cue points for timestamp {}ms starting from cue at {}ms", timestamp.to_milliseconds(), prev_cue_point->timestamp().to_milliseconds());

    if (prev_cue_point->timestamp() == timestamp) {
        TRY(iterator.seek_to_cue_point(*prev_cue_point));
        return {};
    }

    if (prev_cue_point->timestamp() > timestamp) {
        while (index > 0 && prev_cue_point->timestamp() > timestamp) {
            prev_cue_point = &cue_points[--index];
            dbgln_if(MATROSKA_DEBUG, "Checking previous cue point {}ms", prev_cue_point->timestamp().to_milliseconds());
        }
        TRY(iterator.seek_to_cue_point(*prev_cue_point));
        return {};
    }

    while (++index < cue_points.size()) {
        auto const& cue_point = cue_points[index];
        dbgln_if(MATROSKA_DEBUG, "Checking future cue point {}ms", cue_point.timestamp().to_milliseconds());
        if (cue_point.timestamp() > timestamp)
            break;
        prev_cue_point = &cue_point;
    }

    TRY(iterator.seek_to_cue_point(*prev_cue_point));
    return {};
}

static DecoderErrorOr<void> search_clusters_for_keyframe_before_timestamp(SampleIterator& iterator, Duration const& timestamp)
{
#if MATROSKA_DEBUG
    size_t inter_frames_count;
#endif
    Optional<SampleIterator> last_keyframe;

    while (true) {
        SampleIterator rewind_iterator = iterator;
        auto block = TRY(iterator.next_block());

        if (block.only_keyframes()) {
            last_keyframe.emplace(rewind_iterator);
#if MATROSKA_DEBUG
            inter_frames_count = 0;
#endif
        }

        if (block.timestamp() > timestamp)
            break;

#if MATROSKA_DEBUG
        inter_frames_count++;
#endif
    }

    if (last_keyframe.has_value()) {
#if MATROSKA_DEBUG
        dbgln("Seeked to a keyframe with {} inter frames to skip", inter_frames_count);
#endif
        iterator = last_keyframe.release_value();
    }

    return {};
}

DecoderErrorOr<bool> Reader::has_cues_for_track(u64 track_number)
{
    TRY(ensure_cues_are_parsed());
    return m_cues.contains(track_number);
}

DecoderErrorOr<SampleIterator> Reader::seek_to_random_access_point(SampleIterator iterator, Duration timestamp)
{
    if (TRY(has_cues_for_track(iterator.m_track->track_number()))) {
        TRY(seek_to_cue_for_timestamp(iterator, timestamp));
        VERIFY(iterator.last_timestamp().has_value());
        return iterator;
    }

    if (!iterator.last_timestamp().has_value() || timestamp < iterator.last_timestamp().value()) {
        // If the timestamp is before the iterator's current position, then we need to start from the beginning of the Segment.
        iterator = TRY(create_sample_iterator(iterator.m_track->track_number()));
        TRY(search_clusters_for_keyframe_before_timestamp(iterator, timestamp));
        return iterator;
    }

    TRY(search_clusters_for_keyframe_before_timestamp(iterator, timestamp));
    return iterator;
}

DecoderErrorOr<Optional<Vector<CuePoint> const&>> Reader::cue_points_for_track(u64 track_number)
{
    TRY(ensure_cues_are_parsed());
    return m_cues.get(track_number);
}

DecoderErrorOr<Block> SampleIterator::next_block()
{
    if (m_position >= m_data.size())
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "Still at end of stream :^)"sv);

    Streamer streamer { m_data };
    TRY_READ(streamer.seek_to_position(m_position));

    Optional<Block> block;

    while (streamer.has_octet()) {
#if MATROSKA_TRACE_DEBUG
        auto element_position = streamer.position();
#endif
        auto element_id = TRY_READ(streamer.read_variable_size_integer(false));
#if MATROSKA_TRACE_DEBUG
        dbgln("Iterator found element with ID {:#010x} at offset {} within the segment.", element_id, element_position);
#endif

        if (element_id == CLUSTER_ELEMENT_ID) {
            dbgln_if(MATROSKA_DEBUG, "  Iterator is parsing new cluster.");
            m_current_cluster = TRY(parse_cluster(streamer, m_segment_timestamp_scale));
        } else if (element_id == SIMPLE_BLOCK_ID) {
            dbgln_if(MATROSKA_TRACE_DEBUG, "  Iterator is parsing new block.");
            auto candidate_block = TRY(parse_simple_block(streamer, m_current_cluster->timestamp(), m_segment_timestamp_scale, m_track));
            if (candidate_block.track_number() == m_track->track_number())
                block = move(candidate_block);
        } else {
            dbgln_if(MATROSKA_TRACE_DEBUG, "  Iterator is skipping unknown element with ID {:#010x}.", element_id);
            TRY_READ(streamer.read_unknown_element());
        }

        m_position = streamer.position();
        if (block.has_value()) {
            m_last_timestamp = block->timestamp();
            return block.release_value();
        }
    }

    m_current_cluster.clear();
    return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "End of stream"sv);
}

DecoderErrorOr<void> SampleIterator::seek_to_cue_point(CuePoint const& cue_point)
{
    // This is a private function. The position getter can return optional, but the caller should already know that this track has a position.
    auto const& cue_position = cue_point.position_for_track(m_track->track_number()).release_value();
    Streamer streamer { m_data };
    TRY_READ(streamer.seek_to_position(cue_position.cluster_position()));

    auto element_id = TRY_READ(streamer.read_variable_size_integer(false));
    if (element_id != CLUSTER_ELEMENT_ID)
        return DecoderError::corrupted("Cue point's cluster position didn't point to a cluster"sv);

    m_current_cluster = TRY(parse_cluster(streamer, m_segment_timestamp_scale));
    dbgln_if(MATROSKA_DEBUG, "SampleIterator set to cue point at timestamp {}ms", m_current_cluster->timestamp().to_milliseconds());

    m_position = streamer.position() + cue_position.block_offset();
    m_last_timestamp = cue_point.timestamp();
    return {};
}

ErrorOr<ByteString> Streamer::read_string()
{
    auto string_length = TRY(read_variable_size_integer());
    if (remaining() < string_length)
        return Error::from_string_literal("String length extends past the end of the stream");
    auto string_data = data_as_chars();
    auto string_value = ByteString(string_data, strnlen(string_data, string_length));
    TRY(read_raw_octets(string_length));
    return string_value;
}

ErrorOr<u8> Streamer::read_octet()
{
    if (!has_octet()) {
        dbgln_if(MATROSKA_TRACE_DEBUG, "Ran out of stream data");
        return Error::from_string_literal("Stream is out of data");
    }
    u8 byte = *data();
    m_octets_read.last()++;
    m_position++;
    return byte;
}

ErrorOr<i16> Streamer::read_i16()
{
    return (TRY(read_octet()) << 8) | TRY(read_octet());
}

ErrorOr<u64> Streamer::read_variable_size_integer(bool mask_length)
{
    dbgln_if(MATROSKA_TRACE_DEBUG, "Reading VINT from offset {:p}", position());
    auto length_descriptor = TRY(read_octet());
    dbgln_if(MATROSKA_TRACE_DEBUG, "Reading VINT, first byte is {:#02x}", length_descriptor);
    if (length_descriptor == 0)
        return Error::from_string_literal("read_variable_size_integer: Length descriptor has no terminating set bit");
    size_t length = 0;
    while (length < 8) {
        if (((length_descriptor >> (8 - length)) & 1) == 1)
            break;
        length++;
    }
    dbgln_if(MATROSKA_TRACE_DEBUG, "Reading VINT of total length {}", length);
    if (length > 8)
        return Error::from_string_literal("read_variable_size_integer: Length is too large");

    u64 result;
    if (mask_length)
        result = length_descriptor & ~(1u << (8 - length));
    else
        result = length_descriptor;
    dbgln_if(MATROSKA_TRACE_DEBUG, "Beginning of VINT is {:#02x}", result);
    for (size_t i = 1; i < length; i++) {
        u8 next_octet = TRY(read_octet());
        dbgln_if(MATROSKA_TRACE_DEBUG, "Read octet of {:#02x}", next_octet);
        result = (result << 8u) | next_octet;
        dbgln_if(MATROSKA_TRACE_DEBUG, "New result is {:#010x}", result);
    }
    return result;
}

ErrorOr<i64> Streamer::read_variable_size_signed_integer()
{
    auto length_descriptor = TRY(read_octet());
    if (length_descriptor == 0)
        return Error::from_string_literal("read_variable_sized_signed_integer: Length descriptor has no terminating set bit");
    i64 length = 0;
    while (length < 8) {
        if (((length_descriptor >> (8 - length)) & 1) == 1)
            break;
        length++;
    }
    if (length > 8)
        return Error::from_string_literal("read_variable_size_integer: Length is too large");

    i64 result = length_descriptor & ~(1u << (8 - length));
    for (i64 i = 1; i < length; i++) {
        u8 next_octet = TRY(read_octet());
        result = (result << 8u) | next_octet;
    }
    result -= AK::exp2<i64>(length * 7 - 1) - 1;
    return result;
}

ErrorOr<ReadonlyBytes> Streamer::read_raw_octets(size_t num_octets)
{
    if (remaining() < num_octets)
        return Error::from_string_literal("Tried to drop octets past the end of the stream");
    ReadonlyBytes result = { data(), num_octets };
    m_position += num_octets;
    m_octets_read.last() += num_octets;
    return result;
}

ErrorOr<u64> Streamer::read_u64()
{
    auto integer_length = TRY(read_variable_size_integer());
    u64 result = 0;
    for (size_t i = 0; i < integer_length; i++) {
        result = (result << 8u) + TRY(read_octet());
    }
    return result;
}

ErrorOr<double> Streamer::read_float()
{
    auto length = TRY(read_variable_size_integer());
    if (length != 4u && length != 8u)
        return Error::from_string_literal("Float size must be 4 or 8 bytes");

    union {
        u64 value;
        float float_value;
        double double_value;
    } read_data;
    read_data.value = 0;
    for (size_t i = 0; i < length; i++) {
        read_data.value = (read_data.value << 8u) + TRY(read_octet());
    }
    if (length == 4u)
        return read_data.float_value;
    return read_data.double_value;
}

ErrorOr<void> Streamer::read_unknown_element()
{
    auto element_length = TRY(read_variable_size_integer());
    dbgln_if(MATROSKA_TRACE_DEBUG, "Skipping unknown element of size {}.", element_length);
    TRY(read_raw_octets(element_length));
    return {};
}

ErrorOr<void> Streamer::seek_to_position(size_t position)
{
    if (position >= m_data.size())
        return Error::from_string_literal("Attempted to seek past the end of the stream");
    m_position = position;
    return {};
}

}
