/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Endian.h>
#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibMedia/MediaStream.h>
#include <LibWeb/MediaSourceExtensions/ISOBaseMediaByteStreamParser.h>

namespace Web::MediaSourceExtensions {

static constexpr u32 fourcc(char a, char b, char c, char d)
{
    return (static_cast<u32>(a) << 24) | (static_cast<u32>(b) << 16) | (static_cast<u32>(c) << 8) | static_cast<u32>(d);
}

static constexpr auto BOX_FTYP = fourcc('f', 't', 'y', 'p');
static constexpr auto BOX_MOOV = fourcc('m', 'o', 'o', 'v');
static constexpr auto BOX_STYP = fourcc('s', 't', 'y', 'p');
static constexpr auto BOX_SIDX = fourcc('s', 'i', 'd', 'x');
static constexpr auto BOX_MOOF = fourcc('m', 'o', 'o', 'f');
static constexpr auto BOX_MDAT = fourcc('m', 'd', 'a', 't');
static constexpr auto BOX_FREE = fourcc('f', 'r', 'e', 'e');
static constexpr auto BOX_SKIP = fourcc('s', 'k', 'i', 'p');
static constexpr auto BOX_EMSG = fourcc('e', 'm', 's', 'g');
static constexpr auto BOX_PRFT = fourcc('p', 'r', 'f', 't');
static constexpr auto BOX_PDIN = fourcc('p', 'd', 'i', 'n');
static constexpr auto BOX_UUID = fourcc('u', 'u', 'i', 'd');

struct BoxHeader {
    u32 type { 0 };
    size_t start { 0 };
    size_t end { 0 };
};

static Media::DecoderError unexpected_end_of_stream()
{
    return Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "Incomplete ISO BMFF box"sv);
}

static Media::DecoderErrorOr<BoxHeader> read_box_header(Media::MediaStreamCursor& cursor)
{
    auto const start = cursor.position();
    if (cursor.size() - start < 8)
        return unexpected_end_of_stream();

    auto size = static_cast<u64>(TRY(cursor.read_value<u32>(AK::Endianness::Big)));
    auto type = TRY(cursor.read_value<u32>(AK::Endianness::Big));
    auto header_size = 8u;

    if (size == 1) {
        if (cursor.size() - cursor.position() < 8)
            return unexpected_end_of_stream();
        size = TRY(cursor.read_value<u64>(AK::Endianness::Big));
        header_size = 16;
    } else if (size == 0) {
        return Media::DecoderError::corrupted("Indeterminate ISO BMFF box size"sv);
    }

    if (size < header_size)
        return Media::DecoderError::corrupted("Invalid ISO BMFF box size"sv);

    if constexpr (sizeof(size_t) < sizeof(u64)) {
        if (size > NumericLimits<size_t>::max())
            return Media::DecoderError::corrupted("ISO BMFF box is too large"sv);
    }

    if (size > cursor.size() - start)
        return unexpected_end_of_stream();

    return BoxHeader { type, start, start + static_cast<size_t>(size) };
}

static Media::DecoderErrorOr<ByteBuffer> copy_cursor_range(Media::MediaStreamCursor& cursor, size_t start, size_t end)
{
    VERIFY(start <= end);
    auto bytes = DECODER_TRY_ALLOC(ByteBuffer::create_uninitialized(end - start));
    TRY(cursor.seek_to_position(start));
    TRY(cursor.read_until_filled(bytes.bytes()));
    TRY(cursor.seek_to_position(end));
    return bytes;
}

static bool is_ignored_box(u32 type)
{
    return type == BOX_FREE || type == BOX_SKIP || type == BOX_EMSG || type == BOX_PRFT || type == BOX_PDIN || type == BOX_UUID;
}

ISOBaseMediaByteStreamParser::ISOBaseMediaByteStreamParser() = default;
ISOBaseMediaByteStreamParser::~ISOBaseMediaByteStreamParser() = default;

Media::DecoderErrorOr<void> ISOBaseMediaByteStreamParser::skip_ignored_bytes(Media::MediaStreamCursor& cursor)
{
    while (cursor.position() < cursor.size()) {
        auto const position = cursor.position();
        auto header_or_error = read_box_header(cursor);
        if (header_or_error.is_error()) {
            TRY(cursor.seek_to_position(position));
            return header_or_error.release_error();
        }

        auto const& header = header_or_error.value();
        if (!is_ignored_box(header.type)) {
            TRY(cursor.seek_to_position(position));
            return {};
        }
        TRY(cursor.seek_to_position(header.end));
    }
    return {};
}

Media::DecoderErrorOr<SegmentType> ISOBaseMediaByteStreamParser::sniff_segment_type(Media::MediaStreamCursor& cursor)
{
    auto const position = cursor.position();
    ArmedScopeGuard restore_position = [&] { MUST(cursor.seek_to_position(position)); };

    auto header_or_error = read_box_header(cursor);
    if (header_or_error.is_error()) {
        if (header_or_error.error().category() == Media::DecoderErrorCategory::EndOfStream)
            return SegmentType::Incomplete;
        return header_or_error.release_error();
    }

    auto type = header_or_error.value().type;
    if (type == BOX_FTYP || type == BOX_MOOV)
        return SegmentType::InitializationSegment;
    if (type == BOX_STYP || type == BOX_SIDX || type == BOX_MOOF)
        return SegmentType::MediaSegment;
    return SegmentType::Unknown;
}

Media::DecoderErrorOr<void> ISOBaseMediaByteStreamParser::parse_initialization_segment(Media::MediaStreamCursor& cursor)
{
    auto const segment_start = cursor.position();
    ArmedScopeGuard restore_position = [&] { MUST(cursor.seek_to_position(segment_start)); };
    bool found_moov = false;

    while (!found_moov) {
        auto header = TRY(read_box_header(cursor));

        if (header.type == BOX_MOOV)
            found_moov = true;
        else if (header.type != BOX_FTYP && !is_ignored_box(header.type))
            return Media::DecoderError::corrupted("Unexpected box before ISO BMFF movie box"sv);

        TRY(cursor.seek_to_position(header.end));
    }

    auto const segment_end = cursor.position();
    auto initialization_segment = TRY(copy_cursor_range(cursor, segment_start, segment_end));
    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(initialization_segment);
    auto demuxer = TRY(Media::FFmpeg::FFmpegDemuxer::from_stream(stream, Media::FFmpeg::FFmpegDemuxer::BufferedScan::Disabled));
    auto duration = TRY(demuxer->total_duration());

    auto video_tracks = TRY(demuxer->get_tracks_for_type(Media::TrackType::Video));
    auto audio_tracks = TRY(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    auto text_tracks = TRY(demuxer->get_tracks_for_type(Media::TrackType::Subtitles));
    HashMap<u64, TrackDescription> track_descriptions;

    auto record_track_descriptions = [&](Vector<Media::Track> const& tracks) -> Media::DecoderErrorOr<void> {
        for (auto const& track : tracks) {
            auto codec_id = TRY(demuxer->get_codec_id_for_track(track));
            auto codec_initialization_data = DECODER_TRY_ALLOC(ByteBuffer::copy(TRY(demuxer->get_codec_initialization_data_for_track(track))));
            DECODER_TRY_ALLOC(track_descriptions.try_set(track.identifier(), { codec_id, move(codec_initialization_data) }));
        }
        return {};
    };

    TRY(record_track_descriptions(video_tracks));
    TRY(record_track_descriptions(audio_tracks));
    TRY(record_track_descriptions(text_tracks));

    m_initialization_segment = move(initialization_segment);
    m_duration = duration > AK::Duration::zero() ? Optional<AK::Duration> { duration } : OptionalNone {};
    m_track_descriptions = move(track_descriptions);
    m_video_tracks = move(video_tracks);
    m_audio_tracks = move(audio_tracks);
    m_text_tracks = move(text_tracks);

    TRY(cursor.seek_to_position(segment_end));
    restore_position.disarm();
    return {};
}

Media::DecoderErrorOr<ParseMediaSegmentResult> ISOBaseMediaByteStreamParser::parse_media_segment(Media::MediaStreamCursor& cursor)
{
    auto const segment_start = cursor.position();
    ArmedScopeGuard restore_position = [&] { MUST(cursor.seek_to_position(segment_start)); };

    bool found_moof = false;
    bool found_mdat_after_moof = false;

    while (true) {
        if (cursor.position() == cursor.size()) {
            if (found_mdat_after_moof) {
                // FIXME: Inspect the fragment's sample tables to determine whether more mdat boxes belong to this
                //        segment when they arrive in a later append.
                break;
            }
            return ParseMediaSegmentResult {};
        }

        auto const box_start = cursor.position();
        auto header_or_error = read_box_header(cursor);
        if (header_or_error.is_error()) {
            if (header_or_error.error().category() == Media::DecoderErrorCategory::EndOfStream)
                return ParseMediaSegmentResult {};
            return header_or_error.release_error();
        }

        auto const& header = header_or_error.value();
        if (found_mdat_after_moof && header.type != BOX_MDAT) {
            TRY(cursor.seek_to_position(box_start));
            break;
        }

        if (header.type == BOX_MOOF)
            found_moof = true;
        else if (header.type == BOX_MDAT && found_moof)
            found_mdat_after_moof = true;
        else if (header.type == BOX_FTYP || header.type == BOX_MOOV)
            return Media::DecoderError::corrupted("Initialization box found inside an ISO BMFF media segment"sv);

        TRY(cursor.seek_to_position(header.end));
    }

    auto const segment_end = cursor.position();
    auto media_segment = TRY(copy_cursor_range(cursor, segment_start, segment_end));
    auto combined_data = DECODER_TRY_ALLOC(ByteBuffer::create_uninitialized(m_initialization_segment.size() + media_segment.size()));
    m_initialization_segment.bytes().copy_to(combined_data.bytes());
    media_segment.bytes().copy_to(combined_data.bytes().slice(m_initialization_segment.size()));

    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(combined_data);
    auto demuxer = TRY(Media::FFmpeg::FFmpegDemuxer::from_stream(stream, Media::FFmpeg::FFmpegDemuxer::BufferedScan::Disabled));
    ParseMediaSegmentResult result { .completed_segment = true, .coded_frames = {} };

    auto demux_tracks = [&](Vector<Media::Track> const& tracks) -> Media::DecoderErrorOr<void> {
        for (auto const& track : tracks) {
            auto segment_tracks = TRY(demuxer->get_tracks_for_type(track.type()));
            if (!segment_tracks.contains_slow(track))
                return Media::DecoderError::corrupted("ISO BMFF media segment is missing an initialization track"sv);

            TRY(demuxer->create_context_for_track(track));
            Vector<Media::CodedFrame> track_frames;
            while (true) {
                auto frame_or_error = demuxer->get_next_sample_for_track(track);
                if (frame_or_error.is_error()) {
                    if (frame_or_error.error().category() == Media::DecoderErrorCategory::EndOfStream)
                        break;
                    return frame_or_error.release_error();
                }
                DECODER_TRY_ALLOC(track_frames.try_append(frame_or_error.release_value()));
            }

            // FFmpeg does not reliably expose sample durations for fragmented MP4. Derive them from adjacent
            // presentation timestamps so that tiny rounding overlaps do not cause coded-frame replacement to discard
            // H.264 frames, and so that zero-duration AAC packets still contribute buffered ranges.
            // FIXME: Read sample durations from trex/tfhd/trun boxes so single-frame fragments also receive their
            //        declared duration.
            Vector<size_t> presentation_order;
            DECODER_TRY_ALLOC(presentation_order.try_ensure_capacity(track_frames.size()));

            for (size_t i = 0; i < track_frames.size(); i++)
                presentation_order.unchecked_append(i);

            quick_sort(presentation_order, [&](size_t left, size_t right) {
                return track_frames[left].timestamp() < track_frames[right].timestamp();
            });

            for (size_t i = 0; i + 1 < presentation_order.size(); i++) {
                auto& frame = track_frames[presentation_order[i]];
                auto duration = track_frames[presentation_order[i + 1]].timestamp() - frame.timestamp();
                if (duration > AK::Duration::zero())
                    frame.set_duration(duration);
            }

            if (presentation_order.size() > 1) {
                if (auto& last_frame = track_frames[presentation_order.last()]; last_frame.duration().is_zero())
                    last_frame.set_duration(track_frames[presentation_order[presentation_order.size() - 2]].duration());
            }

            // libavformat's fragmented MP4 demuxer may report decode timestamps from the composition-time table,
            // producing near-duplicate timestamps for reordered H.264 frames. Preserve its packet order and assign
            // a continuous decode timeline so MSE does not mistake those timestamps for discontinuities.
            // FIXME: Read decode timestamps from fragment timing instead. Presentation-time deltas can drift for
            //        variable-frame-rate and edit-listed media.
            if (track.type() == Media::TrackType::Video && !track_frames.is_empty()) {
                auto decode_timestamp = track_frames.first().auxiliary_data().get<Media::CodedVideoFrameData>().decode_timestamp().value_or(track_frames.first().timestamp());

                for (auto& frame : track_frames) {
                    frame.auxiliary_data().get<Media::CodedVideoFrameData>().set_decode_timestamp(decode_timestamp);
                    decode_timestamp += frame.duration();
                }
            }

            for (auto& frame : track_frames)
                DECODER_TRY_ALLOC(result.coded_frames.try_append({ track.identifier(), move(frame) }));
        }
        return {};
    };

    TRY(demux_tracks(m_video_tracks));
    TRY(demux_tracks(m_audio_tracks));

    TRY(cursor.seek_to_position(segment_end));
    restore_position.disarm();
    return result;
}

Media::CodecID ISOBaseMediaByteStreamParser::codec_id_for_track(u64 track_number) const
{
    if (auto description = m_track_descriptions.get(track_number); description.has_value())
        return description->codec_id;
    return Media::CodecID::Unknown;
}

ReadonlyBytes ISOBaseMediaByteStreamParser::codec_initialization_data_for_track(u64 track_number) const
{
    if (auto description = m_track_descriptions.get(track_number); description.has_value())
        return description->codec_initialization_data.bytes();
    return {};
}

}
