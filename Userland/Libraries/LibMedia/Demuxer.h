/*
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibCore/EventReceiver.h>

#include "CodecID.h"
#include "DecoderError.h"
#include "Sample.h"
#include "Track.h"

namespace Media {

class Demuxer {
public:
    virtual ~Demuxer() = default;

    virtual DecoderErrorOr<Vector<Track>> get_tracks_for_type(TrackType type) = 0;

    virtual DecoderErrorOr<Sample> get_next_sample_for_track(Track track) = 0;

    virtual DecoderErrorOr<CodecID> get_codec_id_for_track(Track track) = 0;

    virtual DecoderErrorOr<ReadonlyBytes> get_codec_initialization_data_for_track(Track track) = 0;

    // Returns the timestamp of the keyframe that was seeked to.
    // The value is `Optional` to allow the demuxer to decide not to seek so that it can keep its position
    // in the case that the timestamp is closer to the current time than the nearest keyframe.
    virtual DecoderErrorOr<Optional<Duration>> seek_to_most_recent_keyframe(Track track, Duration timestamp, Optional<Duration> earliest_available_sample = OptionalNone()) = 0;

    virtual DecoderErrorOr<Duration> duration() = 0;
};

}
