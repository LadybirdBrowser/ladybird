/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibWeb/Animations/TimeValue.h>
#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>

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

// https://drafts.csswg.org/scroll-animations-1/#typedef-timeline-range-name
inline StringView timeline_range_name_to_string(TimelineRangeName name)
{
    switch (name) {
    case TimelineRangeName::None:
        return "normal"sv;
    case TimelineRangeName::Cover:
        return "cover"sv;
    case TimelineRangeName::Contain:
        return "contain"sv;
    case TimelineRangeName::Entry:
        return "entry"sv;
    case TimelineRangeName::Exit:
        return "exit"sv;
    case TimelineRangeName::EntryCrossing:
        return "entry-crossing"sv;
    case TimelineRangeName::ExitCrossing:
        return "exit-crossing"sv;
    case TimelineRangeName::Scroll:
        return "scroll"sv;
    }
    VERIFY_NOT_REACHED();
}

// One edge of an animation attachment range: a point measured from the start of a named timeline range, as a length
// plus a percentage of the range.
// https://drafts.csswg.org/scroll-animations-1/#animation-range-start
struct TimelineRangeEdge {
    TimelineRangeName name { TimelineRangeName::None };
    double offset_px { 0 };
    double offset_percentage { 0 };
    // NB: An offset only a calculation can describe, such as min(50%, 10px), resolves against the size of its range once
    //     that is known. offset_px and offset_percentage then describe it as it resolves against 0px and 100px.
    CSS::RustStyleValueHandle calculated_offset {};

    // The offset, in pixels, along a range of the given size.
    double offset_along(double range_size) const;

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

// One side of the view progress visibility range: auto takes the scroll container's scroll padding, otherwise a length
// plus a percentage of the scrollport.
// https://drafts.csswg.org/scroll-animations-1/#view-timeline-inset
struct ViewTimelineInsetEdge {
    bool is_auto { true };
    double px { 0 };
    double percentage { 0 };
    // NB: An inset only a calculation can describe, such as min(50%, 10px), resolves against the scrollport size.
    CSS::RustStyleValueHandle calculated {};

    // The inset, in pixels, of a side that is not auto, in a scrollport of the given size.
    double resolve(double scrollport_size) const;

    bool operator==(ViewTimelineInsetEdge const&) const = default;
};

// Takes a reference to a calculated offset or inset the style engine hands over, which may be null.
CSS::RustStyleValueHandle retain_calculated_offset(void const*);

struct ViewTimelineInset {
    ViewTimelineInsetEdge start;
    ViewTimelineInsetEdge end;

    bool operator==(ViewTimelineInset const&) const = default;
};

}
