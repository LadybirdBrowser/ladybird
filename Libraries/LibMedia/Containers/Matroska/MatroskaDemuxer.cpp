/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Debug.h>
#include <AK/Utf16String.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/Containers/Matroska/Utilities.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/MediaStream.h>
#include <LibMedia/SeekMode.h>

#include "MatroskaDemuxer.h"

namespace Media::Matroska {

bool MatroskaDemuxer::supports_container_mime_type(ContainerMimeType mime_type)
{
    if (mime_type.media_type == ContainerMediaType::Application)
        return false;
    return mime_type.container_id == ContainerID::Matroska || mime_type.container_id == ContainerID::WebM;
}

bool MatroskaDemuxer::supports_codec_in_container(ContainerID container_id, CodecID codec_id)
{
    return Matroska::supports_codec_in_container(container_id, codec_id);
}

bool MatroskaDemuxer::should_attempt(NonnullRefPtr<MediaStream> const& stream)
{
    return Reader::is_matroska_or_webm(stream->create_cursor());
}

DecoderErrorOr<NonnullRefPtr<Demuxer>> MatroskaDemuxer::from_stream(NonnullRefPtr<MediaStream> const& stream)
{
    auto cursor = stream->create_cursor();
    auto demuxer = make_ref_counted<MatroskaDemuxer>(stream, TRY(Reader::from_stream(cursor)));
    demuxer->start_buffered_scan_thread(TRY(Reader::from_stream(stream->create_cursor())));
    return demuxer;
}

MatroskaDemuxer::MatroskaDemuxer(NonnullRefPtr<MediaStream> const& stream, Reader&& reader)
    : m_stream(stream)
    , m_reader(move(reader))
{
}

MatroskaDemuxer::~MatroskaDemuxer()
{
    if (m_buffered_scan_thread != nullptr)
        m_buffered_scan_thread->shutdown();
}

void MatroskaDemuxer::start_buffered_scan_thread(Reader&& scan_reader)
{
    auto scan_cursor = m_stream->create_cursor();
    scan_cursor->set_is_blocking(false);

    Vector<Track> tracks;
    tracks.extend(MUST(get_tracks_for_type(TrackType::Video)));
    tracks.extend(MUST(get_tracks_for_type(TrackType::Audio)));

    DemuxerScanState initial_state;
    initial_state.duration = m_reader.duration().value_or(AK::Duration::zero());
    m_buffered_scan_thread = DemuxerScanThread<BufferedScanPayload>::start(m_stream, move(initial_state),
        BufferedScanPayload { move(scan_reader), move(scan_cursor), move(tracks) },
        [](MediaStream& stream, BufferedScanPayload& payload) {
            auto byte_ranges = stream.available_byte_ranges();
            HashMap<u64, BufferedRangesScan> scans_by_track_number;
            if (!byte_ranges.is_empty())
                scans_by_track_number = payload.reader.buffered_time_ranges_by_track_number(payload.scan_cursor, byte_ranges);
            return DemuxerScanState::create_from_track_scans(payload.tracks, move(scans_by_track_number), payload.reader.duration().value_or(AK::Duration::zero()), stream.closing_bytes_are_available());
        });
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

DecoderErrorOr<void> MatroskaDemuxer::create_context_for_track(Track const& track)
{
    auto iterator = TRY(m_reader.create_sample_iterator(m_stream->create_cursor(), track.identifier()));
    Sync::MutexLocker locker(m_track_statuses_mutex);
    VERIFY(m_track_statuses.set(track, TrackStatus(move(iterator))) == HashSetResult::InsertedNewEntry);
    return {};
}

DecoderErrorOr<Vector<Track>> MatroskaDemuxer::get_tracks_for_type(TrackType type)
{
    auto matroska_track_type = matroska_track_type_from_track_type(type);
    Vector<Track> tracks;
    bool is_first = true;
    TRY(m_reader.for_each_track_of_type(matroska_track_type, [&](TrackEntry const& track_entry) -> DecoderErrorOr<IterationDecision> {
        VERIFY(track_entry.track_type() == matroska_track_type);
        DECODER_TRY_ALLOC(tracks.try_append(track_from_track_entry(track_entry, is_first)));
        is_first = false;
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
        result = track_from_track_entry(track_entry, true);
        return IterationDecision::Break;
    }));
    return result;
}

MatroskaDemuxer::TrackStatus& MatroskaDemuxer::get_track_status(Track const& track)
{
    Sync::MutexLocker locker(m_track_statuses_mutex);
    auto track_status = m_track_statuses.get(track);
    VERIFY(track_status.has_value());
    return track_status.release_value();
}

AK::Duration MatroskaDemuxer::select_fast_seek_target_for_track(Track const& track, AK::Duration target, SeekMode mode)
{
    auto cue_points = m_reader.cue_points_for_track(track.identifier());
    if (!cue_points.has_value() || cue_points->is_empty())
        return target;
    auto const& points = cue_points.value();
    auto at_or_before = Reader::find_cue_point_index_at_or_before(points, m_reader.duration(), target);

    auto const& cue_at_or_before = points[at_or_before];
    if (mode == SeekMode::FastBefore)
        return cue_at_or_before.timestamp;

    VERIFY(mode == SeekMode::FastAfter);
    auto after_index = at_or_before + 1;
    if (after_index >= points.size())
        return target;
    return points[after_index].timestamp;
}

DecoderErrorOr<DemuxerSeekResult> MatroskaDemuxer::seek_to_most_recent_keyframe(Track const& track, AK::Duration timestamp, DemuxerSeekOptions options)
{
    auto& track_status = get_track_status(track);
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
    track_status.frames = {};
    track_status.frame_index = 0;
    if (has_flag(options, DemuxerSeekOptions::NeedCodecConfiguration))
        track_status.needs_codec_configuration = true;
    return DemuxerSeekResult::MovedPosition;
}

DecoderErrorOr<CodedFrame> MatroskaDemuxer::get_next_sample_for_track(Track const& track)
{
    auto& status = get_track_status(track);

    if (!status.block.has_value() || (!status.frames.is_empty() && status.frame_index >= status.frames.size())) {
        status.block = TRY(status.iterator.next_block());
        status.frames.clear();
        status.frame_index = 0;
    }
    if (status.frames.is_empty()) {
        status.frames = TRY(status.iterator.get_frames(status.block.value()));
        VERIFY(!status.frames.is_empty());
    }

    VERIFY(status.block.has_value());

    auto track_entry = TRY(m_reader.track_for_track_number(track.identifier()));
    auto codec_id = codec_id_from_matroska_track_entry(track_entry);
    Optional<FixedArray<u8>> codec_configuration;
    if (status.needs_codec_configuration) {
        status.needs_codec_configuration = false;
        codec_configuration = DECODER_TRY_ALLOC(FixedArray<u8>::create(track_entry->codec_private_data()));
    }

    auto timestamp = status.block->timestamp().value();
    auto duration = status.block->duration().value_or(AK::Duration::zero());
    auto flags = status.block->only_keyframes() ? FrameFlags::Keyframe : FrameFlags::None;

    return CodedFrame(codec_id, timestamp, timestamp, duration, flags,
        move(status.frames[status.frame_index++]), move(codec_configuration));
}

DecoderErrorOr<AK::Duration> MatroskaDemuxer::total_duration()
{
    auto duration = m_reader.duration();
    return duration.value_or(AK::Duration::zero());
}

DemuxerScanState const& MatroskaDemuxer::scan_state() const
{
    return m_buffered_scan_thread->main_thread_state();
}

void MatroskaDemuxer::set_scan_state_change_handler(Function<void()> handler)
{
    m_buffered_scan_thread->set_change_handler(move(handler));
}

DecoderErrorOr<AK::Duration> MatroskaDemuxer::duration_of_track(Track const&)
{
    return total_duration();
}

void MatroskaDemuxer::set_blocking_reads_aborted_for_track(Track const& track)
{
    auto& status = get_track_status(track);
    status.iterator.cursor().abort();
}

void MatroskaDemuxer::reset_blocking_reads_aborted_for_track(Track const& track)
{
    auto& status = get_track_status(track);
    status.iterator.cursor().reset_abort();
}

void MatroskaDemuxer::set_read_blocked_change_handler_for_track(Track const& track, ReadBlockedChangeHandler handler)
{
    auto& status = get_track_status(track);
    status.iterator.cursor().set_blocked_change_handler(move(handler));
}

}
