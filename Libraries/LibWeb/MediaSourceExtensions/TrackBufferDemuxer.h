/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <AK/Vector.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/TimeRanges.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>

namespace Web::MediaSourceExtensions {

// TrackBufferDemuxer stores coded frames for a single track and implements the Demuxer
// interface so that it can be used as a media source for PlaybackManager's data providers.
// It is shared between TrackBuffer (which writes frames) and PlaybackManager (which reads them).
class TrackBufferDemuxer final : public Media::Demuxer {
public:
    TrackBufferDemuxer(Media::Track const&, Media::CodecID, ByteBuffer codec_initialization_data);
    virtual ~TrackBufferDemuxer() override;

    Media::Track const& track() const { return m_track; }

    Media::TimeRanges track_buffer_ranges() const;

    void add_coded_frame(Media::CodedFrame);
    void remove_coded_frames_and_dependants_in_range(AK::Duration start, AK::Duration end);

    void set_reached_end_of_stream();
    void clear_reached_end_of_stream();

    virtual Media::DecoderErrorOr<void> create_context_for_track(Media::Track const&) override;
    virtual Media::DecoderErrorOr<Vector<Media::Track>> get_tracks_for_type(Media::TrackType) override;
    virtual Media::DecoderErrorOr<Optional<Media::Track>> get_preferred_track_for_type(Media::TrackType) override;
    virtual Media::DecoderErrorOr<Media::CodedFrame> get_next_sample_for_track(Media::Track const&) override;
    virtual Media::DecoderErrorOr<Media::CodecID> get_codec_id_for_track(Media::Track const&) override;
    virtual Media::DecoderErrorOr<ReadonlyBytes> get_codec_initialization_data_for_track(Media::Track const&) override;
    virtual Media::DecoderErrorOr<Media::DemuxerSeekResult> seek_to_most_recent_keyframe(Media::Track const&, AK::Duration, Media::DemuxerSeekOptions) override;
    virtual Media::DecoderErrorOr<AK::Duration> duration_of_track(Media::Track const&) override;
    virtual Media::DecoderErrorOr<AK::Duration> total_duration() override;

    virtual Media::TimeRanges buffered_time_ranges() const override;

    virtual void set_blocking_reads_aborted_for_track(Media::Track const&) override;
    virtual void reset_blocking_reads_aborted_for_track(Media::Track const&) override;
    virtual bool is_read_blocked_for_track(Media::Track const&) override;

private:
    AK::Duration maximum_time_range_gap() const;
    bool next_frame_is_in_gap_while_locked() const;

    Media::Track m_track;
    Media::CodecID m_codec_id;
    ByteBuffer m_codec_initialization_data;

    mutable Threading::Mutex m_mutex;
    Threading::ConditionVariable m_data_changed { m_mutex };

    Vector<Media::CodedFrame> m_coded_frames;
    size_t m_read_position { 0 };
    bool m_reached_end_of_stream { false };

    Optional<AK::Duration> m_last_returned_timestamp;

    Media::TimeRanges m_track_buffer_ranges;
    AK::Duration m_last_frame_duration;
    Atomic<bool> m_aborted { false };
};

}
