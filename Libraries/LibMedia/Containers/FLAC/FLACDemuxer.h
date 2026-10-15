/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Mutex.h>
#include <LibMedia/ContainerID.h>
#include <LibMedia/Containers/CachedByteRanges.h>
#include <LibMedia/Containers/FLAC/Reader.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/DemuxerScanThread.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>

namespace Media::FLAC {

class MEDIA_API FLACDemuxer final : public Demuxer {
public:
    static DecoderErrorOr<NonnullRefPtr<Demuxer>> from_stream(NonnullRefPtr<MediaStream> const&);
    static bool supports_container_mime_type(ContainerMimeType);
    static bool supports_codec_in_container(ContainerID, CodecID);

    FLACDemuxer(NonnullRefPtr<MediaStream> const&, Reader, Track);
    ~FLACDemuxer();

    virtual DecoderErrorOr<void> create_context_for_track(Track const&) override;
    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType) override;
    virtual DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType) override;
    virtual DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track const&) override;
    virtual AK::Duration select_fast_seek_target_for_track(Track const&, AK::Duration target, SeekMode) override;
    virtual DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const&, AK::Duration timestamp, DemuxerSeekOptions) override;
    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const&) override;
    virtual DecoderErrorOr<AK::Duration> total_duration() override;
    virtual DemuxerScanState const& scan_state() const LIFETIME_BOUND override;
    virtual void set_scan_state_change_handler(ScanStateChangeHandler) override;
    virtual void set_blocking_reads_aborted_for_track(Track const&) override;
    virtual void reset_blocking_reads_aborted_for_track(Track const&) override;
    virtual void set_read_blocked_change_handler_for_track(Track const&, ReadBlockedChangeHandler) override;

private:
    struct SeekAnchor {
        size_t byte { 0 };
        u64 sample { 0 };
    };

    // Frame positions the scan has seen, so that a seek into buffered data probes close by and fetches nothing.
    struct ScannedAnchors : public AtomicRefCounted<ScannedAnchors> {
        Mutex mutex;
        Vector<SeekAnchor> anchors;
    };

    struct ScannedByteRange : public CachedByteRange {
        using CachedByteRange::CachedByteRange;

        Optional<SeekAnchor> first_frame;
        Optional<SeekAnchor> end;
    };

    struct BufferedScanPayload {
        FrameIterator scanner;
        CachedByteRanges<ScannedByteRange> scanned_ranges;
        NonnullRefPtr<ScannedAnchors> anchors;
    };

    struct TrackStatus {
        FrameIterator iterator;
        bool needs_codec_configuration { true };
    };

    void start_buffered_scan_thread();
    TrackStatus& track_status();
    DecoderErrorOr<size_t> find_frame_position_for_sample(FrameIterator&, u64 target_sample);

    NonnullRefPtr<MediaStream> m_stream;
    Reader m_reader;
    Track m_track;
    RefPtr<DemuxerScanThread<BufferedScanPayload>> m_buffered_scan_thread;
    NonnullRefPtr<ScannedAnchors> m_scanned_anchors { make_ref_counted<ScannedAnchors>() };
    Mutex m_track_status_mutex;
    Optional<TrackStatus> m_track_status;
};

}
