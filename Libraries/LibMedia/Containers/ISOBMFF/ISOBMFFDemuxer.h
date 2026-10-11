/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "SampleIterator.h"
#include <AK/Mutex.h>
#include <LibMedia/ContainerID.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/DemuxerScanThread.h>

namespace Media::ISOBMFF {

class MEDIA_API ISOBMFFDemuxer final : public Demuxer {
public:
    static DecoderErrorOr<NonnullRefPtr<Demuxer>> from_stream(NonnullRefPtr<MediaStream> const&);
    static bool supports_container_mime_type(ContainerMimeType);
    static bool supports_codec_in_container(ContainerID, CodecID);
    ISOBMFFDemuxer(NonnullRefPtr<MediaStream>, Movie, size_t movie_end);
    virtual ~ISOBMFFDemuxer() override;

    virtual DecoderErrorOr<void> create_context_for_track(Track const&) override;
    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType) override;
    virtual DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType) override;
    virtual DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track const&) override;
    virtual AK::Duration select_fast_seek_target_for_track(Track const&, AK::Duration, SeekMode) override;
    virtual DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const&, AK::Duration, DemuxerSeekOptions) override;
    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const&) override;
    virtual DecoderErrorOr<AK::Duration> total_duration() override;
    virtual DemuxerScanState const& scan_state() const LIFETIME_BOUND override;
    virtual void set_scan_state_change_handler(ScanStateChangeHandler) override;
    virtual void set_blocking_reads_aborted_for_track(Track const&) override;
    virtual void reset_blocking_reads_aborted_for_track(Track const&) override;
    virtual void set_read_blocked_change_handler_for_track(Track const&, ReadBlockedChangeHandler) override;

private:
    struct TrackStatus {
        AK_MAKE_NONCOPYABLE(TrackStatus);
        AK_MAKE_NONMOVABLE(TrackStatus);
        AK_ALLOC_WITH_KMALLOC;

        TrackStatus(NonnullRefPtr<MediaStreamCursor> cursor, SampleIterator iterator)
            : cursor(move(cursor))
            , iterator(move(iterator))
        {
        }

        NonnullRefPtr<MediaStreamCursor> cursor;
        SampleIterator iterator;
        Optional<Sample> pending_sample;
        Optional<AK::Duration> last_timestamp;
        Optional<u32> description_index;
    };
    struct Durations : public AtomicRefCounted<Durations> {
        Mutex mutex;
        HashMap<u32, AK::Duration> tracks;
        AK::Duration total;
    };
    struct ScanPayload {
        Movie movie;
        size_t movie_end;
        NonnullRefPtr<MediaStreamCursor> cursor;
        NonnullRefPtr<Durations> durations;
    };
    struct SeekPoint {
        SampleIterator iterator;
        Sample sample;
    };
    DecoderErrorOr<Optional<SeekPoint>> find_seek_point(NonnullRefPtr<MediaStreamCursor>, Track const&, AK::Duration, SeekMode);
    TrackStatus& status_for_track(Track const&);
    void start_scan();
    NonnullRefPtr<MediaStream> m_stream;
    Movie m_movie;
    size_t m_movie_end;
    Mutex m_status_mutex;
    HashMap<Track, NonnullOwnPtr<TrackStatus>> m_statuses;
    NonnullRefPtr<Durations> m_durations { make_ref_counted<Durations>() };
    RefPtr<DemuxerScanThread<ScanPayload>> m_scan;
};

}
