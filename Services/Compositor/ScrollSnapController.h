/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Compositor/ScrollSnapSelection.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Compositor {

class AsyncScrollTree;

}

namespace Web::Painting {

class ScrollStateSnapshot;

}

namespace Compositor {

// Selects the snap positions of the wheel scrolls the compositor performs on snap containers, from the geometry the
// display list carries, the way the main thread selects them for the scrolls it performs itself. The context owns the
// scroll animations; this controller decides where they go and remembers what a gesture has asked for so far.
class ScrollSnapController {
public:
    enum class WheelStepOutcome : u8 {
        // The step selects no snap position and scrolls by its delta.
        NotASnapStep,
        // The step selected the snap position the scrolling box already rests at or is scrolling to.
        Consumed,
        // The step selected a snap position for the scrolling box to scroll to.
        StartSnapScroll,
    };

    struct WheelStepDecision {
        WheelStepOutcome outcome { WheelStepOutcome::NotASnapStep };
        Web::Compositor::AsyncScrollNodeStableID stable_node_id;
        Web::CSSPixelPoint initial_scroll_offset;
        Web::CSSPixelPoint unsnapped_scroll_destination;
        Web::Compositor::SnapDestination selection;
    };

    void did_install_scrolling_state(Web::Compositor::AsyncScrollTree const&);
    void did_start_main_thread_scroll(Web::Compositor::AsyncScrollNodeStableID);
    void did_start_snap_scroll(Web::Compositor::AsyncScrollNodeStableID, Web::Compositor::AsyncScrollOperationID, Web::CSSPixelPoint destination);
    void did_end_snap_scroll(Web::Compositor::AsyncScrollNodeStableID, Web::Compositor::AsyncScrollOperationID, Optional<Web::CSSPixelPoint> scroll_offset);
    bool is_snap_scroll(Web::Compositor::AsyncScrollNodeStableID, Web::Compositor::AsyncScrollOperationID) const;

    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
    // A discrete wheel step is a relative scroll with only an intended direction.
    WheelStepDecision decide_discrete_step(Web::Compositor::AsyncScrollTree const&, Web::Painting::ScrollStateSnapshot const&, Web::Compositor::AsyncScrollNodeID, Web::CSSPixelPoint delta, MonotonicTime now);

private:
    struct NodeState {
        // The offset the gesture's steps have asked for without snapping, which its next step travels from, so that a
        // burst of steps advances by the distance they asked for rather than by one snap position each.
        Optional<Web::CSSPixelPoint> unsnapped_scroll_destination;
        Optional<MonotonicTime> gesture_input_deadline;
        // The offset this controller last left the scrolling box at; a box resting anywhere else has been scrolled by
        // something else since, which ends the gesture.
        Optional<Web::CSSPixelPoint> expected_scroll_offset;
        Optional<Web::Compositor::AsyncScrollOperationID> snap_scroll_operation_id;
        Optional<Web::CSSPixelPoint> snap_scroll_destination;
    };

    NodeState& node_state(Web::Compositor::AsyncScrollNodeStableID);
    Web::Compositor::SnapAxisCandidates const& candidates_for(Web::Compositor::AsyncScrollNodeStableID, Web::Compositor::AsyncSnapContainer const&);

    HashMap<Web::Compositor::AsyncScrollNodeStableID, NodeState> m_nodes;
    HashMap<Web::Compositor::AsyncScrollNodeStableID, Web::Compositor::SnapAxisCandidates> m_candidates;
};

}
