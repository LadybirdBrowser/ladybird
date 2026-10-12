/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ISOBMFFDemuxer.h"
#include "Reader.h"

namespace Media::ISOBMFF {

bool ISOBMFFDemuxer::supports_container_mime_type(ContainerMimeType type)
{
    return type.container_id == ContainerID::ISOBMFF && type.media_type != ContainerMediaType::Application;
}

bool ISOBMFFDemuxer::supports_codec_in_container(ContainerID container, CodecID codec)
{
    return container == ContainerID::ISOBMFF && Reader::supports_codec(codec);
}

DecoderErrorOr<NonnullRefPtr<Demuxer>> ISOBMFFDemuxer::from_stream(NonnullRefPtr<MediaStream> const& source)
{
    Streamer stream(source->create_cursor());
    auto first = Reader::read_box_header(stream);
    if (first.is_error() || !first_is_one_of(first.value().type, FILE_TYPE_BOX, MOVIE_BOX, FREE_SPACE_BOX, MEDIA_DATA_BOX, SKIP_BOX, WIDE_ATOM))
        return DecoderError::with_description(DecoderErrorCategory::UnrecognizedFormat, "Stream is not ISO BMFF"sv);
    auto header = first.release_value();
    for (;;) {
        if (header.type == MOVIE_BOX) {
            auto movie = TRY(Reader::parse_movie_box(stream, header));
            auto demuxer = make_ref_counted<ISOBMFFDemuxer>(source, move(movie), header.end_position().value_or(stream.position()));
            demuxer->start_scan();
            return demuxer;
        }
        if (!header.end_position().has_value())
            return DecoderError::corrupted("ISO BMFF stream has no movie box"sv);
        TRY(stream.seek_to_position(header.end_position().value()));
        header = TRY(Reader::read_box_header(stream));
    }
}

ISOBMFFDemuxer::ISOBMFFDemuxer(NonnullRefPtr<MediaStream> stream, Movie movie, size_t movie_end)
    : m_stream(move(stream))
    , m_movie(move(movie))
    , m_movie_end(movie_end)
{
    m_durations->total = m_movie.duration_as_time().value_or({});
    for (auto const& [id, track] : m_movie.tracks) {
        auto duration = AK::Duration::from_time_units(AK::clamp_to<i64>(track->duration.value_or(0)), 1, track->timescale);
        m_durations->tracks.set(id, duration);
        m_durations->total = max(m_durations->total, duration);
    }
}

ISOBMFFDemuxer::~ISOBMFFDemuxer()
{
    if (m_scan) {
        m_scan->payload().cursor->abort();
        m_scan->shutdown();
    }
}

void ISOBMFFDemuxer::start_scan()
{
    auto cursor = m_stream->create_cursor();
    cursor->set_is_blocking(false);
    DemuxerScanState initial;
    initial.duration = m_durations->total;
    m_scan = DemuxerScanThread<ScanPayload>::start(m_stream, move(initial), { m_movie, m_movie_end, move(cursor), m_durations }, [](MediaStream::Availability const& availability, ScanPayload& payload) {
        DemuxerScanState state;
        {
            MutexLocker locker(payload.durations->mutex);
            state.duration = payload.durations->total;
        }
        state.reached_end_of_stream = availability.closing_bytes_are_available();
        auto const& ranges = availability.byte_ranges;
        for (auto const& [id, track] : payload.movie.tracks) {
            if (!first_is_one_of(track->handler_type, HandlerType::Audio, HandlerType::Video))
                continue;
            TimeRanges buffered;
            AK::Duration duration;
            auto iterator = SampleIterator::create(payload.cursor, payload.movie, id, payload.movie_end);
            bool can_decode = false;
            bool last_sample_available = false;
            bool track_reached_end = availability.closing_bytes_are_available();
            if (!iterator.is_error()) {
                for (;;) {
                    auto next = iterator.value().next();
                    if (next.is_error()) {
                        track_reached_end = false;
                        break;
                    }
                    if (!next.value().has_value())
                        break;
                    auto const& sample = next.value().value();
                    auto end = sample.presentation_timestamp + sample.duration;
                    duration = max(duration, end);
                    state.duration = max(state.duration, end);
                    bool available = any_of(ranges, [&](auto const& range) {
                        return sample.data_position >= range.start && sample.data_position <= range.end && sample.data_size <= range.end - sample.data_position;
                    });
                    last_sample_available = available;
                    if (!available)
                        can_decode = false;
                    else if (sample.is_sync_sample)
                        can_decode = true;
                    if (available && can_decode)
                        buffered.add_range(max(sample.presentation_timestamp, AK::Duration::zero()), end);
                }
            } else {
                track_reached_end = false;
            }
            track_reached_end &= last_sample_available;
            state.track_buffered_ranges.append({ id, move(buffered), track_reached_end });
            state.reached_end_of_stream &= track_reached_end;
            {
                MutexLocker locker(payload.durations->mutex);
                auto& track_duration = payload.durations->tracks.get(id).value();
                track_duration = max(track_duration, duration);
            }
        }
        {
            MutexLocker locker(payload.durations->mutex);
            payload.durations->total = max(payload.durations->total, state.duration);
        }
        return state;
    });
}

DecoderErrorOr<void> ISOBMFFDemuxer::create_context_for_track(Track const& track)
{
    auto cursor = m_stream->create_cursor();
    auto iterator = TRY(SampleIterator::create(cursor, m_movie, track.identifier(), m_movie_end));
    auto status = make<TrackStatus>(move(cursor), move(iterator));
    MutexLocker locker(m_status_mutex);
    VERIFY(m_statuses.set(track, move(status)) == HashSetResult::InsertedNewEntry);
    return {};
}

ISOBMFFDemuxer::TrackStatus& ISOBMFFDemuxer::status_for_track(Track const& track)
{
    MutexLocker locker(m_status_mutex);
    return **m_statuses.get(track);
}

DecoderErrorOr<Vector<Track>> ISOBMFFDemuxer::get_tracks_for_type(TrackType type)
{
    Vector<Track> tracks;
    for (auto const& entry : m_movie.tracks) {
        if (track_type_from_handler_type(entry.value->handler_type) == type)
            DECODER_TRY_ALLOC(tracks.try_append(track_from_track_entry(*entry.value, tracks.is_empty())));
    }
    return tracks;
}

DecoderErrorOr<Optional<Track>> ISOBMFFDemuxer::get_preferred_track_for_type(TrackType type)
{
    auto tracks = TRY(get_tracks_for_type(type));
    for (auto const& track : tracks) {
        if (m_movie.tracks.get(track.identifier()).value()->is_enabled)
            return Optional<Track> { track };
    }
    if (tracks.is_empty())
        return Optional<Track> {};
    return Optional<Track> { tracks.first() };
}

DecoderErrorOr<CodedFrame> ISOBMFFDemuxer::get_next_sample_for_track(Track const& track)
{
    auto& status = status_for_track(track);
    if (!status.pending_sample.has_value())
        status.pending_sample = TRY(status.iterator.next());
    if (!status.pending_sample.has_value())
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "No more ISO BMFF samples"sv);
    auto const& sample = status.pending_sample.value();
    Streamer stream(status.cursor);
    TRY(stream.seek_to_position(sample.data_position));
    auto data = TRY(stream.read_bytes(sample.data_size));
    auto entry = m_movie.tracks.get(track.identifier()).value()->sample_entry_for_description_index(sample.sample_description_index);
    if (!entry.has_value())
        return DecoderError::corrupted("Invalid sample description index"sv);
    Optional<FixedArray<u8>> configuration;
    if (status.description_index != sample.sample_description_index)
        configuration = DECODER_TRY_ALLOC(entry->codec_initialization_data.clone());
    status.description_index = sample.sample_description_index;
    status.last_timestamp = sample.presentation_timestamp;
    CodedFrame frame(entry->codec_id, sample.presentation_timestamp, sample.decode_timestamp, sample.duration,
        sample.is_sync_sample ? FrameFlags::Keyframe : FrameFlags::None, move(data), move(configuration));
    status.pending_sample.clear();
    return frame;
}

DecoderErrorOr<Optional<ISOBMFFDemuxer::SeekPoint>> ISOBMFFDemuxer::find_seek_point(NonnullRefPtr<MediaStreamCursor> cursor, Track const& track, AK::Duration target, SeekMode mode)
{
    auto iterator = TRY(SampleIterator::create(move(cursor), m_movie, track.identifier(), m_movie_end));
    Optional<SeekPoint> before;
    Optional<SeekPoint> after;
    bool first_sample = true;
    for (;;) {
        auto next = iterator.next();
        if (next.is_error() && next.error().category() == DecoderErrorCategory::NeedsMoreInput && (before.has_value() || after.has_value()))
            break;
        auto sample = TRY(move(next));
        if (!sample.has_value())
            break;
        if (first_sample || sample->is_sync_sample) {
            auto pts = sample->presentation_timestamp;
            if (pts <= target && (!before.has_value() || pts > before->sample.presentation_timestamp))
                before = SeekPoint { iterator, sample.value() };
            if (pts > target && (!after.has_value() || pts < after->sample.presentation_timestamp))
                after = SeekPoint { iterator, sample.value() };
        }
        first_sample = false;
        if (mode == SeekMode::FastAfter) {
            if (after.has_value())
                break;
        } else if (sample->presentation_timestamp >= target || sample->presentation_timestamp + sample->duration > target) {
            // Reordering may leave a closer keyframe ahead in decode order; decoding forward is sufficient.
            break;
        }
    }
    if (mode == SeekMode::FastAfter)
        return after;
    return before.has_value() ? move(before) : move(after);
}

AK::Duration ISOBMFFDemuxer::select_fast_seek_target_for_track(Track const& track, AK::Duration target, SeekMode mode)
{
    auto cursor = m_stream->create_cursor();
    cursor->set_is_blocking(false);
    auto point = find_seek_point(move(cursor), track, target, mode);
    if (point.is_error() || !point.value().has_value())
        return target;
    return point.value()->sample.presentation_timestamp;
}

DecoderErrorOr<DemuxerSeekResult> ISOBMFFDemuxer::seek_to_most_recent_keyframe(Track const& track, AK::Duration target, DemuxerSeekOptions options)
{
    auto& status = status_for_track(track);
    if (has_flag(options, DemuxerSeekOptions::NeedCodecConfiguration))
        status.description_index.clear();
    auto point = TRY(find_seek_point(status.cursor, track, target, SeekMode::FastBefore));
    if (!point.has_value())
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "Track has no samples"sv);
    if (!has_flag(options, DemuxerSeekOptions::Force) && status.last_timestamp.has_value()
        && point->sample.presentation_timestamp <= status.last_timestamp.value() && status.last_timestamp.value() <= target)
        return DemuxerSeekResult::KeptCurrentPosition;
    status.iterator = move(point->iterator);
    status.pending_sample = point->sample;
    status.last_timestamp.clear();
    return DemuxerSeekResult::MovedPosition;
}

DecoderErrorOr<AK::Duration> ISOBMFFDemuxer::duration_of_track(Track const& track)
{
    MutexLocker locker(m_durations->mutex);
    auto duration = m_durations->tracks.get(track.identifier());
    if (!duration.has_value())
        return DecoderError::corrupted("Unknown track"sv);
    return duration.value();
}

DecoderErrorOr<AK::Duration> ISOBMFFDemuxer::total_duration()
{
    MutexLocker locker(m_durations->mutex);
    return m_durations->total;
}

DemuxerScanState const& ISOBMFFDemuxer::scan_state() const
{
    return m_scan->main_thread_state();
}

void ISOBMFFDemuxer::set_scan_state_change_handler(ScanStateChangeHandler handler)
{
    m_scan->set_change_handler([handler = move(handler)] {
        if (handler)
            handler({});
    });
}

void ISOBMFFDemuxer::set_blocking_reads_aborted_for_track(Track const& track)
{
    status_for_track(track).cursor->abort();
}

void ISOBMFFDemuxer::reset_blocking_reads_aborted_for_track(Track const& track)
{
    status_for_track(track).cursor->reset_abort();
}

void ISOBMFFDemuxer::set_read_blocked_change_handler_for_track(Track const& track, ReadBlockedChangeHandler handler)
{
    status_for_track(track).cursor->set_blocked_change_handler(move(handler));
}

}
