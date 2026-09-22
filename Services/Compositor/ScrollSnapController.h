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
#include <LibCompositing/InputEvent.h>
#include <LibCompositing/PixelUnits.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibCompositing/Scrolling/ScrollSnapSelection.h>
#include <LibCompositing/Types.h>

namespace Compositing {

class AsyncScrollTree;

}

namespace Compositing {

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
        Compositing::AsyncScrollNodeStableID stable_node_id;
        Compositing::CSSPixelPoint initial_scroll_offset;
        Compositing::CSSPixelPoint unsnapped_scroll_destination;
        Compositing::SnapDestination selection;
        Compositing::ScrollAnimationKind animation_kind { Compositing::ScrollAnimationKind::SmoothScroll };
    };

    // A step that selected the snap position the scrolling box already rests at, or is already scrolling to, is
    // consumed without disturbing where the box is going. Keys can still advance the input for that scroll.
    struct StepConsumed {
        Optional<Compositing::StartedUserScroll> updated_scroll;
    };

    // What a scroll step over a snap container does with the snap position it selected; a step that selects none
    // scrolls the box by itself.
    using StepDecision = Variant<StepConsumed, SnapScrollStart>;

    struct GestureEndSnap {
        Compositing::AsyncScrollNodeID node_id;
        SnapScrollStart snap_scroll;
    };

    void did_install_scrolling_state(Compositing::AsyncScrollTree const&);
    void did_start_main_thread_scroll(Compositing::AsyncScrollNodeStableID);
    void did_start_snap_scroll(Compositing::AsyncScrollNodeStableID, Compositing::AsyncScrollOperationID, Compositing::CSSPixelPoint destination);
    void did_end_snap_scroll(Compositing::AsyncScrollNodeStableID, Compositing::AsyncScrollOperationID, Optional<Compositing::CSSPixelPoint> scroll_offset);
    bool is_snap_scroll(Compositing::AsyncScrollNodeStableID, Compositing::AsyncScrollOperationID) const;
    Optional<Compositing::CSSPixelPoint> unsnapped_destination_for_snap_scroll(Compositing::AsyncScrollNodeStableID, Compositing::AsyncScrollOperationID) const;

    // Input that scrolls with a gesture reports its phases; the momentum of a flick and the end of a gesture snap
    // from what the gesture has done so far.
    void note_gesture_phase(Compositing::ScrollGesturePhase);
    void did_scroll_node_plainly(Compositing::AsyncScrollNodeStableID, Compositing::ScrollGesturePhase, Compositing::CSSPixelPoint scroll_offset_before_scroll);

    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
    // A discrete wheel step is a relative scroll with only an intended direction.
    Optional<StepDecision> decide_discrete_step(Compositing::AsyncScrollTree const&, Compositing::ScrollStateSnapshot const&, Compositing::AsyncScrollNodeID, Compositing::CSSPixelPoint delta, MonotonicTime now);
    // The momentum of a flick is a relative scroll with an intended direction and, once its decay tells where it is
    // headed, an intended end position.
    Optional<StepDecision> decide_momentum_delta(Compositing::AsyncScrollTree const&, Compositing::ScrollStateSnapshot const&, Compositing::AsyncScrollNodeID, Compositing::CSSPixelPoint delta);
    // A scroll key is a command of its own rather than a step of a gesture: while a scroll is in flight it travels on
    // from the offset the steps before it asked for, otherwise from the scrolling box itself. An arrow key has only
    // an intended direction; a paging key has an intended end position as well.
    Optional<StepDecision> decide_key_step(Compositing::AsyncScrollTree const&, Compositing::ScrollStateSnapshot const&, Compositing::AsyncScrollNodeID, Compositing::CSSPixelPoint delta, Compositing::SnapSelectionStrategy::Type, Optional<Compositing::CSSPixelPoint> scroll_in_flight_destination, MonotonicTime now);
    // The end of a gesture snaps each scrolling box it panned to the snap position nearest where it was released,
    // and ends the gesture.
    Vector<GestureEndSnap> decide_gesture_end(Compositing::AsyncScrollTree const&, Compositing::ScrollStateSnapshot const&);

    // A gesture whose steps this controller chains awaits its next step until its input deadline passes, and ends
    // once that happens without one.
    bool has_gesture_awaiting_input() const;
    Optional<MonotonicTime> earliest_gesture_input_deadline() const;
    bool end_gestures_whose_input_ran_out(MonotonicTime now);

private:
    struct InFlightSnapScroll {
        Compositing::AsyncScrollOperationID operation_id;
        Compositing::CSSPixelPoint destination;
    };

    // Where the scrolling box rested when the gesture's input started moving it, from which the end of the gesture
    // snaps; momentum that traveled over the box must not pass a snap position that always stops.
    struct GestureStart {
        Compositing::CSSPixelPoint offset;
        bool travels_under_momentum { false };
    };

    struct NodeState {
        // The offset the gesture's steps have asked for without snapping, which its next step travels from, so that a
        // burst of steps advances by the distance they asked for rather than by one snap position each.
        Optional<Compositing::CSSPixelPoint> unsnapped_scroll_destination;
        Optional<MonotonicTime> gesture_input_deadline;
        // The offset this controller last left the scrolling box at; a box resting anywhere else has been scrolled by
        // something else since, which ends the gesture.
        Optional<Compositing::CSSPixelPoint> expected_scroll_offset;
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
        Compositing::AsyncScrollNodeStableID stable_node_id;
        Compositing::AsyncSnapContainer const* snap_container;
        Compositing::CSSPixelPoint current_scroll_offset;
    };
    static Optional<SnapTarget> snap_target_for(Compositing::AsyncScrollTree const&, Compositing::ScrollStateSnapshot const&, Compositing::AsyncScrollNodeID);

    NodeState& node_state(Compositing::AsyncScrollNodeStableID);
    Compositing::SnapAxisCandidates const& candidates_for(Compositing::AsyncScrollNodeStableID, Compositing::AsyncSnapContainer const&);

    void reset_momentum_fling_state();
    void end_gesture();

    HashMap<Compositing::AsyncScrollNodeStableID, NodeState> m_nodes;
    HashMap<Compositing::AsyncScrollNodeStableID, Compositing::SnapAxisCandidates> m_candidates;
    MomentumSnapPositionSelection m_momentum_snap_position_selection { MomentumSnapPositionSelection::NotSelectedYet };
    Compositing::MomentumFlingEstimator m_momentum_fling_estimator;
};

}
