/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Scrolling/AsyncScrollingState.h>
#include <LibCompositing/Scrolling/WheelGestureIdentity.h>
#include <LibGfx/Point.h>
#include <LibWebCommon/Page/CompositorContextId.h>
#include <LibWebCommon/Page/InputEvent.h>

namespace Compositing {

class AsyncScrollTree;

}

namespace Compositor {

class WheelGestureRouting {
public:
    void did_install_scrolling_state(Compositing::AsyncScrollingState const&);
    void invalidate_wheel_event_listener_state(u64 generation);
    Compositing::WheelRoutingAdmission admission() const { return m_admission; }
    bool admits_async_wheel() const { return m_admission == Compositing::WheelRoutingAdmission::Accepted; }
    bool admits_async_pinch() const;
    bool only_blocking_wheel_event_listeners_hold_steps_back() const;

    // Returns whether the event starts another gesture than the one the UI was sending.
    bool note_wheel_event_from_ui(Web::MouseEvent const&, Gfx::FloatPoint position, Compositing::AsyncScrollTree const&, MonotonicTime now);
    // Whether the wheel event is the first step of the gesture from the UI that moves anything. A touchpad gesture can
    // start with a step that has no delta.
    bool is_first_wheel_event_from_ui_with_delta(u64 input_event_id) const { return input_event_id != 0 && input_event_id == m_id_of_first_wheel_event_from_ui_with_delta; }
    // Whether the UI sent a later wheel event that this step cannot come before in one gesture, which makes it a step
    // of a gesture this context has already ended or moved past.
    bool step_was_overtaken_by_ui(Web::ScrollGesturePhase, u64 input_event_id) const;

    bool has_latch() const { return m_latch.has_value(); }
    Optional<Web::AsyncScrollNodeStableID> latched_scroller() const;
    // What a gesture is latched to: a scroller in the current scroll tree, a context nested in this one that latched the
    // gesture to a scroller of its own, or nothing that scrolls, when this context keeps the steps from the document
    // around it.
    struct ResolvedLatch {
        Optional<Compositing::AsyncScrollNodeID> node_id {};
        Optional<Web::CompositorContextId> nested_context_id {};
        bool keeps_steps { false };
    };
    // The latch of the gesture the wheel event continues, with the gesture advanced to the event. A latch the event
    // does not continue, or whose scroller the current display list no longer has, is dropped here.
    ResolvedLatch resolve_latch(Compositing::AsyncScrollTree const&, Compositing::AccumulatedVisualContextTree const& current_visual_context_tree, Gfx::FloatPoint position, Web::ScrollGesturePhase, u32 modifiers, MonotonicTime now);
    // The same for a step that the containing context hands in, which has decided that the step continues the gesture.
    ResolvedLatch resolve_latch_continued_by_containing_context(Compositing::AsyncScrollTree const&, Compositing::AccumulatedVisualContextTree const& current_visual_context_tree, Web::ScrollGesturePhase, MonotonicTime now);
    void latch(Web::AsyncScrollNodeStableID, Gfx::FloatPoint position, Web::ScrollGesturePhase, u32 modifiers, MonotonicTime now);
    void latch_to_nested_context(Web::CompositorContextId, Optional<Web::AsyncScrollNodeStableID> scroller, Gfx::FloatPoint position, Web::ScrollGesturePhase, u32 modifiers, MonotonicTime now);
    void latch_keeping_steps(Gfx::FloatPoint position, Web::ScrollGesturePhase, u32 modifiers, MonotonicTime now);
    void clear_latch() { m_latch.clear(); }
    void relatch_to(Web::AsyncScrollNodeStableID);
    void note_page_let_latched_gesture_through();
    bool page_let_latched_touchpad_gesture_through(Web::WheelDeltaPrecision, Web::ScrollGesturePhase) const;
    bool momentum_of_latched_gesture_can_spring_scroller_back() const;
    void set_momentum_of_latched_gesture_has_sprung_scroller_back(bool);

private:
    // The scroller the first step of a wheel gesture was routed to. Every later step of the gesture scrolls it without
    // hit testing, and stops at its edge rather than handing the rest of the gesture to an ancestor.
    struct Latch {
        // Empty for a latch that keeps the steps without a scroller.
        Optional<Web::AsyncScrollNodeStableID> stable_node_id;
        Compositing::WheelGestureIdentity gesture;
        Optional<Web::CompositorContextId> nested_context_id {};
        // The momentum of the gesture has gone into a rebound of the scroller. Momentum outlasts that rebound, and the
        // rest of it must not start another one.
        bool momentum_has_sprung_the_scroller_back { false };
        // Set once the page dispatched a step of a touchpad gesture that blocking wheel event listeners held back, and
        // did not cancel it. The rest of the gesture is scrolled here first, without asking the page again, for as long
        // as no blocking listener has been added or removed since.
        Optional<u64> wheel_event_listener_state_generation_the_page_let_the_gesture_through_at {};
    };

    struct NewestWheelEventIdsFromUI {
        u64 ongoing { 0 };
        u64 momentum { 0 };
        u64 ended { 0 };
    };

    Compositing::WheelRoutingAdmission m_admission { Compositing::WheelRoutingAdmission::NoAsyncScrollingState };
    bool m_has_blocking_wheel_event_listeners { false };
    u64 m_wheel_event_listener_state_generation { 0 };
    // The touchpad gesture the UI is sending, whether this context or the main thread scrolls its steps.
    Optional<Compositing::WheelGestureIdentity> m_gesture_from_ui;
    NewestWheelEventIdsFromUI m_newest_wheel_event_ids_from_ui;
    u64 m_id_of_first_wheel_event_from_ui_with_delta { 0 };
    bool m_gesture_from_ui_had_wheel_event_with_delta { false };
    Optional<Latch> m_latch;
};

}
