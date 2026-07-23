/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/EnumBits.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <LibCore/EventReceiver.h>
#include <LibMedia/IncrementallyPopulatedStream.h>

#include "CodecID.h"
#include "CodedFrame.h"
#include "DecoderError.h"
#include "SeekMode.h"
#include "TimeRanges.h"
#include "Track.h"

namespace Media {

enum class DemuxerSeekOptions : u8 {
    None = 0,
    Force = 1 << 0,
};

AK_ENUM_BITWISE_OPERATORS(DemuxerSeekOptions);

enum class DemuxerSeekResult : u8 {
    MovedPosition,
    KeptCurrentPosition,
};

struct DemuxerTrackScanState {
    Track track;
    TimeRanges buffered_ranges;
    bool reached_end_of_stream { false };

    bool operator==(DemuxerTrackScanState const&) const = default;
};

struct DemuxerScanState {
    Vector<DemuxerTrackScanState> tracks;
    AK::Duration duration;

    static DemuxerScanState create_from_track_scans(Vector<Track> const& tracks, HashMap<u64, BufferedRangesScan> scans_by_track_identifier, AK::Duration minimum_duration, bool closing_bytes_are_available)
    {
        DemuxerScanState state;
        state.duration = minimum_duration;
        for (auto const& track : tracks) {
            auto scan = scans_by_track_identifier.take(track.identifier());
            if (!scan.has_value())
                continue;
            state.duration = max(state.duration, scan->time_ranges.highest_end_time());
            state.tracks.empend(track, move(scan->time_ranges), scan->last_byte_range_has_samples && closing_bytes_are_available);
        }
        return state;
    }

    DemuxerTrackScanState const* state_for_track(Track const& track) const LIFETIME_BOUND
    {
        for (auto const& track_state : tracks) {
            if (track_state.track == track)
                return &track_state;
        }
        return nullptr;
    }

    bool operator==(DemuxerScanState const&) const = default;
};

class Demuxer : public AtomicRefCounted<Demuxer> {
public:
    virtual ~Demuxer() = default;

    virtual DecoderErrorOr<void> create_context_for_track(Track const&) = 0;

    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType) = 0;
    // Returns the container's preferred track for a given track type. This must return a value if any track of the
    // given type is present.
    virtual DecoderErrorOr<Optional<Track>> get_preferred_track_for_type(TrackType) = 0;

    virtual DecoderErrorOr<CodedFrame> get_next_sample_for_track(Track const&) = 0;

    virtual DecoderErrorOr<CodecID> get_codec_id_for_track(Track const&) = 0;

    virtual DecoderErrorOr<ReadonlyBytes> get_codec_initialization_data_for_track(Track const&) = 0;

    virtual AK::Duration select_fast_seek_target_for_track(Track const&, AK::Duration target, SeekMode) = 0;
    virtual DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const&, AK::Duration timestamp, DemuxerSeekOptions = DemuxerSeekOptions::None) = 0;

    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const&) = 0;
    virtual DecoderErrorOr<AK::Duration> total_duration() = 0;

    // Returns the timeline offset if the media resource provides one.
    virtual Optional<AK::UnixDateTime> start_time_realtime() const { return {}; }

    virtual DemuxerScanState const& scan_state() const LIFETIME_BOUND = 0;
    virtual void set_scan_state_change_handler(Function<void()>) = 0;

    virtual void set_blocking_reads_aborted_for_track(Track const&) = 0;
    virtual void reset_blocking_reads_aborted_for_track(Track const&) = 0;
    virtual void set_read_blocked_change_handler_for_track(Track const&, ReadBlockedChangeHandler) = 0;
};

}
