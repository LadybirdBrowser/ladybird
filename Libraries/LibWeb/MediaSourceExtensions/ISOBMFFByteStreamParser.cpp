/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/GenericShorthands.h>
#include <AK/QuickSort.h>
#include <LibMedia/Containers/ISOBMFF/Reader.h>
#include <LibMedia/Containers/ISOBMFF/Streamer.h>
#include <LibMedia/MediaStream.h>
#include <LibWeb/MediaSourceExtensions/ISOBMFFByteStreamParser.h>

namespace Web::MediaSourceExtensions {

using namespace Media::ISOBMFF;

// https://w3c.github.io/mse-byte-stream-format-isobmff/#iso-media-segments
// Valid top-level boxes defined in [[ISOBMFF]] other than ftyp, moov, styp, moof, and mdat are allowed to
// appear between the end of an initialization segment or media segment and before the beginning of a new media
// segment.
static bool box_begins_a_segment(FourCC type)
{
    return first_is_one_of(type, FILE_TYPE_BOX, MOVIE_BOX, SEGMENT_TYPE_BOX, MOVIE_FRAGMENT_BOX, MEDIA_DATA_BOX);
}

static bool is_ignorable_top_level_box(FourCC type)
{
    return is_top_level_box(type) && !box_begins_a_segment(type);
}

static Media::DecoderErrorOr<BoxHeader> read_top_level_box_header(Streamer& streamer)
{
    auto header = TRY(Reader::read_box_header(streamer));
    if (!header.content_size.has_value())
        return Media::DecoderError::format(Media::DecoderErrorCategory::Corrupted, "{} box may not extend to the end of the stream", header.type);
    return header;
}

ISOBMFFByteStreamParser::ISOBMFFByteStreamParser() = default;
ISOBMFFByteStreamParser::~ISOBMFFByteStreamParser() = default;

bool ISOBMFFByteStreamParser::supports_codec(StringView codec_string, Media::CodecID codec_id)
{
    if (!Reader::supports_codec(codec_id))
        return false;
    if (codec_id == Media::CodecID::VP8)
        return false;
    if (codec_id == Media::CodecID::VP9)
        return codec_string.starts_with("vp09"sv);
    if (codec_id == Media::CodecID::MP3)
        return codec_string.starts_with("mp4a."sv);
    return true;
}

Media::DecoderErrorOr<void> ISOBMFFByteStreamParser::skip_ignored_bytes(Media::MediaStreamCursor& cursor)
{
    // NB: The cursor sits within the Media Data boxes of a partly read media segment, not at a box that could be
    //     skipped.
    if (m_current_media_segment.has_value())
        return {};

    Streamer streamer { cursor };

    while (true) {
        auto position_before = cursor.position();

        auto skip_box = [&]() -> Media::DecoderErrorOr<IterationDecision> {
            auto header = TRY(read_top_level_box_header(streamer));
            if (box_begins_a_segment(header.type))
                return IterationDecision::Break;
            if (!is_top_level_box(header.type))
                return Media::DecoderError::format(Media::DecoderErrorCategory::Corrupted, "Found a {} box that is not valid at the top level", header.type);

            // These boxes MUST be accepted and ignored by the user agent and are not considered part of the
            // media segment in this specification.
            TRY(streamer.seek_to_position(header.end_position().value()));
            return IterationDecision::Continue;
        };

        auto skip_result = skip_box();
        // NB: Restore the cursor to the start of the box so that an incomplete box is not dropped from the
        //     input buffer before it can be skipped.
        if (skip_result.is_error() || skip_result.value() == IterationDecision::Break) {
            TRY(cursor.seek_to_position(position_before));
            if (skip_result.is_error())
                return skip_result.release_error();
            return {};
        }
    }
}

Media::DecoderErrorOr<SegmentType> ISOBMFFByteStreamParser::sniff_segment_type(Media::MediaStreamCursor& cursor)
{
    // NB: The samples of a partly read media segment remain to be read before any other segment can begin.
    if (m_current_media_segment.has_value())
        return SegmentType::MediaSegment;

    auto position_before = cursor.position();
    Streamer streamer { cursor };

    auto header_or_error = read_top_level_box_header(streamer);
    // NB: Sniffing must not consume bytes, so the cursor is always restored.
    TRY(cursor.seek_to_position(position_before));

    if (header_or_error.is_error()) {
        if (header_or_error.error().category() == Media::DecoderErrorCategory::EndOfStream)
            return SegmentType::Incomplete;
        return header_or_error.release_error();
    }

    // https://w3c.github.io/mse-byte-stream-format-isobmff/#iso-init-segments
    // An ISO BMFF initialization segment is defined in this specification as a single File Type Box (ftyp)
    // followed by a single Movie Box (moov).
    if (header_or_error.value().type == FILE_TYPE_BOX)
        return SegmentType::InitializationSegment;

    // https://w3c.github.io/mse-byte-stream-format-isobmff/#iso-media-segments
    // An ISO BMFF media segment is defined in this specification as one optional Segment Type Box (styp)
    // followed by a single Movie Fragment Box (moof) followed by one or more Media Data Boxes (mdat).
    if (first_is_one_of(header_or_error.value().type, SEGMENT_TYPE_BOX, MOVIE_FRAGMENT_BOX))
        return SegmentType::MediaSegment;

    return SegmentType::Unknown;
}

// https://w3c.github.io/mse-byte-stream-format-isobmff/#iso-init-segments
Media::DecoderErrorOr<void> ISOBMFFByteStreamParser::parse_initialization_segment(Media::MediaStreamCursor& cursor)
{
    Streamer streamer { cursor };
    ArmedScopeGuard restore_position = [&cursor, prior_position = cursor.position()] {
        MUST(cursor.seek_to_position(prior_position));
    };

    // An ISO BMFF initialization segment is defined in this specification as a single File Type Box (ftyp)...
    auto file_type_header = TRY(read_top_level_box_header(streamer));
    if (file_type_header.type != FILE_TYPE_BOX)
        return Media::DecoderError::corrupted("ISO BMFF initialization segments must start with a File Type box"sv);
    // FIXME: Run the append error algorithm if the File Type Box contains a major_brand or compatible_brand
    //        that we do not support.
    TRY(Reader::parse_file_type_box(streamer, file_type_header));
    TRY(streamer.seek_to_position(file_type_header.end_position().value()));

    // Valid top-level boxes such as pdin, free, and sidx are allowed to appear before the moov box. These boxes MUST
    // be accepted and ignored by the user agent and are not considered part of the initialization segment in this
    // specification.
    auto next_header = TRY(read_top_level_box_header(streamer));
    while (next_header.type != MOVIE_BOX) {
        if (!is_ignorable_top_level_box(next_header.type))
            return Media::DecoderError::format(Media::DecoderErrorCategory::Corrupted, "Found a {} box where the Movie box was expected", next_header.type);
        TRY(streamer.seek_to_position(next_header.end_position().value()));
        next_header = TRY(read_top_level_box_header(streamer));
    }

    // ...followed by a single Movie Box (moov).

    auto movie = TRY(Reader::parse_movie_box(streamer, next_header));
    TRY(streamer.seek_to_position(next_header.end_position().value()));

    // The user agent MUST run the append error algorithm if any of the following conditions are met:

    //   - A Movie Extends (mvex) box is not contained in the Movie (moov) box to indicate that Movie Fragments
    //     are to be expected.
    if (!movie.has_movie_extends_box)
        return Media::DecoderError::corrupted("Movie box contains no Movie Extends box"sv);

    for (auto const& [track_id, track_entry] : movie.tracks) {
        //   - The tracks in the Movie Box contain samples (i.e., the entry_count in the stts, stsc or stco
        //     boxes are not set to zero).
        if (track_entry->sample_table_entry_count != 0)
            return Media::DecoderError::format(Media::DecoderErrorCategory::Corrupted, "Track {} in the Movie box contains samples", track_id);
    }

    m_duration = movie.duration_as_time();
    m_track_entries = move(movie.tracks);

    m_video_tracks.clear();
    m_audio_tracks.clear();
    m_text_tracks.clear();
    m_track_fragment_contexts.clear();
    m_current_media_segment.clear();

    for (auto const& [track_id, track_entry] : m_track_entries) {
        auto* maybe_tracks_for_type = [&] -> Vector<Media::Track>* {
            switch (track_entry->handler_type) {
            case HandlerType::Video:
                return &m_video_tracks;
            case HandlerType::Audio:
                return &m_audio_tracks;
            case HandlerType::Text:
                return &m_text_tracks;
            case HandlerType::Unknown:
                return nullptr;
            }
            VERIFY_NOT_REACHED();
        }();

        m_track_fragment_contexts.set(track_id, {
                                                    .timescale = track_entry->timescale,
                                                    .composition_to_presentation_offset = track_entry->composition_to_presentation_offset,
                                                    .sample_description_count = track_entry->sample_entries.size(),
                                                    .sample_defaults = track_entry->fragment_defaults,
                                                });

        if (!maybe_tracks_for_type)
            continue;
        auto& tracks_for_type = *maybe_tracks_for_type;
        tracks_for_type.append(track_from_track_entry(track_entry, tracks_for_type.is_empty()));
    }

    restore_position.disarm();
    return {};
}

// https://w3c.github.io/mse-byte-stream-format-isobmff/#iso-media-segments
Media::DecoderErrorOr<ISOBMFFByteStreamParser::MediaSegment> ISOBMFFByteStreamParser::parse_movie_fragment(Streamer& streamer)
{
    auto header = TRY(read_top_level_box_header(streamer));

    // An ISO BMFF media segment is defined in this specification as one optional Segment Type Box (styp)...
    if (header.type == SEGMENT_TYPE_BOX) {
        // FIXME: Run the append error algorithm if this Segment Type Box is not compatible with the
        //        initialization segment's File Type Box. Note that segments of a DASH presentation carry
        //        brands such as 'msdh' that never appear in the initialization segment's brands, so this
        //        cannot be a comparison of the two boxes' brand lists.
        TRY(Reader::parse_file_type_box(streamer, header));

        TRY(streamer.seek_to_position(header.end_position().value()));
        header = TRY(read_top_level_box_header(streamer));
    }

    // AD-HOC: Mirror the top-level box skip allowed before the Movie Box (moov) in an initialization segment here.
    //         Media in the wild does not follow spec by immediately following styp with moof.
    while (header.type != MOVIE_FRAGMENT_BOX) {
        if (!is_ignorable_top_level_box(header.type))
            return Media::DecoderError::format(Media::DecoderErrorCategory::Corrupted, "Found a {} box where the Movie Fragment box was expected", header.type);
        TRY(streamer.seek_to_position(header.end_position().value()));
        header = TRY(read_top_level_box_header(streamer));
    }

    // ...followed by a single Movie Fragment Box (moof)...

    auto movie_fragment = TRY(Reader::parse_movie_fragment_box(streamer, header, m_track_fragment_contexts));
    auto movie_fragment_end = header.end_position().value();

    //   - The Movie Fragment Box does not contain at least one Track Fragment Box (traf).
    if (movie_fragment.track_fragment_count == 0)
        return Media::DecoderError::corrupted("Movie Fragment box contains no Track Fragment boxes"sv);

    //   - The Movie Fragment Box does not use movie-fragment relative addressing.
    if (!movie_fragment.uses_movie_fragment_relative_addressing)
        return Media::DecoderError::corrupted("Movie Fragment box does not use movie-fragment relative addressing"sv);

    //   - At least one Track Fragment Box does not contain a Track Fragment Decode Time Box (tfdt)
    if (!movie_fragment.all_track_fragments_have_decode_times)
        return Media::DecoderError::corrupted("A Track Fragment box contains no Track Fragment Decode Time box"sv);

    // ...followed by one or more Media Data Boxes (mdat).
    for (auto const& run : movie_fragment.track_runs) {
        if (run.sample_count == 0)
            continue;
        if (run.data_position < movie_fragment_end)
            return Media::DecoderError::corrupted("A sample is stored before the end of the Movie Fragment box"sv);
        // AD-HOC: A media segment's Media Data boxes must contain all of its samples, so a run whose samples
        //         occupy no bytes could declare any number of them without the appended data growing to match.
        if (run.total_data_size == 0)
            return Media::DecoderError::corrupted("A Track Run box declares samples that contain no data"sv);
    }

    // AD-HOC: Coded frame processing is allowed to run as complete coded frames arrive rather than once per media
    //         segment, so the runs are ordered by position to turn the separate indices of the Track Fragment Run
    //         boxes into the single forward sequence that their data arrives in. A run's samples are contiguous, so
    //         ordering the runs orders their samples too.
    quick_sort(movie_fragment.track_runs, [](auto const& a, auto const& b) {
        if (a.data_position != b.data_position)
            return a.data_position < b.data_position;
        return a.total_data_size < b.total_data_size;
    });

    Optional<size_t> previous_run_end;
    for (auto const& run : movie_fragment.track_runs) {
        if (run.sample_count == 0)
            continue;
        if (previous_run_end.has_value() && run.data_position < previous_run_end.value())
            return Media::DecoderError::corrupted("Sample data ranges overlap"sv);
        previous_run_end = run.data_end();
    }

    return MediaSegment {
        .samples = FragmentSampleIterator { move(movie_fragment) },
        .next_media_data_header_position = movie_fragment_end,
        .media_data_start = movie_fragment_end,
        .media_data_end = movie_fragment_end,
        .has_media_data_box = false,
        .position = movie_fragment_end,
    };
}

Media::DecoderErrorOr<ParseMediaSegmentResult> ISOBMFFByteStreamParser::parse_media_segment(Media::MediaStreamCursor& cursor)
{
    Streamer streamer { cursor };

    if (!m_current_media_segment.has_value()) {
        auto segment_position = cursor.position();
        auto segment_or_error = parse_movie_fragment(streamer);
        if (segment_or_error.is_error()) {
            if (segment_or_error.error().category() != Media::DecoderErrorCategory::EndOfStream)
                return segment_or_error.release_error();

            // NB: The Movie Fragment box is incomplete, so leave it in the input buffer to be parsed again
            //     once the rest of it has been appended.
            TRY(cursor.seek_to_position(segment_position));
            return ParseMediaSegmentResult {};
        }
        m_current_media_segment = segment_or_error.release_value();
    }

    auto& segment = m_current_media_segment.value();
    ParseMediaSegmentResult result;

    // AD-HOC: Moving through the segment by relative distances keeps its positions from ever having to be mapped onto
    //         the input buffer's, which is rebased every time the bytes behind the cursor are released.
    auto skip_to = [&](size_t position) -> Media::DecoderErrorOr<void> {
        if (position < segment.position)
            return Media::DecoderError::corrupted("Sample data positions are not ordered"sv);
        TRY(streamer.skip(position - segment.position));
        segment.position = position;
        return {};
    };

    auto rewind_to = [&](size_t cursor_position, size_t segment_position) -> Media::DecoderErrorOr<void> {
        TRY(cursor.seek_to_position(cursor_position));
        segment.position = segment_position;
        return {};
    };

    auto read_buffered_samples = [&]() -> Media::DecoderErrorOr<void> {
        while (segment.samples.has_next()) {
            auto cursor_position = cursor.position();
            auto segment_position = segment.position;

            auto read_sample = [&]() -> Media::DecoderErrorOr<void> {
                auto sample = segment.samples.peek();

                while (!segment.has_media_data_box || sample.data_position + sample.data_size > segment.media_data_end) {
                    TRY(skip_to(segment.next_media_data_header_position));
                    auto position_before_header = streamer.position();
                    auto media_data_header = TRY(read_top_level_box_header(streamer));
                    segment.position += streamer.position() - position_before_header;

                    if (media_data_header.type != MEDIA_DATA_BOX)
                        return Media::DecoderError::format(Media::DecoderErrorCategory::Corrupted, "Found a {} box where a Media Data box was expected", media_data_header.type);
                    segment.media_data_start = segment.position;
                    auto media_data_size = media_data_header.content_size.value();
                    if (Checked<size_t>::addition_would_overflow(segment.media_data_start, media_data_size))
                        return Media::DecoderError::corrupted("Media Data box size is too large"sv);
                    segment.media_data_end = segment.media_data_start + media_data_size;
                    segment.next_media_data_header_position = segment.media_data_end;
                    segment.has_media_data_box = true;
                }

                //   - The Media Data Boxes do not contain all the samples referenced by the Track Fragment
                //     Run Boxes (trun) of the Movie Fragment Box.
                if (sample.data_position < segment.media_data_start)
                    return Media::DecoderError::corrupted("A sample is not contained by any Media Data box"sv);

                TRY(skip_to(sample.data_position));
                auto data = TRY(streamer.read_bytes(sample.data_size));
                segment.position += sample.data_size;

                auto* track_entry = m_track_entries.get(sample.track_id).value();
                auto const& sample_entry = track_entry->sample_entry_for_description_index(sample.sample_description_index).value();

                Optional<FixedArray<u8>> new_codec_configuration;
                auto& active_description = m_active_sample_descriptions.ensure(sample.track_id);
                auto current_sample_description = ActiveSampleDescription { track_entry, sample.sample_description_index };
                if (active_description != current_sample_description)
                    new_codec_configuration = MUST(sample_entry.codec_initialization_data.clone());
                active_description = move(current_sample_description);

                result.coded_frames.append({
                    .track_number = sample.track_id,
                    .coded_frame = Media::CodedFrame(
                        sample_entry.codec_id,
                        sample.presentation_timestamp,
                        sample.decode_timestamp,
                        sample.duration,
                        sample.is_sync_sample ? Media::FrameFlags::Keyframe : Media::FrameFlags::None,
                        move(data),
                        move(new_codec_configuration)),
                });
                return {};
            };

            auto read_result = read_sample();
            if (read_result.is_error()) {
                if (read_result.error().category() != Media::DecoderErrorCategory::EndOfStream)
                    return read_result.release_error();
                return rewind_to(cursor_position, segment_position);
            }
            segment.samples.advance();
        }
        return {};
    };

    TRY(read_buffered_samples());

    if (segment.samples.has_next())
        return result;

    if (!segment.has_media_data_box) {
        auto cursor_position = cursor.position();
        auto segment_position = segment.position;
        auto read_media_data_header = [&]() -> Media::DecoderErrorOr<void> {
            auto position_before_header = streamer.position();
            auto media_data_header = TRY(read_top_level_box_header(streamer));
            segment.position += streamer.position() - position_before_header;
            if (media_data_header.type != MEDIA_DATA_BOX)
                return Media::DecoderError::format(Media::DecoderErrorCategory::Corrupted, "Found a {} box where a Media Data box was expected", media_data_header.type);
            segment.media_data_start = segment.position;
            auto media_data_size = media_data_header.content_size.value();
            if (Checked<size_t>::addition_would_overflow(segment.media_data_start, media_data_size))
                return Media::DecoderError::corrupted("Media Data box size is too large"sv);
            segment.media_data_end = segment.media_data_start + media_data_size;
            segment.next_media_data_header_position = segment.media_data_end;
            segment.has_media_data_box = true;
            return {};
        };
        auto read_result = read_media_data_header();
        if (read_result.is_error()) {
            if (read_result.error().category() != Media::DecoderErrorCategory::EndOfStream)
                return read_result.release_error();
            TRY(rewind_to(cursor_position, segment_position));
            return result;
        }
    }

    // NB: Whatever follows the samples in the last Media Data box is skipped, so the segment is only finished once
    //     that has been appended too.
    auto cursor_position = cursor.position();
    auto segment_position = segment.position;
    auto skip_result = skip_to(segment.media_data_end);
    if (skip_result.is_error()) {
        if (skip_result.error().category() != Media::DecoderErrorCategory::EndOfStream)
            return skip_result.release_error();
        TRY(rewind_to(cursor_position, segment_position));
        return result;
    }

    m_current_media_segment.clear();
    result.completed_segment = true;
    return result;
}

void ISOBMFFByteStreamParser::reset_parser_state()
{
    m_current_media_segment.clear();
}

}
