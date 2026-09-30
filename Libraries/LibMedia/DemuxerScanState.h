/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/Export.h>
#include <LibMedia/TimeRanges.h>

namespace Media {

struct MEDIA_API DemuxerScanState {
    // The ranges of each audio and video track in the source.
    Vector<TimeRanges> track_buffered_ranges;
    bool reached_end_of_stream { false };
    AK::Duration duration;

    static DemuxerScanState create_from_track_scans(Vector<BufferedRangesScan> track_scans, AK::Duration minimum_duration, bool closing_bytes_are_available);

    AK::Duration highest_track_end_time() const;
    TimeRanges buffered_ranges() const;

    static TimeRanges buffered_ranges_of_sources(ReadonlySpan<DemuxerScanState const*>);

    bool operator==(DemuxerScanState const&) const = default;
};

}
