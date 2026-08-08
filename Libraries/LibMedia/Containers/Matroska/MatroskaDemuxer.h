/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibMedia/ContainerID.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/DemuxerScanThread.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibSync/Mutex.h>

#include "Reader.h"

namespace Media::Matroska {

class MEDIA_API MatroskaDemuxer final : public Demuxer {
public:
    static bool should_attempt(NonnullRefPtr<MediaStream> const&);
    static DecoderErrorOr<NonnullRefPtr<Demuxer>> from_stream(NonnullRefPtr<MediaStream> const&);
    static bool supports_container_mime_type(ContainerMimeType);
    static bool supports_codec_in_container(ContainerID, CodecID);

    MatroskaDemuxer(NonnullRefPtr<MediaStream> const& stream, Reader&& reader);
    ~MatroskaDemuxer();

    virtual DecoderErrorOr<void> create_context_for_track(Track const&) override;

    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType) override;
    virtual DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType) override;

    virtual AK::Duration select_fast_seek_target_for_track(Track const&, AK::Duration target, SeekMode) override;
    virtual DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const&, AK::Duration timestamp, DemuxerSeekOptions) override;

    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const&) override;
    virtual DecoderErrorOr<AK::Duration> total_duration() override;

    virtual DemuxerScanState const& scan_state() const LIFETIME_BOUND override;
    virtual void set_scan_state_change_handler(Function<void()>) override;

    virtual DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track const&) override;

    virtual void set_blocking_reads_aborted_for_track(Track const&) override;
    virtual void reset_blocking_reads_aborted_for_track(Track const&) override;
    virtual void set_read_blocked_change_handler_for_track(Track const&, ReadBlockedChangeHandler) override;

private:
    struct BufferedScanPayload {
        Reader reader;
        NonnullRefPtr<MediaStreamCursor> scan_cursor;
        Vector<Track> tracks;
    };

    void start_buffered_scan_thread(Reader&& scan_reader);

    struct TrackStatus {
        SampleIterator iterator;
        Optional<Block> block;
        SampleIterator::Frames frames;
        size_t frame_index { 0 };
        bool needs_codec_configuration { true };

        TrackStatus(SampleIterator&& iterator)
            : iterator(iterator)
        {
        }
    };

    TrackStatus& get_track_status(Track const&);

    NonnullRefPtr<MediaStream> m_stream;
    Reader m_reader;
    RefPtr<DemuxerScanThread<BufferedScanPayload>> m_buffered_scan_thread;

    mutable Sync::Mutex m_track_statuses_mutex;
    HashMap<Track, TrackStatus> m_track_statuses;
};

}
