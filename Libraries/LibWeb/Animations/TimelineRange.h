/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWeb/Animations/TimeValue.h>

namespace Web::Animations {

// https://drafts.csswg.org/scroll-animations-1/#named-ranges
enum class TimelineRangeName : u8 {
    // NB: An edge given without a range name is measured along the entire timeline.
    None,
    Cover,
    Contain,
    Entry,
    Exit,
    EntryCrossing,
    ExitCrossing,
    Scroll,
};

// One edge of an animation attachment range: a point measured from the start of a named timeline range, as a length
// plus a percentage of the range.
// https://drafts.csswg.org/scroll-animations-1/#animation-range-start
struct TimelineRangeEdge {
    TimelineRangeName name { TimelineRangeName::None };
    double offset_px { 0 };
    double offset_percentage { 0 };

    bool operator==(TimelineRangeEdge const&) const = default;
};

// https://drafts.csswg.org/scroll-animations-1/#animation-attachment-range
struct AnimationAttachmentRange {
    // NB: The initial edges, normal, are the start and the end of the entire timeline.
    TimelineRangeEdge start { TimelineRangeName::None, 0, 0 };
    TimelineRangeEdge end { TimelineRangeName::None, 0, 100 };

    bool operator==(AnimationAttachmentRange const&) const = default;
};

// How far apart two progress-based timeline times, in percent, may be and still be the same but for floating point error.
// NB: Must match PROGRESS_TOLERANCE in the style engine.
constexpr double progress_timeline_tolerance = 1e-6;

// The timeline times of the start and the end of an attachment range.
struct AttachmentInterval {
    TimeValue start;
    TimeValue end;

    bool operator==(AttachmentInterval const&) const = default;
};

}
