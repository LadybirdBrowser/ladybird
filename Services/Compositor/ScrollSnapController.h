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
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Compositor/ScrollSnapSelection.h>
#include <LibWeb/Compositor/Types.h>
#include <LibWeb/Page/InputEvent.h>
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
    // A snap scroll to start for a wheel delta.
    struct SnapScrollStart {
        Web::Compositor::AsyncScrollNodeStableID stable_node_id;
        Web::CSSPixelPoint initial_scroll_offset;
        Web::CSSPixelPoint unsnapped_scroll_destination;
        Web::Compositor::SnapDestination selection;
        Web::Compositor::ScrollAnimationKind animation_kind { Web::Compositor::ScrollAnimationKind::SmoothScroll };
    };

    // A wheel delta that selected the snap position the scrolling box already rests at, or is already scrolling to,
    // is consumed without disturbing where the box is going.
    struct StepConsumed { };

    // What a wheel delta over a snap container does with the snap position it selected; a delta that selects none
    // scrolls the box by itself.
    using WheelStepDecision = Variant<StepConsumed, SnapScrollStart>;

    struct GestureEndSnap {
        Web::Compositor::AsyncScrollNodeID node_id;
        SnapScrollStart snap_scroll;
    };

    void did_install_scrolling_state(Web::Compositor::AsyncScrollTree const&);
    void did_start_main_thread_scroll(Web::Compositor::AsyncScrollNodeStableID);
    void did_start_snap_scroll(Web::Compositor::AsyncScrollNodeStableID, Web::Compositor::AsyncScrollOperationID, Web::CSSPixelPoint destination);
    void did_end_snap_scroll(Web::Compositor::AsyncScrollNodeStableID, Web::Compositor::AsyncScrollOperationID, Optional<Web::CSSPixelPoint> scroll_offset);
    bool is_snap_scroll(Web::Compositor::AsyncScrollNodeStableID, Web::Compositor::AsyncScrollOperationID) const;

    // Input that scrolls with a gesture reports its phases; the momentum of a flick and the end of a gesture snap
    // from what the gesture has done so far.
    void note_gesture_phase(Web::ScrollGesturePhase);
    void did_scroll_node_plainly(Web::Compositor::AsyncScrollNodeStableID, Web::ScrollGesturePhase, Web::CSSPixelPoint scroll_offset_before_scroll);

    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
    // A discrete wheel step is a relative scroll with only an intended direction.
    Optional<WheelStepDecision> decide_discrete_step(Web::Compositor::AsyncScrollTree const&, Web::Painting::ScrollStateSnapshot const&, Web::Compositor::AsyncScrollNodeID, Web::CSSPixelPoint delta, MonotonicTime now);
    // The momentum of a flick is a relative scroll with an intended direction and, once its decay tells where it is
    // headed, an intended end position.
    Optional<WheelStepDecision> decide_momentum_delta(Web::Compositor::AsyncScrollTree const&, Web::Painting::ScrollStateSnapshot const&, Web::Compositor::AsyncScrollNodeID, Web::CSSPixelPoint delta);
    // The end of a gesture snaps each scrolling box it panned to the snap position nearest where it was released,
    // and ends the gesture.
    Vector<GestureEndSnap> decide_gesture_end(Web::Compositor::AsyncScrollTree const&, Web::Painting::ScrollStateSnapshot const&);

private:
    struct InFlightSnapScroll {
        Web::Compositor::AsyncScrollOperationID operation_id;
        Web::CSSPixelPoint destination;
    };

    // Where the scrolling box rested when the gesture's input started moving it, from which the end of the gesture
    // snaps; momentum that traveled over the box must not pass a snap position that always stops.
    struct GestureStart {
        Web::CSSPixelPoint offset;
        bool travels_under_momentum { false };
    };

    struct NodeState {
        // The offset the gesture's steps have asked for without snapping, which its next step travels from, so that a
        // burst of steps advances by the distance they asked for rather than by one snap position each.
        Optional<Web::CSSPixelPoint> unsnapped_scroll_destination;
        Optional<MonotonicTime> gesture_input_deadline;
        // The offset this controller last left the scrolling box at; a box resting anywhere else has been scrolled by
        // something else since, which ends the gesture.
        Optional<Web::CSSPixelPoint> expected_scroll_offset;
        // The snap scroll of this controller's own that the box is scrolling under.
        Optional<InFlightSnapScroll> snap_scroll;
        Optional<GestureStart> gesture_start;
    };

    // Momentum that selects no snap position is scrolled by for the rest of the gesture rather than being asked again
    // for each delta it produces.
    enum class MomentumSnapPositionSelection : u8 {
        NotSelectedYet,
        ScrollingToSelectedPosition,
        NoPositionSelected,
    };

    // A scroll node a snap position can be selected for: one with snap geometry and a current offset.
    struct SnapTarget {
        Web::Compositor::AsyncScrollNodeStableID stable_node_id;
        Web::Compositor::AsyncSnapContainer const* snap_container;
        Web::CSSPixelPoint current_scroll_offset;
    };
    static Optional<SnapTarget> snap_target_for(Web::Compositor::AsyncScrollTree const&, Web::Painting::ScrollStateSnapshot const&, Web::Compositor::AsyncScrollNodeID);

    NodeState& node_state(Web::Compositor::AsyncScrollNodeStableID);
    Web::Compositor::SnapAxisCandidates const& candidates_for(Web::Compositor::AsyncScrollNodeStableID, Web::Compositor::AsyncSnapContainer const&);

    void reset_momentum_fling_state();
    void end_gesture();

    HashMap<Web::Compositor::AsyncScrollNodeStableID, NodeState> m_nodes;
    HashMap<Web::Compositor::AsyncScrollNodeStableID, Web::Compositor::SnapAxisCandidates> m_candidates;
    MomentumSnapPositionSelection m_momentum_snap_position_selection { MomentumSnapPositionSelection::NotSelectedYet };
    Web::Compositor::MomentumFlingEstimator m_momentum_fling_estimator;
};

}
