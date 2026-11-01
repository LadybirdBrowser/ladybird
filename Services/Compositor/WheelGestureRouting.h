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

    struct NotedWheelEvent {
        // The event starts another gesture than the one before it, which lets go of the latch.
        bool starts_gesture { false };
        // The event is the first step of its gesture that moves anything. A touchpad gesture can start with a step that
        // has no delta.
        bool is_first_wheel_event_with_delta { false };
    };
    // Every wheel event is noted as this context first sees it: from the UI, or from the main thread for one the UI did
    // not send.
    NotedWheelEvent note_wheel_event(Web::MouseEvent const&, Gfx::FloatPoint position, Compositing::AsyncScrollTree const&, MonotonicTime now);
    // The id of the gesture of the wheel events this context notes. No gesture of any context in the process has the
    // same id, so a context nested in another one can tell the gestures of the containing context from its own.
    Optional<u64> gesture_id() const;
    // The context whose page dispatches the wheel events of the gesture: the innermost one under the pointer as the
    // gesture started, which may be nested in this one. Empty for this context.
    Optional<Web::CompositorContextId> wheel_event_listener_context() const;
    void set_wheel_event_listener_context(Web::CompositorContextId);
    // The UI's input of the gesture that this context left to the page. It bounds what the page of another process may
    // scroll here, and in the contexts nested here, for the gesture.
    void note_delta_left_to_the_page(Gfx::FloatPoint delta_in_css_pixels);
    bool take_delta_left_to_the_page(Gfx::FloatPoint delta_in_css_pixels);
    // Whether the UI sent a later wheel event that this step cannot come before in one gesture, which makes it a step
    // of a gesture this context has already ended or moved past.
    bool step_was_overtaken_by_ui(Web::ScrollGesturePhase, u64 input_event_id) const;

    bool has_latch() const { return m_latch.has_value(); }
    bool has_latch_for_gesture(Optional<u64> gesture_id) const { return m_latch.has_value() && m_latch->gesture_id == gesture_id; }
    Optional<Web::CompositorContextId> nested_context_of_latch() const;
    Optional<Web::AsyncScrollNodeStableID> latched_scroller() const;
    void set_touchpad_gesture_end(Compositing::TouchpadGestureEnd touchpad_gesture_end) { m_touchpad_gesture_end = touchpad_gesture_end; }
    // Whether the fingers of the gesture are on the touchpad, where the gesture lasts until they lift.
    bool gesture_lasts_until_fingers_lift() const;
    // The scroller of such a gesture, or of one a containing context hands the steps of to this one.
    Optional<Web::AsyncScrollNodeStableID> scroller_held_by_fingers() const;
    // What a gesture is latched to: a scroller in the current scroll tree, a context nested in this one that latched the
    // gesture to a scroller of its own, or nothing that scrolls, when this context keeps the steps from the document
    // around it.
    struct ResolvedLatch {
        Optional<Compositing::SpatialNodeIndex> scroll_node_index {};
        Optional<Web::CompositorContextId> nested_context_id {};
        bool keeps_steps { false };
    };
    // The latch of the gesture a step continues, advanced to the step's phase. A latch whose scroller the current
    // display list no longer has, or that was taken for another gesture than the step's, is dropped here.
    ResolvedLatch resolve_latch(Compositing::AsyncScrollTree const&, Compositing::AccumulatedVisualContextTree const& current_visual_context_tree, Web::ScrollGesturePhase, Optional<u64> gesture_id);
    void latch(Web::AsyncScrollNodeStableID, Web::ScrollGesturePhase, Optional<u64> gesture_id);
    void latch_to_nested_context(Web::CompositorContextId, Optional<Web::AsyncScrollNodeStableID> scroller, Web::ScrollGesturePhase, Optional<u64> gesture_id);
    void latch_keeping_steps(Web::ScrollGesturePhase, Optional<u64> gesture_id);
    void clear_latch() { m_latch.clear(); }
    void clear_latch_of_gesture(u64 gesture_id);
    void forget_gesture();
    void relatch_to(Web::AsyncScrollNodeStableID);
    // This context's page dispatched a step of the gesture and did not cancel it, or canceled one.
    void note_page_let_gesture_through(u64 gesture_id);
    void forget_page_let_gesture_through(u64 gesture_id);
    // Whether the page let the gesture through, and its wheel event listeners are as they were then.
    bool page_let_gesture_through(Optional<u64> gesture_id) const;
    // Why this context's wheel event listeners hold the steps of a latched gesture back for the page, if they do. Once
    // the page let the gesture through, they only do so after they change.
    Optional<Web::InputRoutingDecision> reason_to_hold_latched_steps_back(Optional<u64> gesture_id) const;
    bool momentum_of_latched_gesture_can_spring_scroller_back() const;
    void set_momentum_of_latched_gesture_has_sprung_scroller_back(bool);

private:
    // The scroller the first step of a wheel gesture was routed to. Every later step of the gesture scrolls it without
    // hit testing, and stops at its edge rather than handing the rest of the gesture to an ancestor.
    struct Latch {
        // The gesture the latch was taken for, which only the steps of that gesture continue. A context nested in
        // another one takes it for the gesture of the containing context. Empty when the step that took it was in no
        // gesture the compositor knows of.
        Optional<u64> gesture_id;
        // Empty for a latch that keeps the steps without a scroller.
        Optional<Web::AsyncScrollNodeStableID> stable_node_id;
        // The phase of the last step that reached the latch, which says whether the fingers of its gesture are down.
        Web::ScrollGesturePhase last_scroll_gesture_phase { Web::ScrollGesturePhase::None };
        Optional<Web::CompositorContextId> nested_context_id {};
        // The momentum of the gesture has gone into a rebound of the scroller. Momentum outlasts that rebound, and the
        // rest of it must not start another one.
        bool momentum_has_sprung_the_scroller_back { false };
    };

    struct NewestWheelEventIdsFromUI {
        u64 ongoing { 0 };
        u64 momentum { 0 };
        u64 ended { 0 };
    };

    Compositing::WheelRoutingAdmission m_admission { Compositing::WheelRoutingAdmission::NoAsyncScrollingState };
    bool m_has_blocking_wheel_event_listeners { false };
    u64 m_wheel_event_listener_state_generation { 0 };
    struct Gesture {
        u64 id;
        Compositing::WheelGestureIdentity identity;
        Optional<Web::CompositorContextId> wheel_event_listener_context {};
        // Summed per axis, whatever the direction.
        Gfx::FloatPoint delta_left_to_the_page {};
    };
    // The gesture of the wheel events this context sees first: the UI's, or ones the UI never sent that the main
    // thread makes. It says which gesture a step is in, whether the page lets the step through before or after this
    // context scrolls by it.
    Optional<Gesture> m_gesture;
    NewestWheelEventIdsFromUI m_newest_wheel_event_ids_from_ui;
    bool m_gesture_had_wheel_event_with_delta { false };
    Optional<Latch> m_latch;
    // The gesture whose step this context's page dispatched and did not cancel, and the state of the page's wheel event
    // listeners then. The later steps of the gesture do not wait for the page for as long as the listeners stay so.
    struct GestureThePageLetThrough {
        u64 gesture_id { 0 };
        u64 wheel_event_listener_state_generation { 0 };
    };
    Optional<GestureThePageLetThrough> m_gesture_the_page_let_through;
    Compositing::TouchpadGestureEnd m_touchpad_gesture_end { Compositing::TouchpadGestureEnd::AfterIdleDelay };
};

}
