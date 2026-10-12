/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Export.h>
#include <LibMedia/TimeRanges.h>

namespace Media {

enum class BufferedRangesPolicy : u8 {
    EnabledTracks,
    AllTracks,
};

struct CombinedScanState {
    TimeRanges time_ranges;
    bool reached_end_of_stream { false };
    bool resource_is_complete { false };
    AK::Duration duration;
};

struct MEDIA_API DemuxerScanState {
    // Includes every audio and video track, even those with no buffered ranges.
    Vector<TrackBufferedRanges> track_buffered_ranges;
    // Chosen by the demuxer, to allow the intersection over active source buffers for MSE.
    BufferedRangesPolicy buffered_ranges_policy { BufferedRangesPolicy::EnabledTracks };
    bool reached_end_of_stream { false };
    bool resource_is_complete { false };
    AK::Duration duration;

    static DemuxerScanState create_from_track_scans(Vector<BufferedRangesScan> track_scans, AK::Duration minimum_duration, bool closing_bytes_are_available);

    TimeRanges buffered_ranges() const;
    CombinedScanState combined_scan_state(Function<bool(u64)> const& is_track_enabled = [](u64) { return true; }) const;

    static BufferedRanges buffered_ranges_of_sources(ReadonlySpan<CombinedScanState>);

    bool operator==(DemuxerScanState const&) const = default;
};

}

namespace IPC {

template<>
MEDIA_API ErrorOr<void> encode(Encoder&, Media::DemuxerScanState const&);

template<>
MEDIA_API ErrorOr<Media::DemuxerScanState> decode(Decoder&);

}
