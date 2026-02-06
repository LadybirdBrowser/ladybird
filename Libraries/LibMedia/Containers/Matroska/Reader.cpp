/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Function.h>
#include <AK/IntegralMath.h>
#include <AK/Math.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Utf8View.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/Containers/Matroska/Utilities.h>
#include <LibMedia/MediaStream.h>

#include "Reader.h"

namespace Media::Matroska {

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
constexpr u32 TRACK_NAME_ID = 0x536E;
constexpr u32 TRACK_LANGUAGE_ID = 0x22B59C;
constexpr u32 TRACK_LANGUAGE_BCP_47_ID = 0x22B59D;
constexpr u32 TRACK_CODEC_ID = 0x86;
constexpr u32 TRACK_CODEC_PRIVATE_ID = 0x63A2;
constexpr u32 TRACK_CODEC_DELAY_ID = 0x56AA;
constexpr u32 TRACK_SEEK_PRE_ROLL_ID = 0x56BB;
constexpr u32 TRACK_TIMESTAMP_SCALE_ID = 0x23314F;
constexpr u32 TRACK_OFFSET_ID = 0x537F;
constexpr u32 TRACK_DEFAULT_DURATION_ID = 0x23E383;
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
constexpr u32 SAMPLING_FREQUENCY_ID = 0xB5;
constexpr u32 BIT_DEPTH_ID = 0x6264;

// Clusters
constexpr u32 SIMPLE_BLOCK_ID = 0xA3;
constexpr u32 TIMESTAMP_ID = 0xE7;
constexpr u32 BLOCK_GROUP_ID = 0xA0;
constexpr u32 BLOCK_ID = 0xA1;
constexpr u32 BLOCK_DURATION_ID = 0x9B;

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

DecoderErrorOr<Reader> Reader::from_stream(NonnullRefPtr<MediaStreamCursor> const& stream_cursor)
{
    Reader reader;
    Streamer streamer { stream_cursor };
    TRY(reader.parse_initial_data(streamer));
    return reader;
}

enum class ElementIterationDecision : u8 {
    Continue,
    BreakHere,
    BreakAtEnd,
};

// Returns the position of the first element that is read from this master element.
static DecoderErrorOr<size_t> parse_master_element(Streamer& streamer, [[maybe_unused]] StringView element_name, Function<DecoderErrorOr<ElementIterationDecision>(u64)> element_consumer)
{
    auto element_data_size = TRY(streamer.read_variable_size_integer());
    dbgln_if(MATROSKA_DEBUG, "{} has {} octets of data.", element_name, element_data_size);

    bool first_element = true;
    auto first_element_position = streamer.position();
    auto element_data_end = first_element_position + element_data_size;

    while (streamer.position() < element_data_end) {
        dbgln_if(MATROSKA_TRACE_DEBUG, "====== Reading  element ======");
        auto element_id = TRY(streamer.read_variable_size_integer(false));
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
            TRY(streamer.read_unknown_element());
            continue;
        }
        if (element_id == EBML_VOID_ELEMENT_ID) {
            // Used to void data or to avoid unexpected behaviors when using damaged data.
            // The content is discarded. Also used to reserve space in a subelement for later use.
            TRY(streamer.read_unknown_element());
            continue;
        }

        auto result = element_consumer(element_id);
        if (result.is_error())
            return DecoderError::format(result.error().category(), "{} -> {}", element_name, result.error().description());
        if (result.value() == ElementIterationDecision::BreakHere)
            break;
        if (result.value() == ElementIterationDecision::BreakAtEnd) {
            TRY(streamer.seek_to_position(element_data_end));
            break;
        }

        dbgln_if(MATROSKA_TRACE_DEBUG, "Read {} octets of the {} so far.", streamer.position() - first_element_position, element_name);
        first_element = false;
    }

    return first_element_position;
}

static DecoderErrorOr<EBMLHeader> parse_ebml_header(Streamer& streamer, ElementIterationDecision complete_decision)
{
    EBMLHeader header;
    TRY(parse_master_element(streamer, "Header"sv, [&](u64 element_id) -> DecoderErrorOr<ElementIterationDecision> {
        switch (element_id) {
        case DOCTYPE_ELEMENT_ID:
            header.doc_type = TRY(streamer.read_string());
            dbgln_if(MATROSKA_DEBUG, "Read DocType attribute: {}", header.doc_type);
            break;
        case DOCTYPE_VERSION_ELEMENT_ID:
            header.doc_type_version = TRY(streamer.read_u64());
            if (header.doc_type_version == 0)
                return DecoderError::corrupted("DocTypeVersion was 0"sv);
            dbgln_if(MATROSKA_DEBUG, "Read DocTypeVersion attribute: {}", header.doc_type_version);
            break;
        default:
            TRY(streamer.read_unknown_element());
        }

        if (!header.doc_type.is_empty() && header.doc_type_version != 0)
            return complete_decision;

        return ElementIterationDecision::Continue;
    }));

    return header;
}

bool Reader::is_matroska_or_webm(NonnullRefPtr<MediaStreamCursor> const& stream_cursor)
{
    auto header = [&] -> DecoderErrorOr<EBMLHeader> {
        Streamer streamer { stream_cursor };
        auto first_element_id = TRY(streamer.read_variable_size_integer(false));
        if (first_element_id != EBML_MASTER_ELEMENT_ID)
            return DecoderError::corrupted("First element was not an EBML header"sv);
        return parse_ebml_header(streamer, ElementIterationDecision::BreakHere);
    }();
    if (header.is_error())
        return false;
    auto doc_type = header.release_value().doc_type;
    if (doc_type == "matroska")
        return true;
    if (doc_type == "webm")
        return true;
    return false;
}

DecoderErrorOr<void> Reader::parse_initial_data(Streamer& streamer)
{
    auto first_element_id = TRY(streamer.read_variable_size_integer(false));
    dbgln_if(MATROSKA_TRACE_DEBUG, "First element ID is {:#010x}\n", first_element_id);
    if (first_element_id != EBML_MASTER_ELEMENT_ID)
        return DecoderError::corrupted("First element was not an EBML header"sv);

    m_header = TRY(parse_ebml_header(streamer, ElementIterationDecision::BreakAtEnd));
    dbgln_if(MATROSKA_DEBUG, "Parsed EBML header");

    auto root_element_id = TRY(streamer.read_variable_size_integer(false));
    if (root_element_id != SEGMENT_ELEMENT_ID)
        return DecoderError::corrupted("Second element was not a segment element"sv);

    m_segment_contents_size = TRY(streamer.read_variable_size_integer());
    m_segment_contents_position = streamer.position();
    dbgln_if(MATROSKA_TRACE_DEBUG, "Segment is at {} with size {}", m_segment_contents_position, m_segment_contents_size);

    TRY(parse_segment_information(streamer));
    TRY(parse_tracks(streamer));

    auto first_cluster_position = TRY(find_first_top_level_element_with_id(streamer, "Cluster"sv, CLUSTER_ELEMENT_ID));
    if (!first_cluster_position.has_value())
        return DecoderError::corrupted("No clusters are present in the segment"sv);
    m_first_cluster_position = first_cluster_position.release_value();

    TRY(parse_cues(streamer));

    return {};
}

static DecoderErrorOr<void> parse_seek_head(Streamer& streamer, size_t base_position, HashMap<u32, size_t>& table)
{
    TRY(parse_master_element(streamer, "SeekHead"sv, [&](u64 seek_head_child_id) -> DecoderErrorOr<ElementIterationDecision> {
        if (seek_head_child_id == SEEK_ELEMENT_ID) {
            Optional<u64> seek_id;
            Optional<u64> seek_position;
            TRY(parse_master_element(streamer, "Seek"sv, [&](u64 seek_entry_child_id) -> DecoderErrorOr<ElementIterationDecision> {
                switch (seek_entry_child_id) {
                case SEEK_ID_ELEMENT_ID:
                    seek_id = TRY(streamer.read_u64());
                    dbgln_if(MATROSKA_TRACE_DEBUG, "Read Seek Element ID value {:#010x}", seek_id.value());
                    break;
                case SEEK_POSITION_ELEMENT_ID:
                    seek_position = TRY(streamer.read_u64());
                    dbgln_if(MATROSKA_TRACE_DEBUG, "Read Seek Position value {}", seek_position.value());
                    break;
                default:
                    TRY(streamer.read_unknown_element());
                }

                return ElementIterationDecision::Continue;
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
                return ElementIterationDecision::Continue;
            }

            DECODER_TRY_ALLOC(table.try_set(seek_id.release_value(), base_position + seek_position.release_value()));
        } else {
            dbgln_if(MATROSKA_TRACE_DEBUG, "Unknown SeekHead child element ID {:#010x}", seek_head_child_id);
        }

        return ElementIterationDecision::Continue;
    }));
    return {};
}

DecoderErrorOr<Optional<size_t>> Reader::find_first_top_level_element_with_id(Streamer& streamer, StringView element_name, u32 element_id)
{
    dbgln_if(MATROSKA_DEBUG, "====== Finding element {} with ID {:#010x} ======", element_name, element_id);

    if (m_seek_entries.contains(element_id)) {
        dbgln_if(MATROSKA_TRACE_DEBUG, "Cache hit!");
        return m_seek_entries.get(element_id).release_value();
    }

    if (m_last_top_level_element_position != 0)
        TRY(streamer.seek_to_position(m_last_top_level_element_position));
    else
        TRY(streamer.seek_to_position(m_segment_contents_position));

    Optional<size_t> position;

    while (streamer.position() < m_segment_contents_position + m_segment_contents_size) {
        auto found_element_position = streamer.position();
        auto found_element_id = TRY(streamer.read_variable_size_integer(false));
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

        TRY(streamer.read_unknown_element());

        m_last_top_level_element_position = streamer.position();

        DECODER_TRY_ALLOC(m_seek_entries.try_set(found_element_id, found_element_position, AK::HashSetExistingEntryBehavior::Keep));

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
    TRY(parse_master_element(streamer, "Segment Information"sv, [&](u64 element_id) -> DecoderErrorOr<ElementIterationDecision> {
        switch (element_id) {
        case TIMESTAMP_SCALE_ID:
            segment_information.set_timestamp_scale(TRY(streamer.read_u64()));
            dbgln_if(MATROSKA_DEBUG, "Read TimestampScale attribute: {}", segment_information.timestamp_scale());
            break;
        case MUXING_APP_ID:
            segment_information.set_muxing_app(TRY(streamer.read_string()));
            dbgln_if(MATROSKA_DEBUG, "Read MuxingApp attribute: {}", segment_information.muxing_app());
            break;
        case WRITING_APP_ID:
            segment_information.set_writing_app(TRY(streamer.read_string()));
            dbgln_if(MATROSKA_DEBUG, "Read WritingApp attribute: {}", segment_information.writing_app());
            break;
        case DURATION_ID:
            segment_information.set_duration_unscaled(TRY(streamer.read_float()));
            dbgln_if(MATROSKA_DEBUG, "Read Duration attribute: {}", segment_information.duration_unscaled().value());
            break;
        default:
            TRY(streamer.read_unknown_element());
        }

        return ElementIterationDecision::Continue;
    }));

    return segment_information;
}

DecoderErrorOr<void> Reader::parse_segment_information(Streamer& streamer)
{
    auto position = TRY(find_first_top_level_element_with_id(streamer, "Segment Information"sv, SEGMENT_INFORMATION_ELEMENT_ID));
    if (!position.has_value())
        return DecoderError::corrupted("No Segment Information element found"sv);
    TRY(streamer.seek_to_position(position.release_value()));
    if (TRY(streamer.read_variable_size_integer(false)) != SEGMENT_INFORMATION_ELEMENT_ID)
        return DecoderError::corrupted("Unexpected Matroska element when seeking to the Segment element"sv);
    m_segment_information = TRY(parse_information(streamer));
    return {};
}

static DecoderErrorOr<TrackEntry::ColorFormat> parse_video_color_information(Streamer& streamer)
{
    TrackEntry::ColorFormat color_format {};

    TRY(parse_master_element(streamer, "Colour"sv, [&](u64 element_id) -> DecoderErrorOr<ElementIterationDecision> {
        switch (element_id) {
        case PRIMARIES_ID:
            color_format.color_primaries = static_cast<ColorPrimaries>(TRY(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Colour's Primaries attribute: {}", color_primaries_to_string(color_format.color_primaries));
            break;
        case TRANSFER_CHARACTERISTICS_ID:
            color_format.transfer_characteristics = static_cast<TransferCharacteristics>(TRY(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Colour's TransferCharacteristics attribute: {}", transfer_characteristics_to_string(color_format.transfer_characteristics));
            break;
        case MATRIX_COEFFICIENTS_ID:
            color_format.matrix_coefficients = static_cast<MatrixCoefficients>(TRY(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Colour's MatrixCoefficients attribute: {}", matrix_coefficients_to_string(color_format.matrix_coefficients));
            break;
        case RANGE_ID:
            color_format.range = static_cast<TrackEntry::ColorRange>(TRY(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Colour's Range attribute: {}", to_underlying(color_format.range));
            break;
        case BITS_PER_CHANNEL_ID:
            color_format.bits_per_channel = TRY(streamer.read_u64());
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Colour's BitsPerChannel attribute: {}", color_format.bits_per_channel);
            break;
        default:
            TRY(streamer.read_unknown_element());
        }

        return ElementIterationDecision::Continue;
    }));

    return color_format;
}

static DecoderErrorOr<TrackEntry::VideoTrack> parse_video_track_information(Streamer& streamer)
{
    TrackEntry::VideoTrack video_track {};

    TRY(parse_master_element(streamer, "VideoTrack"sv, [&](u64 element_id) -> DecoderErrorOr<ElementIterationDecision> {
        switch (element_id) {
        case PIXEL_WIDTH_ID:
            video_track.pixel_width = TRY(streamer.read_u64());
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read VideoTrack's PixelWidth attribute: {}", video_track.pixel_width);
            break;
        case PIXEL_HEIGHT_ID:
            video_track.pixel_height = TRY(streamer.read_u64());
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read VideoTrack's PixelHeight attribute: {}", video_track.pixel_height);
            break;
        case COLOR_ENTRY_ID:
            video_track.color_format = TRY(parse_video_color_information(streamer));
            break;
        default:
            TRY(streamer.read_unknown_element());
        }

        return ElementIterationDecision::Continue;
    }));

    return video_track;
}

static DecoderErrorOr<TrackEntry::AudioTrack> parse_audio_track_information(Streamer& streamer)
{
    TrackEntry::AudioTrack audio_track {};

    TRY(parse_master_element(streamer, "AudioTrack"sv, [&](u64 element_id) -> DecoderErrorOr<ElementIterationDecision> {
        switch (element_id) {
        case CHANNELS_ID:
            audio_track.channels = TRY(streamer.read_u64());
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read AudioTrack's Channels attribute: {}", audio_track.channels);
            break;
        case SAMPLING_FREQUENCY_ID:
            audio_track.sampling_frequency = TRY(streamer.read_float());
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read AudioTrack's SamplingFrequency attribute: {}", audio_track.channels);
            break;
        case BIT_DEPTH_ID:
            audio_track.bit_depth = TRY(streamer.read_u64());
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read AudioTrack's BitDepth attribute: {}", audio_track.bit_depth);
            break;
        default:
            TRY(streamer.read_unknown_element());
        }

        return ElementIterationDecision::Continue;
    }));

    return audio_track;
}

static DecoderErrorOr<NonnullRefPtr<TrackEntry>> parse_track_entry(Streamer& streamer)
{
    auto track_entry = DECODER_TRY_ALLOC(try_make_ref_counted<TrackEntry>());
    TRY(parse_master_element(streamer, "Track"sv, [&](u64 element_id) -> DecoderErrorOr<ElementIterationDecision> {
        switch (element_id) {
        case TRACK_NUMBER_ID:
            track_entry->set_track_number(TRY(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read TrackNumber attribute: {}", track_entry->track_number());
            break;
        case TRACK_UID_ID:
            track_entry->set_track_uid(TRY(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read TrackUID attribute: {}", track_entry->track_uid());
            break;
        case TRACK_TYPE_ID:
            track_entry->set_track_type(static_cast<TrackEntry::TrackType>(TRY(streamer.read_u64())));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read TrackType attribute: {}", to_underlying(track_entry->track_type()));
            break;
        case TRACK_NAME_ID:
            track_entry->set_name(TRY(streamer.read_string()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's Name attribute: {}", track_entry->name());
            break;
        case TRACK_LANGUAGE_ID:
            track_entry->set_language(TRY(streamer.read_string()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's Language attribute: {}", track_entry->language());
            break;
        case TRACK_LANGUAGE_BCP_47_ID:
            track_entry->set_language_bcp_47(TRY(streamer.read_string()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's LanguageBCP47 attribute: {}", track_entry->language());
            break;
        case TRACK_CODEC_ID:
            track_entry->set_codec_id(TRY(streamer.read_string()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's CodecID attribute: {}", track_entry->codec_id());
            break;
        case TRACK_CODEC_PRIVATE_ID: {
            auto codec_private_data = TRY(streamer.read_raw_octets(TRY(streamer.read_variable_size_integer())));
            DECODER_TRY_ALLOC(track_entry->set_codec_private_data(codec_private_data));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's CodecPrivateData element");
            break;
        }
        case TRACK_CODEC_DELAY_ID:
            track_entry->set_codec_delay(TRY(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's CodecDelay attribute: {}", track_entry->codec_delay());
            break;
        case TRACK_SEEK_PRE_ROLL_ID:
            track_entry->set_seek_pre_roll(TRY(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's SeekPreRoll attribute: {}", track_entry->seek_pre_roll());
            break;
        case TRACK_TIMESTAMP_SCALE_ID:
            track_entry->set_timestamp_scale(TRY(streamer.read_float()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's TrackTimestampScale attribute: {}", track_entry->timestamp_scale());
            break;
        case TRACK_OFFSET_ID:
            track_entry->set_timestamp_offset(TRY(streamer.read_variable_size_signed_integer()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's TrackOffset attribute: {}", track_entry->timestamp_offset());
            break;
        case TRACK_DEFAULT_DURATION_ID:
            track_entry->set_default_duration(TRY(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read Track's DefaultDuration attribute: {}", track_entry->default_duration());
            break;
        case TRACK_VIDEO_ID:
            track_entry->set_video_track(TRY(parse_video_track_information(streamer)));
            break;
        case TRACK_AUDIO_ID:
            track_entry->set_audio_track(TRY(parse_audio_track_information(streamer)));
            break;
        default:
            TRY(streamer.read_unknown_element());
        }

        return ElementIterationDecision::Continue;
    }));

    if (track_entry->track_type() == TrackEntry::TrackType::Complex) {
        // A mix of different other TrackType. The codec needs to define how the Matroska Player
        // should interpret such data.
        auto codec_track_type = track_type_from_codec_id(codec_id_from_matroska_id_string(track_entry->codec_id()));
        switch (codec_track_type) {
        case TrackType::Video:
            track_entry->set_track_type(TrackEntry::TrackType::Video);
            break;
        case TrackType::Audio:
            track_entry->set_track_type(TrackEntry::TrackType::Audio);
            break;
        case TrackType::Subtitles:
            track_entry->set_track_type(TrackEntry::TrackType::Subtitle);
            break;
        case TrackType::Unknown:
            break;
        }
    }
    return track_entry;
}

DecoderErrorOr<void> Reader::parse_tracks(Streamer& streamer)
{
    auto position = TRY(find_first_top_level_element_with_id(streamer, "Tracks"sv, TRACK_ELEMENT_ID));
    if (!position.has_value())
        return DecoderError::corrupted("No Tracks element found"sv);
    TRY(streamer.seek_to_position(position.release_value()));

    if (TRY(streamer.read_variable_size_integer(false)) != TRACK_ELEMENT_ID)
        return DecoderError::corrupted("Unexpected Matroska element when seeking to the Tracks element"sv);

    TRY(parse_master_element(streamer, "Tracks"sv, [&](u64 element_id) -> DecoderErrorOr<ElementIterationDecision> {
        if (element_id == TRACK_ENTRY_ID) {
            auto track_entry = TRY(parse_track_entry(streamer));
            dbgln_if(MATROSKA_DEBUG, "Parsed track {}", track_entry->track_number());
            DECODER_TRY_ALLOC(m_tracks.try_set(track_entry->track_number(), track_entry));
        } else {
            TRY(streamer.read_unknown_element());
        }

        return ElementIterationDecision::Continue;
    }));

    fix_track_quirks();

    return {};
}

void Reader::fix_track_quirks()
{
    fix_ffmpeg_webm_quirk();
}

void Reader::fix_ffmpeg_webm_quirk()
{
    // In libavformat versions <= 59.30.100, blocks were not allowed to have negative timestamps. This means that
    // all blocks were shifted forward until any negative timestamps became zero.
    //
    // Additionally, the pre-skip value for Opus tracks was incorrectly scaled based on the audio sample rate when
    // it was written to the CodecDelay element.
    //
    // In order to get the correct timestamps, we must shift all tracks' timestamps back by the maximum of all the
    // tracks' codec-inherent delays, corrected based on the sample rate in the case of Opus.
    auto muxing_app = m_segment_information.muxing_app();
    auto libavformatPrefix = "Lavf"sv;

    if (muxing_app.starts_with(libavformatPrefix)) {
        auto versionString = muxing_app.substring_view(libavformatPrefix.length());
        auto split = versionString.split_view('.');

        if (split.size() < 3)
            return;

        auto is_affected_version = [&] {
            constexpr u32 final_major_version = 59;
            constexpr u32 final_minor_version = 30;
            constexpr u32 final_micro_version = 100;

            auto major_version = split[0].to_number<u32>();
            if (!major_version.has_value() || major_version.value() > final_major_version)
                return false;
            if (major_version.value() < final_major_version)
                return true;

            auto minor_version = split[1].to_number<u32>();
            if (!minor_version.has_value() || minor_version.value() > final_minor_version)
                return false;
            if (minor_version.value() < final_minor_version)
                return true;

            auto micro_version = split[2].to_number<u32>();
            return micro_version.has_value() && micro_version.value() <= final_micro_version;
        }();
        if (!is_affected_version)
            return;

        u64 max_codec_delay = 0;
        for (auto& [id, track] : m_tracks) {
            auto delay = track->codec_delay();

            if (codec_id_from_matroska_id_string(track->codec_id()) == CodecID::Opus && track->audio_track().has_value()) {
                auto sampling_frequency = AK::clamp_to<u64>(track->audio_track()->sampling_frequency);
                if (sampling_frequency == 0)
                    return;
                delay = delay * 48'000 / sampling_frequency;
            }

            max_codec_delay = max(max_codec_delay, delay);
        }

        auto timestamp_scale = m_segment_information.timestamp_scale();
        max_codec_delay = ((max_codec_delay + (timestamp_scale / 2)) / timestamp_scale) * timestamp_scale;

        for (auto& [id, track] : m_tracks) {
            if (track->codec_delay() != 0)
                continue;
            track->set_codec_delay(max_codec_delay);
        }

        auto duration = m_segment_information.duration_unscaled();

        if (duration.has_value()) {
            auto max_codec_delay_in_duration_units = static_cast<double>(max_codec_delay) / static_cast<double>(m_segment_information.timestamp_scale());
            m_segment_information.set_duration_unscaled(duration.value() - max_codec_delay_in_duration_units);
        }
    }
}

DecoderErrorOr<void> Reader::for_each_track(TrackEntryCallback callback)
{
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
    auto optional_track_entry = m_tracks.get(track_number);
    if (!optional_track_entry.has_value())
        return DecoderError::format(DecoderErrorCategory::Invalid, "No track found with number {}", track_number);
    return *optional_track_entry.release_value();
}

DecoderErrorOr<size_t> Reader::track_count()
{
    return m_tracks.size();
}

static DecoderErrorOr<Cluster> parse_cluster(Streamer& streamer, u64 timestamp_scale)
{
    Optional<u64> timestamp;

    auto first_element_position = TRY(parse_master_element(streamer, "Cluster"sv, [&](u64 element_id) -> DecoderErrorOr<ElementIterationDecision> {
        switch (element_id) {
        case TIMESTAMP_ID:
            timestamp = TRY(streamer.read_u64());
            return ElementIterationDecision::BreakHere;
        default:
            TRY(streamer.read_unknown_element());
        }

        return ElementIterationDecision::Continue;
    }));

    if (!timestamp.has_value())
        return DecoderError::corrupted("Cluster was missing a timestamp"sv);
    if (first_element_position == 0)
        return DecoderError::corrupted("Cluster had no children"sv);

    dbgln_if(MATROSKA_TRACE_DEBUG, "Seeking back to position {}", first_element_position);
    TRY(streamer.seek_to_position(first_element_position));

    Cluster cluster;
    cluster.set_timestamp(AK::Duration::from_nanoseconds(AK::clamp_to<i64>(timestamp.release_value() * timestamp_scale)));
    return cluster;
}

static AK::Duration block_timestamp_to_duration(AK::Duration cluster_timestamp, u64 segment_timestamp_scale, TrackEntry const& track, i16 timestamp_offset)
{
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
    Checked<i64> timestamp_offset_in_cluster_offset = AK::clamp_to<i64>(static_cast<double>(timestamp_offset * AK::clamp_to<i64>(segment_timestamp_scale)) * track.timestamp_scale());
    timestamp_offset_in_cluster_offset.saturating_sub(AK::clamp_to<i64>(track.codec_delay()));
    // This is only mentioned in the elements specification under TrackOffset.
    // https://www.matroska.org/technical/elements.html
    timestamp_offset_in_cluster_offset.saturating_add(AK::clamp_to<i64>(track.timestamp_offset()));
    return cluster_timestamp + AK::Duration::from_nanoseconds(timestamp_offset_in_cluster_offset.value());
}

DecoderErrorOr<Vector<ByteBuffer>> SampleIterator::get_frames(Block block)
{
    Streamer streamer { m_stream_cursor };
    TRY(streamer.seek_to_position(block.data_position()));
    Vector<ByteBuffer> frames;

    if (block.lacing() == Block::Lacing::EBML) {
        auto frames_start_position = streamer.position();
        auto frame_count = TRY(streamer.read_octet()) + 1;
        Vector<u64> frame_sizes;
        frame_sizes.ensure_capacity(frame_count);

        u64 frame_size_sum = 0;
        u64 previous_frame_size;
        auto first_frame_size = TRY(streamer.read_variable_size_integer());
        frame_sizes.append(first_frame_size);
        frame_size_sum += first_frame_size;
        previous_frame_size = first_frame_size;

        for (int i = 0; i < frame_count - 2; i++) {
            auto frame_size_difference = TRY(streamer.read_variable_size_signed_integer());
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
        frame_sizes.append(block.data_size() - frame_size_sum - (streamer.position() - frames_start_position));

        for (int i = 0; i < frame_count; i++) {
            // FIXME: ReadonlyBytes instead of copying the frame data?
            auto current_frame_size = frame_sizes.at(i);
            frames.append(TRY(streamer.read_raw_octets(current_frame_size)));
        }
    } else if (block.lacing() == Block::Lacing::FixedSize) {
        auto frame_count = TRY(streamer.read_octet()) + 1;
        auto frames_data_size = block.data_size() - 1;
        if ((frames_data_size % frame_count) != 0)
            return DecoderError::corrupted("Block with fixed-size frames has non-divisible size"sv);
        auto individual_frame_size = frames_data_size / frame_count;
        for (int i = 0; i < frame_count; i++)
            frames.append(TRY(streamer.read_raw_octets(individual_frame_size)));
    } else if (block.lacing() == Block::Lacing::XIPH) {
        auto frames_start_position = streamer.position();

        auto frame_count_minus_one = TRY(streamer.read_octet());
        frames.ensure_capacity(frame_count_minus_one + 1);

        auto frame_sizes = Vector<size_t>();
        frame_sizes.ensure_capacity(frame_count_minus_one);
        for (auto i = 0; i < frame_count_minus_one; i++) {
            auto frame_size = 0;
            while (true) {
                auto octet = TRY(streamer.read_octet());
                frame_size += octet;
                if (octet < 255)
                    break;
            }
            frame_sizes.append(frame_size);
        }

        for (auto i = 0; i < frame_count_minus_one; i++)
            frames.append(TRY(streamer.read_raw_octets(frame_sizes[i])));
        frames.append(TRY(streamer.read_raw_octets(block.data_size() - (streamer.position() - frames_start_position))));
    } else {
        frames.append(TRY(streamer.read_raw_octets(block.data_size())));
    }

    return frames;
}

static void set_block_duration_to_default(Block& block, TrackEntry const& track)
{
    if (track.default_duration() != 0)
        block.set_duration(AK::Duration::from_nanoseconds(AK::clamp_to<i64>(track.default_duration())));
}

static DecoderErrorOr<Block> parse_simple_block(Streamer& streamer, AK::Duration cluster_timestamp, u64 segment_timestamp_scale, TrackEntry const& track)
{
    Block block;
    set_block_duration_to_default(block, track);

    auto content_size = TRY(streamer.read_variable_size_integer());
    auto content_end = streamer.position() + content_size;

    block.set_track_number(TRY(streamer.read_variable_size_integer()));

    auto timestamp_offset = TRY(streamer.read_i16());
    block.set_timestamp(block_timestamp_to_duration(cluster_timestamp, segment_timestamp_scale, track, timestamp_offset));

    auto flags = TRY(streamer.read_octet());
    block.set_only_keyframes((flags & (1u << 7u)) != 0);
    block.set_invisible((flags & (1u << 3u)) != 0);
    block.set_lacing(static_cast<Block::Lacing>((flags & 0b110u) >> 1u));
    block.set_discardable((flags & 1u) != 0);

    auto data_position = streamer.position();
    auto data_size = content_end - data_position;
    block.set_data(data_position, data_size);
    TRY(streamer.seek_to_position(content_end));
    return block;
}

static DecoderErrorOr<Block> parse_block_group(Streamer& streamer, AK::Duration cluster_timestamp, u64 segment_timestamp_scale, TrackEntry const& track)
{
    Block block;
    set_block_duration_to_default(block, track);

    auto parsed_a_block = false;
    TRY(parse_master_element(streamer, "BlockGroup"sv, [&](u64 element_id) -> DecoderErrorOr<ElementIterationDecision> {
        switch (element_id) {
        case BLOCK_ID: {
            if (parsed_a_block)
                return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Block group contained multiple blocks"sv);

            auto content_size = TRY(streamer.read_variable_size_integer());
            auto content_end = streamer.position() + content_size;

            block.set_track_number(TRY(streamer.read_variable_size_integer()));

            auto timestamp_offset = TRY(streamer.read_i16());
            block.set_timestamp(block_timestamp_to_duration(cluster_timestamp, segment_timestamp_scale, track, timestamp_offset));

            auto flags = TRY(streamer.read_octet());
            block.set_invisible((flags & (1u << 3)) != 0);
            block.set_lacing(static_cast<Block::Lacing>((flags & 0b110) >> 1u));

            auto data_position = streamer.position();
            auto data_size = content_end - data_position;
            block.set_data(data_position, data_size);
            TRY(streamer.seek_to_position(content_end));
            break;
        }
        case BLOCK_DURATION_ID: {
            auto duration = TRY(streamer.read_u64());
            auto duration_nanoseconds = Checked<i64>::saturating_mul(duration, segment_timestamp_scale);
            if (track.timestamp_scale() != 1)
                duration_nanoseconds = AK::clamp_to<i64>(static_cast<double>(duration_nanoseconds) * track.timestamp_scale());
            block.set_duration(AK::Duration::from_nanoseconds(duration_nanoseconds));
            break;
        }
        default:
            TRY(streamer.read_unknown_element());
            break;
        }

        return ElementIterationDecision::Continue;
    }));

    return block;
}

DecoderErrorOr<SampleIterator> Reader::create_sample_iterator(NonnullRefPtr<MediaStreamCursor> const& stream_consumer, u64 track_number)
{
    dbgln_if(MATROSKA_DEBUG, "Creating sample iterator starting at {} relative to segment at {}", m_first_cluster_position, m_segment_contents_position);
    return SampleIterator(stream_consumer, TRY(track_for_track_number(track_number)), m_segment_information.timestamp_scale(), m_segment_contents_position, m_first_cluster_position);
}

static DecoderErrorOr<CueTrackPosition> parse_cue_track_position(Streamer& streamer)
{
    CueTrackPosition track_position;

    bool had_cluster_position = false;

    TRY(parse_master_element(streamer, "CueTrackPositions"sv, [&](u64 element_id) -> DecoderErrorOr<ElementIterationDecision> {
        switch (element_id) {
        case CUE_TRACK_ID:
            track_position.set_track_number(TRY(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read CueTrackPositions track number {}", track_position.track_number());
            break;
        case CUE_CLUSTER_POSITION_ID:
            track_position.set_cluster_position(TRY(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read CueTrackPositions cluster position {}", track_position.cluster_position());
            had_cluster_position = true;
            break;
        case CUE_RELATIVE_POSITION_ID:
            track_position.set_block_offset(TRY(streamer.read_u64()));
            dbgln_if(MATROSKA_TRACE_DEBUG, "Read CueTrackPositions relative position {}", track_position.block_offset());
            break;
        case CUE_CODEC_STATE_ID:
            // Mandatory in spec, but not present in files? 0 means use TrackEntry's codec state.
            // FIXME: Do something with this value.
            dbgln_if(MATROSKA_DEBUG, "Found CodecState, skipping");
            TRY(streamer.read_unknown_element());
            break;
        case CUE_REFERENCE_ID:
            return DecoderError::not_implemented();
        default:
            TRY(streamer.read_unknown_element());
            break;
        }

        return ElementIterationDecision::Continue;
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

    TRY(parse_master_element(streamer, "CuePoint"sv, [&](u64 element_id) -> DecoderErrorOr<ElementIterationDecision> {
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
            auto timestamp = AK::Duration::from_nanoseconds(static_cast<i64>(TRY(streamer.read_u64()) * timestamp_scale));
            cue_point.set_timestamp(timestamp);
            dbgln_if(MATROSKA_DEBUG, "Read CuePoint timestamp {}ms", cue_point.timestamp().to_milliseconds());
            break;
        }
        case CUE_TRACK_POSITIONS_ID: {
            auto track_position = TRY(parse_cue_track_position(streamer));
            DECODER_TRY_ALLOC(cue_point.track_positions().try_set(track_position.track_number(), track_position));
            break;
        }
        default:
            TRY(streamer.read_unknown_element());
            break;
        }

        return ElementIterationDecision::Continue;
    }));

    if (cue_point.timestamp().is_negative())
        return DecoderError::corrupted("CuePoint was missing a timestamp"sv);

    if (cue_point.track_positions().is_empty())
        return DecoderError::corrupted("CuePoint was missing track positions"sv);

    return cue_point;
}

DecoderErrorOr<void> Reader::parse_cues(Streamer& streamer)
{
    VERIFY(m_cues.is_empty());

    auto position = TRY(find_first_top_level_element_with_id(streamer, "Cues"sv, CUES_ID));
    if (!position.has_value())
        return {};
    TRY(streamer.seek_to_position(position.release_value()));
    if (TRY(streamer.read_variable_size_integer(false)) != CUES_ID) {
        dbgln("Unexpected Matroska element when seeking to the Cues element, skipping parsing.");
        return {};
    }

    TRY(parse_master_element(streamer, "Cues"sv, [&](u64 element_id) -> DecoderErrorOr<ElementIterationDecision> {
        switch (element_id) {
        case CUE_POINT_ID: {
            auto cue_point = TRY(parse_cue_point(streamer, m_segment_information.timestamp_scale()));

            // FIXME: Verify that these are already in order of timestamp. If they are not, return a corrupted error for now,
            //        but if it turns out that Matroska files with out-of-order cue points are valid, sort them instead.

            for (auto const& [track_id, track_position] : cue_point.track_positions()) {
                auto& cue_points_for_track = m_cues.ensure(track_id);
                cue_points_for_track.append({ cue_point.timestamp(), track_position });
            }
            break;
        }
        default:
            return DecoderError::format(DecoderErrorCategory::Corrupted, "Unknown Cues child ID {:#010x}", element_id);
        }

        return ElementIterationDecision::Continue;
    }));

    return {};
}

DecoderErrorOr<void> Reader::seek_to_cue_for_timestamp(SampleIterator& iterator, AK::Duration const& timestamp, Vector<TrackCuePoint> const& cue_points, CuePointTarget target)
{
    // Take a guess at where in the cues the timestamp will be and correct from there.
    auto duration = m_segment_information.duration();
    size_t index = 0;
    if (duration.has_value())
        index = clamp(((timestamp.to_nanoseconds() * cue_points.size()) / duration->to_nanoseconds()), 0, cue_points.size() - 1);

    auto const* prev_cue_point = &cue_points[index];
    dbgln_if(MATROSKA_DEBUG, "Finding Matroska cue points for timestamp {}ms starting from cue at {}ms", timestamp.to_milliseconds(), prev_cue_point->timestamp.to_milliseconds());

    if (prev_cue_point->timestamp == timestamp) {
        TRY(iterator.seek_to_cue_point(*prev_cue_point, target));
        return {};
    }

    if (prev_cue_point->timestamp > timestamp) {
        while (index > 0 && prev_cue_point->timestamp > timestamp) {
            prev_cue_point = &cue_points[--index];
            dbgln_if(MATROSKA_DEBUG, "Checking previous cue point {}ms", prev_cue_point->timestamp.to_milliseconds());
        }
        TRY(iterator.seek_to_cue_point(*prev_cue_point, target));
        return {};
    }

    while (++index < cue_points.size()) {
        auto const& cue_point = cue_points[index];
        dbgln_if(MATROSKA_DEBUG, "Checking future cue point {}ms", cue_point.timestamp.to_milliseconds());
        if (cue_point.timestamp > timestamp)
            break;
        prev_cue_point = &cue_point;
    }

    TRY(iterator.seek_to_cue_point(*prev_cue_point, target));
    return {};
}

static DecoderErrorOr<void> search_clusters_for_keyframe_before_timestamp(SampleIterator& iterator, AK::Duration const& timestamp)
{
#if MATROSKA_DEBUG
    size_t inter_frames_count;
#endif
    SampleIterator last_keyframe = iterator;

    while (true) {
        SampleIterator rewind_iterator = iterator;
        auto block_result = iterator.next_block();
        if (block_result.is_error()) {
            if (block_result.error().category() == DecoderErrorCategory::EndOfStream)
                break;
            return block_result.release_error();
        }

        auto block = block_result.release_value();
        if (block.timestamp() > timestamp)
            break;

        if (block.only_keyframes()) {
            last_keyframe = rewind_iterator;
#if MATROSKA_DEBUG
            inter_frames_count = 0;
#endif
        }

#if MATROSKA_DEBUG
        inter_frames_count++;
#endif
    }

#if MATROSKA_DEBUG
    dbgln("Seeked to a keyframe with {} inter frames to skip", inter_frames_count);
#endif
    iterator = move(last_keyframe);

    return {};
}

bool Reader::has_cues_for_track(u64 track_number)
{
    return m_cues.contains(track_number);
}

DecoderErrorOr<SampleIterator> Reader::seek_to_random_access_point(SampleIterator iterator, AK::Duration timestamp)
{
    timestamp -= AK::Duration::from_nanoseconds(AK::clamp_to<i64>(iterator.m_track->seek_pre_roll()));

    auto cue_points = cue_points_for_track(iterator.m_track->track_number());
    auto seek_target = CuePointTarget::Block;

    // If no cues are present for the track, use the first track's cues.
    if (!cue_points.has_value() && !m_cues.is_empty()) {
        auto first_track_number = m_tracks.begin()->key;
        cue_points = m_cues.get(first_track_number);
        seek_target = CuePointTarget::Cluster;
    }

    if (cue_points.has_value()) {
        TRY(seek_to_cue_for_timestamp(iterator, timestamp, cue_points.value(), seek_target));
        VERIFY(iterator.last_timestamp().has_value());
    }

    if (!iterator.last_timestamp().has_value() || timestamp < iterator.last_timestamp().value()) {
        // If the timestamp is before the iterator's current position, then we need to start from the beginning of the Segment.
        if (timestamp > AK::Duration::zero())
            warnln("Seeking track {} to {}s required restarting the sample iterator from the start, streaming may be broken for this file.", timestamp, iterator.m_track->track_number());
        iterator = TRY(create_sample_iterator(iterator.m_stream_cursor, iterator.m_track->track_number()));
        TRY(search_clusters_for_keyframe_before_timestamp(iterator, timestamp));
        return iterator;
    }

    TRY(search_clusters_for_keyframe_before_timestamp(iterator, timestamp));
    return iterator;
}

Optional<Vector<TrackCuePoint> const&> Reader::cue_points_for_track(u64 track_number)
{
    return m_cues.get(track_number);
}

DecoderErrorOr<Block> SampleIterator::next_block()
{
    Streamer streamer { m_stream_cursor };
    TRY(streamer.seek_to_position(m_position));

    // Remove the last timestamp from this iterator so that if we encounter an error, especially EOS,
    // we will always seek the sample iterator, ensuring that we will decode the last block again.
    m_last_timestamp = {};

    Optional<Block> block;

    while (true) {
#if MATROSKA_TRACE_DEBUG
        auto element_position = streamer.position();
#endif
        auto element_id = TRY(streamer.read_variable_size_integer(false));
#if MATROSKA_TRACE_DEBUG
        dbgln("Iterator found element with ID {:#010x} at offset {} within the segment.", element_id, element_position);
#endif

        if (element_id == CLUSTER_ELEMENT_ID) {
            dbgln_if(MATROSKA_DEBUG, "  Iterator is parsing new cluster.");
            m_current_cluster = TRY(parse_cluster(streamer, m_segment_timestamp_scale));
        } else if (element_id == SIMPLE_BLOCK_ID) {
            dbgln_if(MATROSKA_TRACE_DEBUG, "  Iterator is parsing a new simple block.");
            auto candidate_block = TRY(parse_simple_block(streamer, m_current_cluster->timestamp(), m_segment_timestamp_scale, m_track));
            if (candidate_block.track_number() == m_track->track_number())
                block = move(candidate_block);
        } else if (element_id == BLOCK_GROUP_ID) {
            dbgln_if(MATROSKA_TRACE_DEBUG, "  Iterator is parsing a new block group.");
            auto candidate_block = TRY(parse_block_group(streamer, m_current_cluster->timestamp(), m_segment_timestamp_scale, m_track));
            if (candidate_block.track_number() == m_track->track_number())
                block = move(candidate_block);
        } else if (element_id == SEGMENT_ELEMENT_ID) {
            dbgln("Malformed file, found a segment element within the root segment element. Jumping into it.");
            [[maybe_unused]] auto segment_size = TRY(streamer.read_variable_size_integer());
        } else {
            dbgln_if(MATROSKA_TRACE_DEBUG, "  Iterator is skipping unknown element with ID {:#010x}.", element_id);
            TRY(streamer.read_unknown_element());
        }

        m_position = streamer.position();
        if (block.has_value()) {
            m_last_timestamp = block->timestamp();
            return block.release_value();
        }
    }

    VERIFY_NOT_REACHED();
}

SampleIterator::SampleIterator(NonnullRefPtr<MediaStreamCursor> const& stream_cursor, TrackEntry& track, u64 timestamp_scale, size_t segment_contents_position, size_t position)
    : m_stream_cursor(stream_cursor)
    , m_track(track)
    , m_segment_timestamp_scale(timestamp_scale)
    , m_segment_contents_position(segment_contents_position)
    , m_position(position)
{
}

SampleIterator::~SampleIterator() = default;

DecoderErrorOr<void> SampleIterator::seek_to_cue_point(TrackCuePoint const& cue_point, CuePointTarget target)
{
    // This is a private function. The position getter can return optional, but the caller should already know that this track has a position.
    auto const& cue_position = cue_point.position;
    Streamer streamer { m_stream_cursor };
    TRY(streamer.seek_to_position(m_segment_contents_position + cue_position.cluster_position()));

    auto element_id = TRY(streamer.read_variable_size_integer(false));
    if (element_id != CLUSTER_ELEMENT_ID)
        return DecoderError::corrupted("Cue point's cluster position didn't point to a cluster"sv);

    m_current_cluster = TRY(parse_cluster(streamer, m_segment_timestamp_scale));
    dbgln_if(MATROSKA_DEBUG, "SampleIterator set to cue point at timestamp {}ms", m_current_cluster->timestamp().to_milliseconds());

    if (target == CuePointTarget::Cluster) {
        m_position = streamer.position();
        m_last_timestamp = m_current_cluster->timestamp();
    } else {
        m_position = streamer.position() + cue_position.block_offset();
        m_last_timestamp = cue_point.timestamp;
    }
    return {};
}

Streamer::Streamer(NonnullRefPtr<MediaStreamCursor> const& stream_cursor)
    : m_stream_cursor(stream_cursor)
{
}

Streamer::~Streamer() = default;

DecoderErrorOr<String> Streamer::read_string()
{
    auto string_length = TRY(read_variable_size_integer());
    auto string_data = TRY(read_raw_octets(string_length));
    auto const* string_data_raw = reinterpret_cast<char const*>(string_data.data());
    auto string_value = String::from_utf8(ReadonlyBytes(string_data.data(), strnlen(string_data_raw, string_length)));
    if (string_value.is_error())
        return DecoderError::format(DecoderErrorCategory::Invalid, "String is not valid UTF-8");
    return string_value.release_value();
}

DecoderErrorOr<u8> Streamer::read_octet()
{
    u8 result;
    Bytes bytes { &result, 1 };
    TRY(m_stream_cursor->read_into(bytes));
    return bytes[0];
}

DecoderErrorOr<i16> Streamer::read_i16()
{
    return (TRY(read_octet()) << 8) | TRY(read_octet());
}

DecoderErrorOr<u64> Streamer::read_variable_size_integer(bool mask_length)
{
    dbgln_if(MATROSKA_TRACE_DEBUG, "Reading VINT from offset {:p}", position());
    auto length_descriptor = TRY(read_octet());
    dbgln_if(MATROSKA_TRACE_DEBUG, "Reading VINT, first byte is {:#02x}", length_descriptor);
    if (length_descriptor == 0)
        return DecoderError::format(DecoderErrorCategory::Invalid, "read_variable_size_integer: Length descriptor has no terminating set bit");
    size_t length = 0;
    while (length < 8) {
        if (((length_descriptor >> (8 - length)) & 1) == 1)
            break;
        length++;
    }
    dbgln_if(MATROSKA_TRACE_DEBUG, "Reading VINT of total length {}", length);
    if (length > 8)
        return DecoderError::format(DecoderErrorCategory::Invalid, "read_variable_size_integer: Length is too large");

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

DecoderErrorOr<i64> Streamer::read_variable_size_signed_integer()
{
    auto length_descriptor = TRY(read_octet());
    if (length_descriptor == 0)
        return DecoderError::format(DecoderErrorCategory::Invalid, "read_variable_sized_signed_integer: Length descriptor has no terminating set bit");
    i64 length = 0;
    while (length < 8) {
        if (((length_descriptor >> (8 - length)) & 1) == 1)
            break;
        length++;
    }
    if (length > 8)
        return DecoderError::format(DecoderErrorCategory::Invalid, "read_variable_size_integer: Length is too large");

    i64 result = length_descriptor & ~(1u << (8 - length));
    for (i64 i = 1; i < length; i++) {
        u8 next_octet = TRY(read_octet());
        result = (result << 8u) | next_octet;
    }
    result -= AK::exp2<i64>(length * 7 - 1) - 1;
    return result;
}

DecoderErrorOr<ByteBuffer> Streamer::read_raw_octets(size_t num_octets)
{
    auto result = MUST(ByteBuffer::create_uninitialized(num_octets));
    auto bytes = result.bytes();
    TRY(m_stream_cursor->read_into(bytes));
    return result;
}

DecoderErrorOr<u64> Streamer::read_u64()
{
    auto integer_length = TRY(read_variable_size_integer());
    u64 result = 0;
    for (size_t i = 0; i < integer_length; i++) {
        result = (result << 8u) + TRY(read_octet());
    }
    return result;
}

DecoderErrorOr<double> Streamer::read_float()
{
    auto length = TRY(read_variable_size_integer());
    if (length != 4u && length != 8u)
        return DecoderError::format(DecoderErrorCategory::Invalid, "Float size must be 4 or 8 bytes");

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

DecoderErrorOr<void> Streamer::read_unknown_element()
{
    auto element_length = TRY(read_variable_size_integer());
    dbgln_if(MATROSKA_TRACE_DEBUG, "Skipping unknown element of size {}.", element_length);
    TRY(m_stream_cursor->seek(element_length, AK::SeekMode::FromCurrentPosition));
    return {};
}

size_t Streamer::position() const
{
    return m_stream_cursor->position();
}

DecoderErrorOr<void> Streamer::seek_to_position(size_t position)
{
    return m_stream_cursor->seek(position, AK::SeekMode::SetPosition);
}

}
