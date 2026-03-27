/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibMedia/Track.h>

namespace Web::MediaSourceExtensions {

class TrackBufferDemuxer;

// https://w3c.github.io/media-source/#track-buffers
// TrackBuffer holds MSE spec state for a single track. Frame storage is managed by
// the associated TrackBufferDemuxer, which is shared with the PlaybackManager.
class TrackBuffer {
public:
    TrackBuffer(NonnullRefPtr<TrackBufferDemuxer>);
    ~TrackBuffer();

    TrackBufferDemuxer& demuxer() { return m_demuxer; }
    TrackBufferDemuxer const& demuxer() const { return m_demuxer; }

    // https://w3c.github.io/media-source/#last-decode-timestamp
    Optional<AK::Duration> last_decode_timestamp() const { return m_last_decode_timestamp; }
    void set_last_decode_timestamp(AK::Duration timestamp) { m_last_decode_timestamp = timestamp; }
    void unset_last_decode_timestamp() { m_last_decode_timestamp = {}; }

    // https://w3c.github.io/media-source/#last-frame-duration
    Optional<AK::Duration> last_frame_duration() const { return m_last_frame_duration; }
    void set_last_frame_duration(AK::Duration duration) { m_last_frame_duration = duration; }
    void unset_last_frame_duration() { m_last_frame_duration = {}; }

    // https://w3c.github.io/media-source/#highest-end-timestamp
    Optional<AK::Duration> highest_end_timestamp() const { return m_highest_end_timestamp; }
    void set_highest_end_timestamp(AK::Duration timestamp) { m_highest_end_timestamp = timestamp; }
    void unset_highest_end_timestamp() { m_highest_end_timestamp = {}; }

    // https://w3c.github.io/media-source/#need-RAP-flag
    bool need_random_access_point_flag() const { return m_need_random_access_point_flag; }
    void set_need_random_access_point_flag(bool flag) { m_need_random_access_point_flag = flag; }

    // https://w3c.github.io/media-source/#track-buffer-ranges
    // FIXME: Return a TimeRanges-like structure
    void track_buffer_ranges() const;

private:
    NonnullRefPtr<TrackBufferDemuxer> m_demuxer;

    // https://w3c.github.io/media-source/#last-decode-timestamp
    Optional<AK::Duration> m_last_decode_timestamp;

    // https://w3c.github.io/media-source/#last-frame-duration
    Optional<AK::Duration> m_last_frame_duration;

    // https://w3c.github.io/media-source/#highest-end-timestamp
    Optional<AK::Duration> m_highest_end_timestamp;

    // https://w3c.github.io/media-source/#need-RAP-flag
    bool m_need_random_access_point_flag { true };
};

}
