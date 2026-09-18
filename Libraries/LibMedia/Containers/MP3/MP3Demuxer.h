/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <LibMedia/ContainerID.h>
#include <LibMedia/Containers/MP3/Reader.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/DemuxerScanThread.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>

namespace Media::MP3 {

class MEDIA_API MP3Demuxer final : public Demuxer {
public:
    static DecoderErrorOr<NonnullRefPtr<Demuxer>> from_stream(NonnullRefPtr<MediaStream> const&);
    static bool supports_container_mime_type(ContainerMimeType);
    static bool supports_codec_in_container(ContainerID, CodecID);

    MP3Demuxer(NonnullRefPtr<MediaStream> const&, Reader, Track);
    ~MP3Demuxer();

    virtual DecoderErrorOr<void> create_context_for_track(Track const&) override;

    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType) override;
    virtual DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType) override;

    virtual DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track const&) override;

    virtual AK::Duration select_fast_seek_target_for_track(Track const&, AK::Duration target, SeekMode) override;
    virtual DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const&, AK::Duration timestamp, DemuxerSeekOptions) override;

    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const&) override;
    virtual DecoderErrorOr<AK::Duration> total_duration() override;

    virtual DemuxerScanState const& scan_state() const LIFETIME_BOUND override;
    virtual void set_scan_state_change_handler(Function<void()>) override;

    virtual void set_blocking_reads_aborted_for_track(Track const&) override;
    virtual void reset_blocking_reads_aborted_for_track(Track const&) override;
    virtual void set_read_blocked_change_handler_for_track(Track const&, ReadBlockedChangeHandler) override;

private:
    struct BufferedScanPayload {
        NonnullOwnPtr<FrameScanTimeline> timeline;
        NonnullRefPtr<MediaStreamCursor> scan_cursor;
        Vector<Track> tracks;
    };

    struct TrackStatus {
        FrameIterator iterator;
        AK::Duration next_timestamp { AK::Duration::zero() };
        bool needs_codec_configuration { true };
    };

    void start_buffered_scan_thread();
    TrackStatus& track_status();

    NonnullRefPtr<MediaStream> m_stream;
    Reader m_reader;
    Track m_track;

    RefPtr<DemuxerScanThread<BufferedScanPayload>> m_buffered_scan_thread;

    NonnullRefPtr<MediaStreamCursor> m_seek_scanning_cursor;
    NonnullRefPtr<MediaStreamCursor> m_seek_cursor;

    Mutex m_track_status_mutex;
    Optional<TrackStatus> m_track_status;
};

}
