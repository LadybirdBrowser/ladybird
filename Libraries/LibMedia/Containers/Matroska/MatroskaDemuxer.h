/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/Export.h>

#include "Reader.h"

namespace Media::Matroska {

class MEDIA_API MatroskaDemuxer final : public Demuxer {
public:
    static DecoderErrorOr<NonnullRefPtr<MatroskaDemuxer>> from_data(ReadonlyBytes data);

    MatroskaDemuxer(Reader&& reader)
        : m_reader(move(reader))
    {
    }

    DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType) override;
    DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType) override;

    DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const&, AK::Duration timestamp, DemuxerSeekOptions) override;

    DecoderErrorOr<AK::Duration> duration_of_track(Track const&) override;
    DecoderErrorOr<AK::Duration> total_duration() override;

    DecoderErrorOr<CodecID> get_codec_id_for_track(Track const&) override;

    DecoderErrorOr<ReadonlyBytes> get_codec_initialization_data_for_track(Track const&) override;

    DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track const&) override;

private:
    struct TrackStatus {
        SampleIterator iterator;
        Optional<Block> block;
        size_t frame_index { 0 };

        TrackStatus(SampleIterator&& iterator)
            : iterator(iterator)
        {
        }
    };

    DecoderErrorOr<TrackStatus*> get_track_status(Track const& track);

    Reader m_reader;

    HashMap<Track, TrackStatus> m_track_statuses;
};

}
