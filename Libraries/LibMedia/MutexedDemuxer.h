/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibMedia/DecoderError.h>
#include <LibThreading/MutexProtected.h>

#include "Demuxer.h"

namespace Media {

class MutexedDemuxer final : public Demuxer {
public:
    MutexedDemuxer(NonnullRefPtr<Demuxer> demuxer)
        : m_demuxer(move(demuxer))
    {
    }

    virtual ~MutexedDemuxer() override
    {
        m_demuxer.with_locked([](auto& demuxer) {
            auto to_destroy = move(demuxer);
        });
    }

    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType type) override
    {
        return m_demuxer.with_locked([&](auto& demuxer) {
            return demuxer->get_tracks_for_type(type);
        });
    }
    virtual DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType type) override
    {
        return m_demuxer.with_locked([&](auto& demuxer) {
            return demuxer->get_preferred_track_for_type(type);
        });
    }

    virtual DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track track) override
    {
        return m_demuxer.with_locked([&](auto& demuxer) {
            return demuxer->get_next_sample_for_track(track);
        });
    }

    virtual DecoderErrorOr<CodecID> get_codec_id_for_track(Track track) override
    {
        return m_demuxer.with_locked([&](auto& demuxer) {
            return demuxer->get_codec_id_for_track(track);
        });
    }

    virtual DecoderErrorOr<ReadonlyBytes> get_codec_initialization_data_for_track(Track track) override
    {
        return m_demuxer.with_locked([&](auto& demuxer) {
            return demuxer->get_codec_initialization_data_for_track(track);
        });
    }

    virtual DecoderErrorOr<Optional<AK::Duration>> seek_to_most_recent_keyframe(Track track, AK::Duration timestamp, Optional<AK::Duration> earliest_available_sample = {}) override
    {
        return m_demuxer.with_locked([&](auto& demuxer) {
            return demuxer->seek_to_most_recent_keyframe(track, timestamp, earliest_available_sample);
        });
    }

    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const& track) override
    {
        return m_demuxer.with_locked([&](auto& demuxer) {
            return demuxer->duration_of_track(track);
        });
    }
    virtual DecoderErrorOr<AK::Duration> total_duration() override
    {
        return m_demuxer.with_locked([&](auto& demuxer) {
            return demuxer->total_duration();
        });
    }

private:
    Threading::MutexProtected<NonnullRefPtr<Demuxer>> m_demuxer;
};

}
