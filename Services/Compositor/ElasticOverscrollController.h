/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibCompositing/Scrolling/ElasticOverscroll.h>
#include <LibCompositing/Scrolling/ScrollFling.h>
#include <LibCompositing/Scrolling/ScrollState.h>
#include <LibGfx/Point.h>
#include <LibWebCommon/Page/InputEvent.h>

namespace Compositing {

class AsyncScrollTree;

}

namespace Compositor {

class ElasticOverscrollController {
public:
    void set_enabled(bool);
    bool step_can_stretch(Web::WheelDeltaPrecision, Web::ScrollGesturePhase) const;
    void note_step_from_ui(Gfx::FloatPoint delta, MonotonicTime now);

    bool has_active_overscroll_of(Web::AsyncScrollNodeStableID) const;
    Gfx::FloatPoint stretch_of(Web::AsyncScrollNodeStableID) const;
    // Uses the part of a finger step that points back to the scroll range to relax the stretch of the scroller, and
    // returns the rest of the step.
    Gfx::FloatPoint relax_by(Web::AsyncScrollNodeStableID, Gfx::FloatPoint step_delta);
    // Returns whether the scroller is at an edge that the step points past, which is what lets the step pull it.
    bool pull_by(Compositing::AsyncScrollNode const&, Gfx::FloatPoint scroll_offset, Gfx::FloatPoint step_delta, Gfx::FloatPoint delta_the_scroller_did_not_scroll_by, float visual_viewport_magnification);
    void release_held_by_fingers(MonotonicTime now);
    // A scroller that something else scrolls along an axis it is stretched in lets go of the stretch at once, while one
    // that something else scrolls smoothly springs back from it.
    void drop_stretch_moved_by(Web::AsyncScrollNodeStableID, Gfx::FloatPoint scroll_delta);
    void release_stretch_of(Web::AsyncScrollNodeStableID, MonotonicTime now);
    void end_gesture(MonotonicTime now);
    void forget();
    void did_install_scrolling_state(Compositing::AsyncScrollTree const&);

    bool has_rebound() const;
    bool advance_rebounds(MonotonicTime frame_time);

    // The scroll offsets to present when a scroller is stretched, which are the only ones that leave a scroll range.
    Optional<Compositing::ScrollStateSnapshot> stretched_scroll_state_snapshot(Compositing::ScrollStateSnapshot const&, Compositing::AsyncScrollTree const&) const;

private:
    // A scroller that a touchpad gesture holds past an edge of its scroll range, or that is springing back from there.
    // The stretch is only presented: it is in no scroll offset that the scroll tree or WebContent sees.
    struct ActiveOverscroll {
        Web::AsyncScrollNodeStableID stable_node_id;
        Compositing::ElasticOverscroll overscroll;
    };

    ActiveOverscroll* active_overscroll_of(Web::AsyncScrollNodeStableID);

    bool m_enabled { false };
    Vector<ActiveOverscroll> m_active_overscrolls;
    Compositing::ScrollVelocityTracker m_step_velocity_tracker;
};

}
