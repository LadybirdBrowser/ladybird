/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libcompositing_rust::scrolling::wheel_gesture_identity::{
    TouchpadGestureEnd, WHEEL_GESTURE_POSITION_SLOP_IN_CSS_PIXELS, WheelGestureIdentity,
};
use libcompositing_rust::scrolling::{
    AccumulatedVisualContextTree, AsyncScrollNodeStableID, AsyncScrollTree, AsyncScrollingState, CompositorContextId,
    InputRoutingDecision, MouseEvent, ScrollGesturePhase, SpatialNodeIndex, WheelRoutingAdmission,
    input_routing_decision_for_wheel_step_not_admitted_by, wheel_routing_admission_for_with_generation,
};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::FloatPoint;
use std::cell::Cell;
use std::rc::Rc;

#[derive(Default)]
pub struct WheelGestureIdAllocator {
    last_wheel_gesture_id: Cell<u64>,
    last_wheel_step_number: Cell<u64>,
}

impl WheelGestureIdAllocator {
    pub fn allocate_wheel_gesture_id(&self) -> u64 {
        let id = self.last_wheel_gesture_id.get().wrapping_add(1);
        self.last_wheel_gesture_id.set(id);
        id
    }

    // Every wheel step a context notes gets the next number, whether the UI sent it or the main thread made it, so the
    // numbers order all of them as the compositor first saw them.
    pub fn allocate_wheel_step_number(&self) -> u64 {
        let number = self.last_wheel_step_number.get().wrapping_add(1);
        self.last_wheel_step_number.set(number);
        number
    }
}

pub struct WheelGestureRouting {
    admission: WheelRoutingAdmission,
    wheel_event_listener_state_generation: u64,
    // The gesture of the wheel events this context sees first: the UI's, or ones the UI never sent that the main
    // thread makes. It says which gesture a step is in, whether the page lets the step through before or after this
    // context scrolls by it.
    gesture: Option<Gesture>,
    newest_step_numbers: NewestStepNumbers,
    gesture_had_wheel_event_with_delta: bool,
    latch: Option<Latch>,
    gesture_the_page_let_through: Option<GestureThePageLetThrough>,
    touchpad_gesture_end: TouchpadGestureEnd,
    id_allocator: Rc<WheelGestureIdAllocator>,
}

#[derive(Default)]
pub struct NotedWheelEvent {
    // The event starts another gesture than the one before it, which lets go of the latch.
    pub starts_gesture: bool,
    // The event is the first step of its gesture that moves anything. A touchpad gesture can start with a step that
    // has no delta.
    pub is_first_wheel_event_with_delta: bool,
    pub step_number: u64,
}

// What a gesture is latched to: a scroller in the current scroll tree, a context nested in this one that latched the
// gesture to a scroller of its own, or nothing that scrolls, when this context keeps the steps from the document
// around it.
#[derive(Default)]
pub struct ResolvedLatch {
    pub scroll_node_index: Option<SpatialNodeIndex>,
    pub nested_context_id: Option<CompositorContextId>,
    pub keeps_steps: bool,
}

// The scroller the first step of a wheel gesture was routed to. Every later step of the gesture scrolls it without
// hit testing, and stops at its edge rather than handing the rest of the gesture to an ancestor.
struct Latch {
    // The gesture the latch was taken for, which only the steps of that gesture continue. A context nested in
    // another one takes it for the gesture of the containing context. Empty when the step that took it was in no
    // gesture the compositor knows of.
    gesture_id: Option<u64>,
    // Empty for a latch that keeps the steps without a scroller.
    stable_node_id: Option<AsyncScrollNodeStableID>,
    // The phase of the last step that reached the latch, which says whether the fingers of its gesture are down.
    last_scroll_gesture_phase: ScrollGesturePhase,
    nested_context_id: Option<CompositorContextId>,
    // The momentum of the gesture has gone into a rebound of the scroller. Momentum outlasts that rebound, and the
    // rest of it must not start another one.
    momentum_has_sprung_the_scroller_back: bool,
}

#[derive(Default)]
struct NewestStepNumbers {
    ongoing: u64,
    momentum: u64,
    ended: u64,
}

struct Gesture {
    id: u64,
    identity: WheelGestureIdentity,
    wheel_event_listener_context: Option<CompositorContextId>,
    // Summed per axis, whatever the direction.
    delta_left_to_the_page: FloatPoint,
}

// The gesture whose step this context's page dispatched and did not cancel, and the state of the page's wheel event
// listeners then. The later steps of the gesture do not wait for the page for as long as the listeners stay so.
#[derive(Default)]
struct GestureThePageLetThrough {
    gesture_id: u64,
    wheel_event_listener_state_generation: u64,
}

fn position_slop_in_device_pixels(async_scroll_tree: &AsyncScrollTree) -> f32 {
    WHEEL_GESTURE_POSITION_SLOP_IN_CSS_PIXELS * async_scroll_tree.device_pixels_per_css_pixel() as f32
}

impl WheelGestureRouting {
    pub fn new(id_allocator: Rc<WheelGestureIdAllocator>) -> Self {
        Self {
            admission: WheelRoutingAdmission::NoAsyncScrollingState,
            wheel_event_listener_state_generation: 0,
            gesture: None,
            newest_step_numbers: NewestStepNumbers::default(),
            gesture_had_wheel_event_with_delta: false,
            latch: None,
            gesture_the_page_let_through: None,
            touchpad_gesture_end: TouchpadGestureEnd::AfterIdleDelay,
            id_allocator,
        }
    }

    pub fn did_install_scrolling_state(&mut self, async_scrolling_state: &AsyncScrollingState) {
        self.admission = wheel_routing_admission_for_with_generation(
            async_scrolling_state,
            self.wheel_event_listener_state_generation,
        );
        self.wheel_event_listener_state_generation = self
            .wheel_event_listener_state_generation
            .max(async_scrolling_state.wheel_event_listener_state_generation);
    }

    pub fn invalidate_wheel_event_listener_state(&mut self, generation: u64) {
        self.wheel_event_listener_state_generation = self.wheel_event_listener_state_generation.max(generation);
        self.admission = WheelRoutingAdmission::StaleWheelEventListeners;
    }

    pub fn admission(&self) -> WheelRoutingAdmission {
        self.admission
    }
    pub fn admits_async_wheel(&self) -> bool {
        self.admission == WheelRoutingAdmission::Accepted
    }

    pub fn admits_async_pinch(&self) -> bool {
        // Pinch zoom scales the visual viewport, which a page whose viewport cannot be scrolled has too.
        matches!(
            self.admission,
            WheelRoutingAdmission::Accepted | WheelRoutingAdmission::NoScrollNode
        )
    }

    // Every wheel event is noted as this context first sees it: from the UI, or from the main thread for one the UI did
    // not send.
    pub fn note_wheel_event(
        &mut self,
        event: &MouseEvent,
        position: FloatPoint,
        async_scroll_tree: &AsyncScrollTree,
        now: MonotonicTime,
    ) -> NotedWheelEvent {
        let phase = event.scroll_gesture_phase;
        let step_number = self.id_allocator.allocate_wheel_step_number();
        if phase == ScrollGesturePhase::Ongoing {
            self.newest_step_numbers.ongoing = step_number;
        } else if phase == ScrollGesturePhase::Momentum {
            self.newest_step_numbers.momentum = step_number;
        } else if phase == ScrollGesturePhase::Ended {
            self.newest_step_numbers.ended = step_number;
        }

        let starts_gesture = self.gesture.as_ref().is_none_or(|gesture| {
            !gesture.identity.is_continued_by(
                position,
                phase,
                event.modifiers.0,
                now,
                position_slop_in_device_pixels(async_scroll_tree),
                self.touchpad_gesture_end,
            )
        });
        if starts_gesture {
            self.gesture = Some(Gesture {
                id: self.id_allocator.allocate_wheel_gesture_id(),
                identity: WheelGestureIdentity::started_by(position, phase, event.modifiers.0, now),
                wheel_event_listener_context: None,
                delta_left_to_the_page: FloatPoint::default(),
            });
            self.gesture_had_wheel_event_with_delta = false;
            self.latch = None;
        } else if let Some(gesture) = self.gesture.as_mut() {
            gesture.identity.advance_to(phase, now);
        }
        let is_first_wheel_event_with_delta =
            !self.gesture_had_wheel_event_with_delta && (event.wheel_delta_x != 0.0 || event.wheel_delta_y != 0.0);
        if is_first_wheel_event_with_delta {
            self.gesture_had_wheel_event_with_delta = true;
        }
        NotedWheelEvent {
            starts_gesture,
            is_first_wheel_event_with_delta,
            step_number,
        }
    }

    // The id of the gesture of the wheel events this context notes. No gesture of any context in the process has the
    // same id, so a context nested in another one can tell the gestures of the containing context from its own.
    pub fn gesture_id(&self) -> Option<u64> {
        Some(self.gesture.as_ref()?.id)
    }

    // The context whose page dispatches the wheel events of the gesture: the innermost one under the pointer as the
    // gesture started, which may be nested in this one. Empty for this context.
    pub fn wheel_event_listener_context(&self) -> Option<CompositorContextId> {
        self.gesture.as_ref()?.wheel_event_listener_context
    }

    pub fn set_wheel_event_listener_context(&mut self, context_id: CompositorContextId) {
        if let Some(gesture) = self.gesture.as_mut() {
            gesture.wheel_event_listener_context = Some(context_id);
        }
    }

    // The UI's input of the gesture that this context left to the page. It bounds what the page of another process may
    // scroll here, and in the contexts nested here, for the gesture.
    pub fn note_delta_left_to_the_page(&mut self, delta_in_css_pixels: FloatPoint) {
        if let Some(gesture) = self.gesture.as_mut() {
            gesture.delta_left_to_the_page.translate_by(FloatPoint {
                x: delta_in_css_pixels.x.abs(),
                y: delta_in_css_pixels.y.abs(),
            });
        }
    }

    pub fn take_delta_left_to_the_page(&mut self, delta_in_css_pixels: FloatPoint) -> bool {
        let Some(gesture) = self.gesture.as_mut() else {
            return false;
        };
        // The page sends back the deltas it was given, which may have been summed up on the way.
        const ROUNDING_SLACK: f32 = 0.01;
        let delta_left = &mut gesture.delta_left_to_the_page;
        let step = FloatPoint {
            x: delta_in_css_pixels.x.abs(),
            y: delta_in_css_pixels.y.abs(),
        };
        if step.x > delta_left.x + ROUNDING_SLACK || step.y > delta_left.y + ROUNDING_SLACK {
            return false;
        }
        *delta_left = FloatPoint {
            x: max(0.0, delta_left.x - step.x),
            y: max(0.0, delta_left.y - step.y),
        };
        true
    }

    pub fn forget_gesture(&mut self) {
        self.gesture = None;
        self.gesture_had_wheel_event_with_delta = false;
        self.latch = None;
    }

    // Whether this context noted a later wheel step that this step cannot come before in one gesture, which makes it a step
    // of a gesture this context has already ended or moved past.
    pub fn step_was_overtaken(&self, phase: ScrollGesturePhase, step_number: u64) -> bool {
        let newest = &self.newest_step_numbers;
        match phase {
            ScrollGesturePhase::None => false,
            ScrollGesturePhase::Ongoing => {
                // The fingers lifted after this step.
                newest.momentum > step_number || newest.ended > step_number
            }
            ScrollGesturePhase::Momentum => {
                // The momentum ended, or the fingers touched down again, after this step.
                newest.ended > step_number || newest.ongoing > step_number
            }
            ScrollGesturePhase::Ended => {
                // Another gesture started after this end.
                newest.ongoing > step_number
            }
        }
    }

    pub fn has_latch_for_gesture(&self, gesture_id: Option<u64>) -> bool {
        self.latch.as_ref().is_some_and(|latch| latch.gesture_id == gesture_id)
    }
    pub fn set_touchpad_gesture_end(&mut self, touchpad_gesture_end: TouchpadGestureEnd) {
        self.touchpad_gesture_end = touchpad_gesture_end;
    }
    pub fn clear_latch(&mut self) {
        self.latch = None;
    }

    // The latch of the gesture a step continues, advanced to the step's phase. A latch whose scroller the current
    // display list no longer has, or that was taken for another gesture than the step's, is dropped here.
    pub fn resolve_latch(
        &mut self,
        async_scroll_tree: &AsyncScrollTree,
        current_visual_context_tree: Option<&AccumulatedVisualContextTree>,
        scroll_gesture_phase: ScrollGesturePhase,
        gesture_id: Option<u64>,
    ) -> ResolvedLatch {
        let Some(latch) = self.latch.as_mut() else {
            return ResolvedLatch::default();
        };
        if latch.gesture_id != gesture_id {
            self.latch = None;
            return ResolvedLatch::default();
        }
        if latch.nested_context_id.is_some() || latch.stable_node_id.is_none() {
            latch.last_scroll_gesture_phase = scroll_gesture_phase;
            return ResolvedLatch {
                nested_context_id: latch.nested_context_id,
                keeps_steps: latch.nested_context_id.is_none(),
                ..Default::default()
            };
        }
        let scroll_node_index = async_scroll_tree.scroll_node_index_for_stable_id(latch.stable_node_id.unwrap());
        if scroll_node_index.is_none() {
            self.latch = None;
            return ResolvedLatch::default();
        }
        // The hit test a latched step skips would refuse a tree the wheel hit-test targets were not built for.
        if !current_visual_context_tree.is_some_and(|tree| async_scroll_tree.has_wheel_hit_test_targets_for(tree)) {
            return ResolvedLatch::default();
        }
        latch.last_scroll_gesture_phase = scroll_gesture_phase;
        ResolvedLatch {
            scroll_node_index,
            ..Default::default()
        }
    }

    pub fn latched_scroller(&self) -> Option<AsyncScrollNodeStableID> {
        self.latch.as_ref()?.stable_node_id
    }

    // Whether the fingers of the gesture are on the touchpad, where the gesture lasts until they lift.
    pub fn gesture_lasts_until_fingers_lift(&self) -> bool {
        self.touchpad_gesture_end == TouchpadGestureEnd::WhenFingersLift
            && self
                .gesture
                .as_ref()
                .is_some_and(|gesture| gesture.identity.is_held_by_fingers())
    }

    // The scroller of such a gesture, or of one a containing context hands the steps of to this one.
    pub fn scroller_held_by_fingers(&self) -> Option<AsyncScrollNodeStableID> {
        if self.touchpad_gesture_end != TouchpadGestureEnd::WhenFingersLift {
            return None;
        }
        let latch = self.latch.as_ref()?;
        // The gesture here has every event; a context nested in another has only the steps handed to it, on its latch.
        let held_by_fingers = self.gesture.as_ref().map_or(
            latch.last_scroll_gesture_phase == ScrollGesturePhase::Ongoing,
            |gesture| gesture.identity.is_held_by_fingers(),
        );
        if !held_by_fingers {
            return None;
        }
        latch.stable_node_id
    }

    pub fn latch(
        &mut self,
        stable_node_id: AsyncScrollNodeStableID,
        scroll_gesture_phase: ScrollGesturePhase,
        gesture_id: Option<u64>,
    ) {
        self.latch = Some(Latch {
            gesture_id,
            stable_node_id: Some(stable_node_id),
            last_scroll_gesture_phase: scroll_gesture_phase,
            nested_context_id: None,
            momentum_has_sprung_the_scroller_back: false,
        });
    }

    pub fn latch_to_nested_context(
        &mut self,
        nested_context_id: CompositorContextId,
        scroller: Option<AsyncScrollNodeStableID>,
        scroll_gesture_phase: ScrollGesturePhase,
        gesture_id: Option<u64>,
    ) {
        self.latch = Some(Latch {
            gesture_id,
            stable_node_id: scroller,
            last_scroll_gesture_phase: scroll_gesture_phase,
            nested_context_id: Some(nested_context_id),
            momentum_has_sprung_the_scroller_back: false,
        });
    }

    pub fn latch_keeping_steps(&mut self, scroll_gesture_phase: ScrollGesturePhase, gesture_id: Option<u64>) {
        self.latch = Some(Latch {
            gesture_id,
            stable_node_id: None,
            last_scroll_gesture_phase: scroll_gesture_phase,
            nested_context_id: None,
            momentum_has_sprung_the_scroller_back: false,
        });
    }

    pub fn relatch_to(&mut self, stable_node_id: AsyncScrollNodeStableID) {
        if let Some(latch) = self.latch.as_mut()
            && latch.stable_node_id.is_some()
            && latch.nested_context_id.is_none()
        {
            latch.stable_node_id = Some(stable_node_id);
        }
    }

    pub fn nested_context_of_latch(&self) -> Option<CompositorContextId> {
        self.latch.as_ref()?.nested_context_id
    }

    pub fn clear_latch_of_gesture(&mut self, gesture_id: u64) {
        if self.has_latch_for_gesture(Some(gesture_id)) {
            self.latch = None;
        }
    }

    // This context's page dispatched a step of the gesture and did not cancel it, or canceled one.
    pub fn note_page_let_gesture_through(&mut self, gesture_id: u64) {
        self.gesture_the_page_let_through = Some(GestureThePageLetThrough {
            gesture_id,
            wheel_event_listener_state_generation: self.wheel_event_listener_state_generation,
        });
    }

    pub fn forget_page_let_gesture_through(&mut self, gesture_id: u64) {
        if self
            .gesture_the_page_let_through
            .as_ref()
            .is_some_and(|gesture| gesture.gesture_id == gesture_id)
        {
            self.gesture_the_page_let_through = None;
        }
    }

    // Whether the page let the gesture through, and its wheel event listeners are as they were then.
    pub fn page_let_gesture_through(&self, gesture_id: Option<u64>) -> bool {
        gesture_id.is_some_and(|id| {
            self.gesture_the_page_let_through.as_ref().is_some_and(|gesture| {
                gesture.gesture_id == id
                    && gesture.wheel_event_listener_state_generation == self.wheel_event_listener_state_generation
            })
        })
    }

    // Why this context's wheel event listeners hold the steps of a latched gesture back for the page, if they do. Once
    // the page let the gesture through, they only do so after they change.
    pub fn reason_to_hold_latched_steps_back(&self, gesture_id: Option<u64>) -> Option<InputRoutingDecision> {
        if self.page_let_gesture_through(gesture_id) {
            return None;
        }
        // Blocking listeners over all of the viewport hold every step back, and so do listeners that a context without a
        // current display list cannot place.
        if matches!(
            self.admission,
            WheelRoutingAdmission::Accepted | WheelRoutingAdmission::NoScrollNode
        ) {
            return None;
        }
        Some(input_routing_decision_for_wheel_step_not_admitted_by(self.admission))
    }

    pub fn momentum_of_latched_gesture_can_spring_scroller_back(&self) -> bool {
        self.latch
            .as_ref()
            .is_some_and(|latch| !latch.momentum_has_sprung_the_scroller_back)
    }

    pub fn set_momentum_of_latched_gesture_has_sprung_scroller_back(&mut self, has_sprung_back: bool) {
        if let Some(latch) = self.latch.as_mut() {
            latch.momentum_has_sprung_the_scroller_back = has_sprung_back;
        }
    }
}

fn max(a: f32, b: f32) -> f32 {
    if a < b { b } else { a }
}
