/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/MediaStream.h>
#include <LibMedia/TimeRanges.h>

namespace Media {

struct SeekSkipped { };

struct SeekedPosition {
    i64 byte_position;
    AK::Duration timestamp;
};

using SeekResult = Variant<Empty, SeekSkipped, SeekedPosition>;

class ContainerNavigator {
public:
    virtual ~ContainerNavigator() = default;

    // Seeks arrive from each track's demuxing thread, and the buffered-ranges scan runs on its
    // own thread; implementations must tolerate all of these running concurrently.
    virtual DecoderErrorOr<SeekResult> seek_to_timestamp(u64 track_identifier, AK::Duration) const
    {
        (void)track_identifier;
        return Empty {};
    }
    virtual HashMap<u64, BufferedRangesScan> buffered_time_ranges_by_track(Vector<MediaStream::ByteRange> const& byte_ranges) const = 0;
};

// Adapts a navigator over a single-track container to the per-track interface. Such containers
// hold their format's only stream, so the track identifier is always stream index zero.
class SingleTrackContainerNavigator : public ContainerNavigator {
public:
    virtual DecoderErrorOr<SeekResult> seek_to_timestamp(u64, AK::Duration timestamp) const final
    {
        return seek_to_timestamp(timestamp);
    }

    virtual HashMap<u64, BufferedRangesScan> buffered_time_ranges_by_track(Vector<MediaStream::ByteRange> const& byte_ranges) const final
    {
        HashMap<u64, BufferedRangesScan> result;
        result.set(0, buffered_time_ranges(byte_ranges));
        return result;
    }

protected:
    virtual DecoderErrorOr<SeekResult> seek_to_timestamp(AK::Duration) const { return Empty {}; }
    virtual BufferedRangesScan buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges) const = 0;
};

}
