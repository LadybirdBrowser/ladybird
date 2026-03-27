/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibMedia/Containers/Matroska/ElementIDs.h>
#include <LibMedia/Containers/Matroska/Reader.h>
#include <LibMedia/MediaStream.h>
#include <LibWeb/MediaSourceExtensions/WebMByteStreamParser.h>

namespace Web::MediaSourceExtensions {

using namespace Media::Matroska;

WebMByteStreamParser::WebMByteStreamParser() = default;
WebMByteStreamParser::~WebMByteStreamParser() = default;

Media::DecoderErrorOr<void> WebMByteStreamParser::skip_ignored_bytes(Media::MediaStreamCursor& cursor)
{
    Streamer streamer { cursor };

    if (!m_segment_information.has_value() || !m_cluster_has_been_read) {
        // https://w3c.github.io/mse-byte-stream-format-webm/#webm-init-segments
        // The user agent MUST accept and ignore any elements other than an EBML Header or a Cluster that occur before,
        // in between, or after the Segment Information and Track elements.
        while (true) {
            auto position_before = cursor.position();
            auto element_id = TRY(streamer.read_element_id());

            if (element_id == EBML_MASTER_ELEMENT_ID
                || element_id == CLUSTER_ELEMENT_ID) {
                TRY(cursor.seek(position_before, SeekMode::SetPosition));
                break;
            }

            TRY(streamer.read_unknown_element());
        }
    } else if (m_cluster_has_been_read && !m_current_media_segment_data.has_value()) {
        // https://www.w3.org/TR/mse-byte-stream-format-webm/#webm-media-segments
        // The user agent MUST accept and ignore Cues or Chapters elements that follow a Cluster element.
        while (true) {
            auto position_before = cursor.position();
            auto element_id = TRY(streamer.read_element_id());

            if (!first_is_one_of(element_id, CUES_ID, CHAPTERS_ELEMENT_ID)) {
                TRY(cursor.seek(position_before, SeekMode::SetPosition));
                break;
            }

            TRY(streamer.read_unknown_element());
        }
    }
    return {};
}

Media::DecoderErrorOr<SegmentType> WebMByteStreamParser::sniff_segment_type(Media::MediaStreamCursor& cursor)
{
    auto position_before = cursor.position();
    Streamer streamer { cursor };

    auto element_id_or_error = streamer.read_element_id();
    // Always restore cursor — sniffing must not consume bytes.
    TRY(cursor.seek(position_before, SeekMode::SetPosition));

    if (element_id_or_error.is_error()) {
        if (element_id_or_error.error().category() == Media::DecoderErrorCategory::EndOfStream)
            return SegmentType::Incomplete;
        return element_id_or_error.release_error();
    }

    auto element_id = element_id_or_error.value();

    // https://w3c.github.io/mse-byte-stream-format-webm/#webm-init-segments
    // 1. The initialization segment MUST start with an EBML Header element, followed by a Segment header.
    if (element_id == EBML_MASTER_ELEMENT_ID)
        return SegmentType::InitializationSegment;

    // https://w3c.github.io/mse-byte-stream-format-webm/#webm-media-segments
    // A WebM media segment is a single Cluster element.
    if (element_id == CLUSTER_ELEMENT_ID)
        return SegmentType::MediaSegment;

    return SegmentType::Unknown;
}

// https://w3c.github.io/mse-byte-stream-format-webm/#webm-init-segments
Media::DecoderErrorOr<void> WebMByteStreamParser::parse_initialization_segment(Media::MediaStreamCursor& cursor)
{
    Streamer streamer { cursor };
    ArmedScopeGuard restore_position = [&cursor, prior_position = static_cast<i64>(cursor.position())] {
        MUST(cursor.seek(prior_position, SeekMode::SetPosition));
    };

    // The initialization segment MUST start with an EBML Header element...
    auto ebml_element_id = TRY(streamer.read_element_id());
    if (ebml_element_id != EBML_MASTER_ELEMENT_ID)
        return Media::DecoderError::corrupted("WebM initialization segments must start with an EBML header"sv);

    auto header = TRY(Reader::parse_ebml_header(streamer));

    // ...followed by a Segment header.
    auto segment_element_id = TRY(streamer.read_element_id());
    if (segment_element_id != SEGMENT_ELEMENT_ID)
        return Media::DecoderError::corrupted("Expected a Segment element after the EBML header"sv);

    // The size value in the Segment header MUST signal an 'unknown size' or contain a value large enough
    // to include the Segment Information and Track elements that follow.
    auto segment_start = streamer.position();
    auto segment_size = TRY(streamer.read_element_size());

    // A Segment Information element and a Track element MUST appear, in that order, after the Segment header and
    // before any further EBML Header or Cluster elements.

    // The user agent MUST accept and ignore any elements other than an EBML Header or a Cluster that occur before, in
    // between, or after the Segment Information and Track elements.
    bool found_segment_info = false;
    bool found_tracks = false;

    while (!found_segment_info || !found_tracks) {
        auto child_position = streamer.position();
        auto child_element_id = TRY(streamer.read_element_id());

        // NB: Stop reading the initialization segment upon finding another EBML Header, or a Cluster element.
        if (child_element_id == EBML_MASTER_ELEMENT_ID || child_element_id == CLUSTER_ELEMENT_ID) {
            TRY(cursor.seek(child_position, SeekMode::SetPosition));
            break;
        }

        if (child_element_id == SEGMENT_INFORMATION_ELEMENT_ID) {
            if (found_segment_info)
                return Media::DecoderError::corrupted("Found a duplicate Segment Information element"sv);

            m_segment_information = TRY(Reader::parse_segment_information_element(streamer));
            found_segment_info = true;
        } else if (child_element_id == TRACK_ELEMENT_ID) {
            if (!found_segment_info)
                return Media::DecoderError::corrupted("Track element appeared before Segment Information"sv);
            if (found_tracks)
                return Media::DecoderError::corrupted("Found a duplicate Track element"sv);

            m_track_entries.clear();
            TRY(Reader::parse_master_element(streamer, "Track"sv, [&](u64 element_id) -> Media::DecoderErrorOr<ElementIterationDecision> {
                if (element_id == TRACK_ENTRY_ID) {
                    auto track_entry = TRY(Reader::parse_track_entry(streamer));
                    if (m_track_entries.set(track_entry->track_number(), track_entry) != HashSetResult::InsertedNewEntry)
                        return Media::DecoderError::corrupted("Found a duplicate track number"sv);
                } else {
                    TRY(streamer.read_unknown_element());
                }
                return ElementIterationDecision::Continue;
            }));
            found_tracks = true;
        } else {
            TRY(streamer.read_unknown_element());
        }
    }

    if (segment_size.has_value() && streamer.position() > segment_start + segment_size.value())
        return Media::DecoderError::corrupted("Segment size was smaller than its required elements"sv);
    if (!found_segment_info)
        return Media::DecoderError::corrupted("Initialization segment had no Segment Info element"sv);
    if (!found_tracks)
        return Media::DecoderError::corrupted("Initialization segment had no Track element"sv);

    m_video_tracks.clear();
    m_audio_tracks.clear();
    m_text_tracks.clear();

    for (auto const& [track_number, track_entry] : m_track_entries) {
        auto* maybe_tracks_for_type = [&] -> Vector<Media::Track>* {
            switch (track_entry->track_type()) {
            case TrackEntry::TrackType::Video:
                return &m_video_tracks;
            case TrackEntry::TrackType::Audio:
                return &m_audio_tracks;
            case TrackEntry::TrackType::Subtitle:
            case TrackEntry::TrackType::Metadata:
                return &m_text_tracks;
            default:
                return nullptr;
            }
        }();
        if (!maybe_tracks_for_type)
            continue;
        auto& tracks_for_type = *maybe_tracks_for_type;
        tracks_for_type.append(track_from_track_entry(track_entry, tracks_for_type.is_empty()));
        m_track_block_contexts.set(track_number, TrackBlockContext::from_track_entry(track_entry));
    }

    m_current_media_segment_data.clear();
    restore_position.disarm();
    return {};
}

// https://w3c.github.io/mse-byte-stream-format-webm/#webm-media-segments
Media::DecoderErrorOr<ParseMediaSegmentResult> WebMByteStreamParser::parse_media_segment(Media::MediaStreamCursor& cursor)
{
    Streamer streamer { cursor };
    ParseMediaSegmentResult result;

    // NB: If we haven't started parsing a cluster yet, try to read the new cluster's size and base timestamp.
    if (!m_current_media_segment_data.has_value()) {
        auto first_element_id = TRY(streamer.read_element_id());
        if (first_element_id != CLUSTER_ELEMENT_ID)
            return Media::DecoderError::format(Media::DecoderErrorCategory::Invalid, "Media segment did not begin with a Cluster element");

        auto cluster_size = TRY(streamer.read_element_size());
        auto cluster_data_position = streamer.position();

        // - The Timecode element MUST appear before any Block & SimpleBlock elements in a Cluster.
        auto first_child_element_id = TRY(streamer.read_element_id());
        if (first_child_element_id != TIMESTAMP_ID)
            return Media::DecoderError::format(Media::DecoderErrorCategory::Invalid, "The Cluster element did not begin with a Timecode element");

        auto timestamp_scale = m_segment_information->timestamp_scale();
        auto timecode = AK::Duration::from_nanoseconds(AK::clamp_to<i64>(TRY(streamer.read_u64()) * timestamp_scale));

        m_current_media_segment_data = MediaSegmentParsingData {
            .timecode = timecode,
            .remaining_bytes = cluster_size.map([&](auto size) { return cluster_data_position + size - streamer.position(); }),
        };
        m_cluster_has_been_read = true;
    }

    VERIFY(m_current_media_segment_data.has_value());

    // NB: Read all the blocks we can until we reach the end of the cluster.
    auto& timecode = m_current_media_segment_data->timecode;
    auto& remaining_bytes = m_current_media_segment_data->remaining_bytes;
    auto& last_block_timestamp = m_current_media_segment_data->last_block_timestamp;
    auto& seen_track_numbers = m_current_media_segment_data->seen_track_numbers;
    while (!remaining_bytes.has_value() || remaining_bytes.value() > 0) {
        auto block_position = streamer.position();

        auto try_read_block = [&]() -> Media::DecoderErrorOr<IterationDecision> {
            auto element_id = TRY(streamer.read_element_id());

            if (element_id == SIMPLE_BLOCK_ID || element_id == BLOCK_GROUP_ID) {
                auto block = TRY([&] -> Media::DecoderErrorOr<Block> {
                    if (element_id == SIMPLE_BLOCK_ID)
                        return TRY(Reader::parse_simple_block(streamer, timecode, m_segment_information->timestamp_scale(), m_track_block_contexts));
                    VERIFY(element_id == BLOCK_GROUP_ID);
                    return Reader::parse_block_group(streamer, timecode, m_segment_information->timestamp_scale(), m_track_block_contexts);
                }());

                VERIFY(block.timestamp().has_value());

                // - Block & SimpleBlock elements are in time increasing order consistent with [WEBM].
                if (block.timestamp().value() < last_block_timestamp)
                    return Media::DecoderError::corrupted("Block timestamps are not in increasing order"sv);
                last_block_timestamp = block.timestamp().value();

                // - If the most recent WebM initialization segment describes multiple tracks, then blocks from all the
                //   tracks MUST be interleaved in time increasing order. At least one block from all audio and video
                //   tracks MUST be present.
                seen_track_numbers.set(block.track_number());

                auto data_position = block.data_position();
                auto data_size = block.data_size();
                auto current_position = streamer.position();
                TRY(cursor.seek(data_position, SeekMode::SetPosition));

                // FIXME: Support lacing.
                if (block.lacing() != Block::Lacing::None)
                    return Media::DecoderError::with_description(Media::DecoderErrorCategory::NotImplemented, "Block lacing is not supported"sv);
                auto frame_data = TRY(streamer.read_raw_octets(data_size));
                TRY(cursor.seek(current_position, SeekMode::SetPosition));

                auto track_entry = m_track_entries.get(block.track_number());
                auto is_video = track_entry.has_value() && (*track_entry)->track_type() == Media::Matroska::TrackEntry::TrackType::Video;

                Media::CodedFrame::AuxiliaryData aux_data = is_video
                    ? Media::CodedFrame::AuxiliaryData { Media::CodedVideoFrameData {} }
                    : Media::CodedFrame::AuxiliaryData { Media::CodedAudioFrameData {} };

                result.coded_frames.append({
                    .track_number = block.track_number(),
                    .coded_frame = Media::CodedFrame(
                        block.timestamp().value(),
                        block.duration().value_or(AK::Duration::zero()),
                        block.only_keyframes() ? Media::FrameFlags::Keyframe : Media::FrameFlags::None,
                        move(frame_data),
                        aux_data),
                });
                return IterationDecision::Continue;
            }

            // - The Cluster header MAY contain an "unknown" size value. If it does then the end of the cluster is
            //   reached when another Cluster header or an element header that indicates the start of a WebM
            //   initialization segment is encountered.
            if (!remaining_bytes.has_value() && first_is_one_of(element_id, EBML_MASTER_ELEMENT_ID, CLUSTER_ELEMENT_ID)) {
                return IterationDecision::Break;
            }

            TRY(streamer.read_unknown_element());
            return IterationDecision::Continue;
        };

        auto read_block_result = try_read_block();
        if (read_block_result.is_error()) {
            if (read_block_result.error().category() == Media::DecoderErrorCategory::EndOfStream) {
                TRY(cursor.seek(block_position, SeekMode::SetPosition));
                return result;
            }
            return read_block_result.release_error();
        }

        if (read_block_result.value() == IterationDecision::Break)
            break;

        if (remaining_bytes.has_value())
            remaining_bytes.value() -= streamer.position() - block_position;
    }

    for (auto& [track_number, track_entry] : m_track_entries) {
        if (!seen_track_numbers.contains(track_number))
            return Media::DecoderError::format(Media::DecoderErrorCategory::Corrupted, "Found no blocks for track number {}", track_number);
    }

    m_current_media_segment_data.clear();

    result.completed_segment = true;
    return result;
}

}
