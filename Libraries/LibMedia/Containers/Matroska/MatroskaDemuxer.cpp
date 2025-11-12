/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Debug.h>
#include <AK/Utf16String.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/DecoderError.h>

#include "MatroskaDemuxer.h"

namespace Media::Matroska {

DecoderErrorOr<NonnullRefPtr<MatroskaDemuxer>> MatroskaDemuxer::from_file(StringView filename)
{
    return make_ref_counted<MatroskaDemuxer>(TRY(Reader::from_file(filename)));
}

DecoderErrorOr<NonnullRefPtr<MatroskaDemuxer>> MatroskaDemuxer::from_mapped_file(NonnullOwnPtr<Core::MappedFile> mapped_file)
{
    return make_ref_counted<MatroskaDemuxer>(TRY(Reader::from_mapped_file(move(mapped_file))));
}

DecoderErrorOr<NonnullRefPtr<MatroskaDemuxer>> MatroskaDemuxer::from_data(ReadonlyBytes data)
{
    return make_ref_counted<MatroskaDemuxer>(TRY(Reader::from_data(data)));
}

static TrackEntry::TrackType matroska_track_type_from_track_type(TrackType type)
{
    switch (type) {
    case TrackType::Video:
        return TrackEntry::TrackType::Video;
    case TrackType::Audio:
        return TrackEntry::TrackType::Audio;
    case TrackType::Subtitles:
        return TrackEntry::TrackType::Subtitle;
    case TrackType::Unknown:
        return TrackEntry::TrackType::Invalid;
    }
    VERIFY_NOT_REACHED();
}

static TrackType track_type_from_matroska_track_type(TrackEntry::TrackType type)
{
    switch (type) {
    case TrackEntry::TrackType::Video:
        return TrackType::Video;
    case TrackEntry::TrackType::Audio:
        return TrackType::Audio;
    case TrackEntry::TrackType::Subtitle:
        return TrackType::Subtitles;
    case TrackEntry::TrackType::Invalid:
        return TrackType::Unknown;
    case TrackEntry::TrackType::Complex:
    case TrackEntry::TrackType::Logo:
    case TrackEntry::TrackType::Buttons:
    case TrackEntry::TrackType::Control:
    case TrackEntry::TrackType::Metadata:
        break;
    }
    VERIFY_NOT_REACHED();
}

static Track track_from_track_entry(TrackEntry const& track_entry)
{
    auto name = Utf16String::from_utf8(track_entry.name());
    auto language = [&] {
        // LanguageBCP47 - The language of the track, in the BCP47 form; see basics on language codes. If this Element is used,
        // then any Language Elements used in the same TrackEntry MUST be ignored.
        if (track_entry.language_bcp_47().has_value())
            return Utf16String::from_utf8(track_entry.language_bcp_47().value());
        return Utf16String::from_utf8(track_entry.language());
    }();
    Track track(track_type_from_matroska_track_type(track_entry.track_type()), track_entry.track_number(), name, language);

    if (track.type() == TrackType::Video) {
        auto video_track = track_entry.video_track();
        if (video_track.has_value()) {
            track.set_video_data({
                .pixel_width = video_track->pixel_width,
                .pixel_height = video_track->pixel_height,
                .cicp = video_track->color_format.to_cicp(),
            });
        }
    }

    return track;
}

DecoderErrorOr<Vector<Track>> MatroskaDemuxer::get_tracks_for_type(TrackType type)
{
    auto matroska_track_type = matroska_track_type_from_track_type(type);
    Vector<Track> tracks;
    TRY(m_reader.for_each_track_of_type(matroska_track_type, [&](TrackEntry const& track_entry) -> DecoderErrorOr<IterationDecision> {
        VERIFY(track_entry.track_type() == matroska_track_type);
        DECODER_TRY_ALLOC(tracks.try_append(track_from_track_entry(track_entry)));
        return IterationDecision::Continue;
    }));
    return tracks;
}

DecoderErrorOr<Optional<Track>> MatroskaDemuxer::get_preferred_track_for_type(TrackType type)
{
    auto matroska_track_type = matroska_track_type_from_track_type(type);
    Optional<Track> result;
    TRY(m_reader.for_each_track_of_type(matroska_track_type, [&](TrackEntry const& track_entry) -> DecoderErrorOr<IterationDecision> {
        VERIFY(track_entry.track_type() == matroska_track_type);
        result = track_from_track_entry(track_entry);
        return IterationDecision::Break;
    }));
    return result;
}

DecoderErrorOr<MatroskaDemuxer::TrackStatus*> MatroskaDemuxer::get_track_status(Track const& track)
{
    if (!m_track_statuses.contains(track)) {
        auto iterator = TRY(m_reader.create_sample_iterator(track.identifier()));
        DECODER_TRY_ALLOC(m_track_statuses.try_set(track, TrackStatus(move(iterator))));
    }

    return &m_track_statuses.get(track).release_value();
}

static CodecID get_codec_id_for_string(String const& codec_id)
{
    dbgln_if(MATROSKA_DEBUG, "Codec ID: {}", codec_id);
    if (codec_id == "V_VP8")
        return CodecID::VP8;
    if (codec_id == "V_VP9")
        return CodecID::VP9;
    if (codec_id == "V_MPEG1")
        return CodecID::MPEG1;
    if (codec_id == "V_MPEG2")
        return CodecID::H262;
    if (codec_id == "V_MPEG4/ISO/AVC")
        return CodecID::H264;
    if (codec_id == "V_MPEGH/ISO/HEVC")
        return CodecID::H265;
    if (codec_id == "A_MPEG/L3")
        return CodecID::MP3;
    if (codec_id == "A_AAC" || codec_id == "A_AAC/MPEG4/LC"
        || codec_id == "A_AAC/MPEG4/LC/SBR" || codec_id == "A_AAC/MPEG4/LTP"
        || codec_id == "A_AAC/MPEG4/MAIN" || codec_id == "A_AAC/MPEG4/SSR")
        return CodecID::AAC;
    if (codec_id == "V_AV1")
        return CodecID::AV1;
    if (codec_id == "V_THEORA")
        return CodecID::Theora;
    if (codec_id == "A_VORBIS")
        return CodecID::Vorbis;
    if (codec_id == "A_OPUS")
        return CodecID::Opus;
    if (codec_id == "A_FLAC")
        return CodecID::FLAC;
    return CodecID::Unknown;
}

DecoderErrorOr<CodecID> MatroskaDemuxer::get_codec_id_for_track(Track const& track)
{
    auto codec_id = TRY(m_reader.track_for_track_number(track.identifier()))->codec_id();
    return get_codec_id_for_string(codec_id);
}

DecoderErrorOr<ReadonlyBytes> MatroskaDemuxer::get_codec_initialization_data_for_track(Track const& track)
{
    return TRY(m_reader.track_for_track_number(track.identifier()))->codec_private_data();
}

DecoderErrorOr<DemuxerSeekResult> MatroskaDemuxer::seek_to_most_recent_keyframe(Track const& track, AK::Duration timestamp, DemuxerSeekOptions options)
{
    // Removing the track status will cause us to start from the beginning.
    if (timestamp.is_zero()) {
        m_track_statuses.remove(track);
        return DemuxerSeekResult::MovedPosition;
    }

    auto& track_status = *TRY(get_track_status(track));
    auto seeked_iterator = TRY(m_reader.seek_to_random_access_point(track_status.iterator, timestamp));

    auto last_sample = track_status.iterator.last_timestamp();
    if (has_flag(options, DemuxerSeekOptions::Force))
        last_sample = {};
    if (last_sample.has_value() && seeked_iterator.last_timestamp().has_value()) {
        bool skip_seek = seeked_iterator.last_timestamp().value() <= last_sample.value() && last_sample.value() <= timestamp;
        dbgln_if(MATROSKA_DEBUG, "The last available sample at {}ms is {}closer to target timestamp {}ms than the keyframe at {}ms, {}", last_sample->to_milliseconds(), skip_seek ? ""sv : "not "sv, timestamp.to_milliseconds(), seeked_iterator.last_timestamp()->to_milliseconds(), skip_seek ? "skipping seek"sv : "seeking"sv);
        if (skip_seek)
            return DemuxerSeekResult::KeptCurrentPosition;
    }

    track_status.iterator = move(seeked_iterator);
    track_status.block = {};
    track_status.frame_index = 0;
    return DemuxerSeekResult::MovedPosition;
}

DecoderErrorOr<CodedFrame> MatroskaDemuxer::get_next_sample_for_track(Track const& track)
{
    // FIXME: This makes a copy of the sample, which shouldn't be necessary.
    //        Matroska should make a RefPtr<ByteBuffer>, probably.
    auto& status = *TRY(get_track_status(track));

    if (!status.block.has_value() || status.frame_index >= status.block->frame_count()) {
        status.block = TRY(status.iterator.next_block());
        status.frame_index = 0;
    }

    auto flags = status.block->only_keyframes() ? FrameFlags::Keyframe : FrameFlags::None;
    auto aux_data = [&] -> CodedFrame::AuxiliaryData {
        if (track.type() == TrackType::Video) {
            return CodedVideoFrameData();
        }
        if (track.type() == TrackType::Audio) {
            return CodedAudioFrameData();
        }
        VERIFY_NOT_REACHED();
    }();
    auto sample_data = DECODER_TRY_ALLOC(ByteBuffer::copy(status.block->frame(status.frame_index++)));
    return CodedFrame(status.block->timestamp(), flags, move(sample_data), aux_data);
}

DecoderErrorOr<AK::Duration> MatroskaDemuxer::total_duration()
{
    auto duration = TRY(m_reader.segment_information()).duration();
    return duration.value_or(AK::Duration::zero());
}

DecoderErrorOr<AK::Duration> MatroskaDemuxer::duration_of_track(Track const&)
{
    return total_duration();
}

}
