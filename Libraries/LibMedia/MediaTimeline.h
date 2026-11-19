/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibMedia/TimeRanges.h>

namespace Media {

struct MediaTimeline {
    // AK::Duration::max() means the duration is unbounded, as a live stream's is.
    AK::Duration duration;
    Optional<TimeRanges::Range> live_seekable_range;
    AK::Duration earliest_possible_position;
};

}
