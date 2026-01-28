/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibThreading/Mutex.h>

#include "Reader.h"

namespace Media::Matroska {

class MEDIA_API MatroskaDemuxer final : public Demuxer {
public:
    static DecoderErrorOr<NonnullRefPtr<MatroskaDemuxer>> from_stream(NonnullRefPtr<MediaStream> const&);

    MatroskaDemuxer(NonnullRefPtr<MediaStream> const& stream, Reader&& reader);
    ~MatroskaDemuxer();

    virtual DecoderErrorOr<void> create_context_for_track(Track const&) override;

    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType) override;
    virtual DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType) override;

    virtual DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const&, AK::Duration timestamp, DemuxerSeekOptions) override;

    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const&) override;
    virtual DecoderErrorOr<AK::Duration> total_duration() override;

    virtual DecoderErrorOr<CodecID> get_codec_id_for_track(Track const&) override;

    virtual DecoderErrorOr<ReadonlyBytes> get_codec_initialization_data_for_track(Track const&) override;

    virtual DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track const&) override;

    virtual void set_blocking_reads_aborted_for_track(Track const&) override;
    virtual void reset_blocking_reads_aborted_for_track(Track const&) override;
    virtual bool is_read_blocked_for_track(Track const&) override;

private:
    struct TrackStatus {
        SampleIterator iterator;
        Optional<Block> block;
        Vector<ByteBuffer, 4> frames;
        size_t frame_index { 0 };

        TrackStatus(SampleIterator&& iterator)
            : iterator(iterator)
        {
        }
    };

    TrackStatus& get_track_status(Track const&);

    NonnullRefPtr<MediaStream> m_stream;
    Reader m_reader;

    mutable Threading::Mutex m_track_statuses_mutex;
    HashMap<Track, TrackStatus> m_track_statuses;
};

}
