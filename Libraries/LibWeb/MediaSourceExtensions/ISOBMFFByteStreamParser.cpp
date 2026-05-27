/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/DecoderError.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibWeb/MediaSourceExtensions/ISOBMFFByteStreamParser.h>

namespace Web::MediaSourceExtensions {

using Media::DecoderError;
using Media::DecoderErrorCategory;

ISOBMFFByteStreamParser::ISOBMFFByteStreamParser() = default;
ISOBMFFByteStreamParser::~ISOBMFFByteStreamParser() = default;

struct BoxHeader {
    u64 size { 0 };
    size_t header_size { 0 };
    StringView type;
};

static Media::DecoderErrorOr<ByteBuffer> read_remaining_bytes(Media::MediaStreamCursor& cursor)
{
    auto start = cursor.position();
    auto remaining_size = cursor.size() - start;
    auto bytes = DECODER_TRY_ALLOC(ByteBuffer::create_uninitialized(remaining_size));
    if (remaining_size > 0)
        TRY(cursor.read_into(bytes.bytes()));
    TRY(cursor.seek(start, SeekMode::SetPosition));
    return bytes;
}

static u32 read_big_endian_u32(ReadonlyBytes bytes)
{
    return (static_cast<u32>(bytes[0]) << 24)
        | (static_cast<u32>(bytes[1]) << 16)
        | (static_cast<u32>(bytes[2]) << 8)
        | static_cast<u32>(bytes[3]);
}

static u64 read_big_endian_u64(ReadonlyBytes bytes)
{
    return (static_cast<u64>(read_big_endian_u32(bytes.slice(0, 4))) << 32)
        | read_big_endian_u32(bytes.slice(4, 4));
}

static bool box_type_is(StringView type, StringView expected)
{
    return type == expected;
}

static Media::DecoderErrorOr<BoxHeader> read_box_header(ReadonlyBytes bytes, size_t offset)
{
    if (bytes.size() - offset < 8)
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "Incomplete ISO BMFF box header"sv);

    auto size = static_cast<u64>(read_big_endian_u32(bytes.slice(offset, 4)));
    auto type = StringView { reinterpret_cast<char const*>(bytes.offset_pointer(offset + 4)), 4 };
    size_t header_size = 8;

    if (size == 1) {
        if (bytes.size() - offset < 16)
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "Incomplete large ISO BMFF box header"sv);
        size = read_big_endian_u64(bytes.slice(offset + 8, 8));
        header_size = 16;
    } else if (size == 0) {
        size = bytes.size() - offset;
    }

    if (size < header_size)
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::Corrupted, "Invalid ISO BMFF box size"sv);
    if (size > bytes.size() - offset)
        return Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "Incomplete ISO BMFF box"sv);

    return BoxHeader { size, header_size, type };
}

static Media::DecoderErrorOr<size_t> find_initialization_segment_end(ReadonlyBytes bytes)
{
    size_t offset = 0;
    bool saw_ftyp = false;

    while (offset < bytes.size()) {
        auto box = TRY(read_box_header(bytes, offset));
        if (box_type_is(box.type, "ftyp"sv))
            saw_ftyp = true;
        if (box_type_is(box.type, "moov"sv)) {
            if (!saw_ftyp)
                return Media::DecoderError::with_description(Media::DecoderErrorCategory::Corrupted, "ISO BMFF initialization segment is missing ftyp"sv);
            return offset + box.size;
        }
        offset += box.size;
    }

    return Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "Incomplete ISO BMFF initialization segment"sv);
}

static Media::DecoderErrorOr<size_t> find_media_segment_end(ReadonlyBytes bytes)
{
    size_t offset = 0;
    bool saw_moof = false;
    bool saw_mdat = false;

    while (offset < bytes.size()) {
        auto header = read_box_header(bytes, offset);
        if (header.is_error()) {
            if (saw_moof && saw_mdat && header.error().category() == Media::DecoderErrorCategory::EndOfStream)
                return offset;
            return header.release_error();
        }

        auto box = header.release_value();
        if (box_type_is(box.type, "moof"sv) && saw_moof && saw_mdat)
            return offset;
        if (box_type_is(box.type, "ftyp"sv) || box_type_is(box.type, "moov"sv)) {
            if (saw_moof && saw_mdat)
                return offset;
            return Media::DecoderError::with_description(Media::DecoderErrorCategory::Corrupted, "Unexpected initialization box in ISO BMFF media segment"sv);
        }
        if (box_type_is(box.type, "moof"sv))
            saw_moof = true;
        if (box_type_is(box.type, "mdat"sv))
            saw_mdat = true;

        offset += box.size;
    }

    if (saw_moof && saw_mdat)
        return offset;
    return Media::DecoderError::with_description(Media::DecoderErrorCategory::EndOfStream, "Incomplete ISO BMFF media segment"sv);
}

static Media::DecoderErrorOr<NonnullRefPtr<Media::FFmpeg::FFmpegDemuxer>> create_demuxer_from_bytes(ReadonlyBytes bytes)
{
    auto stream = Media::IncrementallyPopulatedStream::create_from_data(bytes);
    return Media::FFmpeg::FFmpegDemuxer::from_stream(stream);
}

Media::DecoderErrorOr<void> ISOBMFFByteStreamParser::skip_ignored_bytes(Media::MediaStreamCursor&)
{
    return {};
}

Media::DecoderErrorOr<SegmentType> ISOBMFFByteStreamParser::sniff_segment_type(Media::MediaStreamCursor& cursor)
{
    auto bytes = TRY(read_remaining_bytes(cursor));
    if (bytes.size() < 8)
        return SegmentType::Incomplete;

    auto box = TRY(read_box_header(bytes.bytes(), 0));
    if (box_type_is(box.type, "ftyp"sv))
        return SegmentType::InitializationSegment;
    if (box_type_is(box.type, "styp"sv) || box_type_is(box.type, "sidx"sv) || box_type_is(box.type, "moof"sv))
        return SegmentType::MediaSegment;
    return SegmentType::Unknown;
}

Media::DecoderErrorOr<void> ISOBMFFByteStreamParser::parse_initialization_segment(Media::MediaStreamCursor& cursor)
{
    auto bytes = TRY(read_remaining_bytes(cursor));
    auto segment_end = TRY(find_initialization_segment_end(bytes.bytes()));
    m_initialization_segment = DECODER_TRY_ALLOC(ByteBuffer::copy(bytes.bytes().slice(0, segment_end)));

    auto demuxer = TRY(create_demuxer_from_bytes(m_initialization_segment.bytes()));
    m_duration = TRY(demuxer->total_duration());
    if (m_duration.has_value() && m_duration.value().is_zero())
        m_duration.clear();

    m_audio_tracks = TRY(demuxer->get_tracks_for_type(Media::TrackType::Audio));
    m_video_tracks = TRY(demuxer->get_tracks_for_type(Media::TrackType::Video));
    m_text_tracks.clear();

    auto remember_track_data = [&](Media::Track const& track) -> Media::DecoderErrorOr<void> {
        auto codec_id = TRY(demuxer->get_codec_id_for_track(track));
        auto codec_initialization_data = TRY(demuxer->get_codec_initialization_data_for_track(track));
        auto copied_codec_initialization_data = DECODER_TRY_ALLOC(ByteBuffer::copy(codec_initialization_data));
        DECODER_TRY_ALLOC(m_track_data.try_set(track.identifier(), TrackData { codec_id, move(copied_codec_initialization_data) }));
        return {};
    };

    for (auto const& track : m_audio_tracks)
        TRY(remember_track_data(track));
    for (auto const& track : m_video_tracks)
        TRY(remember_track_data(track));

    TRY(cursor.seek(segment_end, SeekMode::SetPosition));
    return {};
}

Media::DecoderErrorOr<ParseMediaSegmentResult> ISOBMFFByteStreamParser::parse_media_segment(Media::MediaStreamCursor& cursor)
{
    auto bytes = TRY(read_remaining_bytes(cursor));
    auto segment_end_or_error = find_media_segment_end(bytes.bytes());
    if (segment_end_or_error.is_error()) {
        if (segment_end_or_error.error().category() == Media::DecoderErrorCategory::EndOfStream)
            return ParseMediaSegmentResult { .completed_segment = false, .coded_frames = {} };
        return segment_end_or_error.release_error();
    }
    auto segment_end = segment_end_or_error.release_value();

    auto demuxer_input = DECODER_TRY_ALLOC(ByteBuffer::create_uninitialized(m_initialization_segment.size() + segment_end));
    m_initialization_segment.bytes().copy_to(demuxer_input.bytes());
    bytes.bytes().slice(0, segment_end).copy_to(demuxer_input.bytes().slice(m_initialization_segment.size()));

    auto demuxer = TRY(create_demuxer_from_bytes(demuxer_input.bytes()));
    ParseMediaSegmentResult result { .completed_segment = true, .coded_frames = {} };

    auto collect_frames = [&](Vector<Media::Track> const& tracks) -> Media::DecoderErrorOr<void> {
        for (auto const& track : tracks) {
            Vector<DemuxedCodedFrame> track_frames;
            size_t frame_count = 0;
            size_t keyframe_count = 0;
            bool decode_timestamps_were_monotonic = true;
            Optional<AK::Duration> previous_decode_timestamp;
            Optional<AK::Duration> first_presentation_timestamp;
            Optional<AK::Duration> first_decode_timestamp;
            Optional<AK::Duration> first_keyframe_presentation_timestamp;
            Optional<AK::Duration> first_keyframe_decode_timestamp;
            TRY(demuxer->create_context_for_track(track));
            while (true) {
                auto frame = demuxer->get_next_sample_for_track(track);
                if (frame.is_error()) {
                    if (frame.error().category() == Media::DecoderErrorCategory::EndOfStream)
                        break;
                    return frame.release_error();
                }
                auto coded_frame = frame.release_value();
                ++frame_count;
                if (coded_frame.is_keyframe())
                    ++keyframe_count;

                if (track.type() == Media::TrackType::Video) {
                    if (!first_presentation_timestamp.has_value()) {
                        first_presentation_timestamp = coded_frame.timestamp();
                        first_decode_timestamp = coded_frame.decode_timestamp();
                    }
                    if (coded_frame.is_keyframe() && !first_keyframe_presentation_timestamp.has_value()) {
                        first_keyframe_presentation_timestamp = coded_frame.timestamp();
                        first_keyframe_decode_timestamp = coded_frame.decode_timestamp();
                    }

                    if (previous_decode_timestamp.has_value() && coded_frame.decode_timestamp() < previous_decode_timestamp.value())
                        decode_timestamps_were_monotonic = false;
                    previous_decode_timestamp = coded_frame.decode_timestamp();

                    auto insert_index = track_frames.size();
                    while (insert_index > 0 && coded_frame.decode_timestamp() < track_frames[insert_index - 1].coded_frame.decode_timestamp())
                        --insert_index;
                    DECODER_TRY_ALLOC(track_frames.try_insert(insert_index, { track.identifier(), move(coded_frame) }));
                } else {
                    DECODER_TRY_ALLOC(track_frames.try_append({ track.identifier(), move(coded_frame) }));
                }
            }
            if (track.type() == Media::TrackType::Video)
                dbgln("MSE/MP4: demuxed video segment track={} frames={} keyframes={} sorted_by_dts={} first_pts={}ms first_dts={}ms first_key_pts={}ms first_key_dts={}ms",
                    track.identifier(),
                    frame_count,
                    keyframe_count,
                    !decode_timestamps_were_monotonic,
                    first_presentation_timestamp.value_or(AK::Duration::zero()).to_milliseconds(),
                    first_decode_timestamp.value_or(AK::Duration::zero()).to_milliseconds(),
                    first_keyframe_presentation_timestamp.value_or(AK::Duration::zero()).to_milliseconds(),
                    first_keyframe_decode_timestamp.value_or(AK::Duration::zero()).to_milliseconds());
            for (auto& coded_frame : track_frames)
                DECODER_TRY_ALLOC(result.coded_frames.try_append(move(coded_frame)));
        }
        return {};
    };

    TRY(collect_frames(TRY(demuxer->get_tracks_for_type(Media::TrackType::Video))));
    TRY(collect_frames(TRY(demuxer->get_tracks_for_type(Media::TrackType::Audio))));

    TRY(cursor.seek(segment_end, SeekMode::SetPosition));
    return result;
}

Media::CodecID ISOBMFFByteStreamParser::codec_id_for_track(u64 track_number) const
{
    auto track_data = m_track_data.get(track_number);
    VERIFY(track_data.has_value());
    return track_data->codec_id;
}

ReadonlyBytes ISOBMFFByteStreamParser::codec_initialization_data_for_track(u64 track_number) const
{
    auto track_data = m_track_data.get(track_number);
    VERIFY(track_data.has_value());
    return track_data->codec_initialization_data.bytes();
}

}
