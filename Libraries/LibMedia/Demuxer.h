/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/EnumBits.h>
#include <AK/NonnullOwnPtr.h>
#include <LibCore/EventReceiver.h>
#include <LibMedia/IncrementallyPopulatedStream.h>

#include "CodecID.h"
#include "CodedFrame.h"
#include "DecoderError.h"
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

    // Returns the timestamp of the keyframe that was seeked to.
    // The value is `Optional` to allow the demuxer to decide not to seek so that it can keep its position
    // in the case that the timestamp is closer to the current time than the nearest keyframe.
    virtual DecoderErrorOr<DemuxerSeekResult> seek_to_most_recent_keyframe(Track const&, AK::Duration timestamp, DemuxerSeekOptions = DemuxerSeekOptions::None) = 0;

    virtual DecoderErrorOr<AK::Duration> duration_of_track(Track const&) = 0;
    virtual DecoderErrorOr<AK::Duration> total_duration() = 0;

    virtual void set_blocking_reads_aborted_for_track(Track const&) = 0;
    virtual void reset_blocking_reads_aborted_for_track(Track const&) = 0;
    virtual bool is_read_blocked_for_track(Track const&) = 0;
};

}
