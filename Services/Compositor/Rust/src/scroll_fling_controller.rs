/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Synthesizes the momentum of a touchpad flick as wheel events, for platforms that send no momentum scroll events.

use crate::input_event::wheel_tick_120ths_from_pixel_delta;
use libcompositing_rust::scrolling::scroll_fling::{ScrollFling, ScrollFlingBooster, ScrollVelocityTracker};
use libcompositing_rust::scrolling::{
    DevicePixelPoint, KeyModifier, MouseButton, MouseEvent, MouseEventType, PointerInputData, ScrollGesturePhase,
    WheelDeltaPrecision, WheelGestureMark,
};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::FloatPoint;

#[derive(Default)]
pub struct ScrollFlingController {
    velocity_tracker: ScrollVelocityTracker,
    booster: ScrollFlingBooster,
    active_fling: Option<ActiveFling>,
}

struct ActiveFling {
    fling: ScrollFling,
    // The fling steps use the position, modifiers and gesture of the gesture end event.
    position: DevicePixelPoint,
    screen_position: DevicePixelPoint,
    modifiers: KeyModifier,
    wheel_gesture_mark: Option<WheelGestureMark>,
    ends_on_next_step: bool,
}

fn is_touchpad_step(event: &MouseEvent) -> bool {
    event.wheel_delta_precision == WheelDeltaPrecision::Precise
        && event.scroll_gesture_phase == ScrollGesturePhase::Ongoing
}

impl ScrollFlingController {
    pub fn note_wheel_event(&mut self, event: &MouseEvent, now: MonotonicTime) {
        if is_touchpad_step(event) {
            self.velocity_tracker.add_step(
                FloatPoint {
                    x: event.wheel_delta_x as f32,
                    y: event.wheel_delta_y as f32,
                },
                now,
            );
            return;
        }
        self.velocity_tracker.reset();
        // Other wheel input cancels the boost of the next fling.
        if event.scroll_gesture_phase != ScrollGesturePhase::Ended {
            self.booster.reset();
        }
    }

    // Starts a fling if the gesture that the event ends was a flick.
    pub fn start_if_flicked(&mut self, gesture_end_event: &MouseEvent, now: MonotonicTime) -> bool {
        assert!(gesture_end_event.scroll_gesture_phase == ScrollGesturePhase::Ended);
        let velocity = self.velocity_tracker.velocity_at_lift(now);
        self.velocity_tracker.reset();
        let Some(velocity) = velocity else {
            return false;
        };

        let Some(fling) = ScrollFling::start(self.booster.boosted_velocity(velocity, now), now) else {
            return false;
        };
        self.active_fling = Some(ActiveFling {
            fling,
            position: gesture_end_event.position,
            screen_position: gesture_end_event.screen_position,
            modifiers: gesture_end_event.modifiers,
            wheel_gesture_mark: gesture_end_event.wheel_gesture_mark.clone(),
            ends_on_next_step: false,
        });
        true
    }

    pub fn is_active(&self) -> bool {
        self.active_fling.is_some()
    }

    // The next momentum step of the fling, or the end of its gesture once the fling has run out or was told to end.
    pub fn take_step(&mut self, frame_time: MonotonicTime) -> Option<MouseEvent> {
        let active = self.active_fling.as_mut()?;
        if active.ends_on_next_step {
            return self.end();
        }

        let Some(delta) = active.fling.step(frame_time) else {
            return self.end();
        };
        if delta.is_zero() {
            return None;
        }
        Some(self.event_of_active_fling(delta, ScrollGesturePhase::Momentum))
    }

    pub fn end_on_next_step(&mut self) {
        assert!(self.active_fling.is_some());
        self.active_fling.as_mut().unwrap().ends_on_next_step = true;
    }

    // The end of the gesture of the fling, if one is active.
    pub fn end(&mut self) -> Option<MouseEvent> {
        self.active_fling.as_ref()?;

        // WebContent holds the gesture open while momentum events arrive. The end of the gesture releases it.
        let event = self.event_of_active_fling(FloatPoint::default(), ScrollGesturePhase::Ended);
        self.active_fling = None;
        Some(event)
    }

    // Ends the fling before a wheel event from a new gesture or other input.
    pub fn interrupt(&mut self, event: &MouseEvent, now: MonotonicTime) -> Option<MouseEvent> {
        let active = self.active_fling.as_ref()?;

        // A new touchpad step stops the fling. The next flick can add the velocity of this fling.
        if is_touchpad_step(event) {
            self.booster.fling_was_interrupted(active.fling.velocity_at(now), now);
        }
        self.end()
    }

    fn event_of_active_fling(&self, delta: FloatPoint, phase: ScrollGesturePhase) -> MouseEvent {
        assert!(self.active_fling.is_some());
        let active = self.active_fling.as_ref().unwrap();
        MouseEvent {
            r#type: MouseEventType::MouseWheel,
            position: active.position,
            screen_position: active.screen_position,
            button: MouseButton::None,
            buttons: MouseButton::None,
            modifiers: active.modifiers,
            wheel_delta_x: delta.x as f64,
            wheel_delta_y: delta.y as f64,
            wheel_tick_120ths_x: wheel_tick_120ths_from_pixel_delta(delta.x as f64, WheelDeltaPrecision::Precise),
            wheel_tick_120ths_y: wheel_tick_120ths_from_pixel_delta(delta.y as f64, WheelDeltaPrecision::Precise),
            wheel_delta_precision: WheelDeltaPrecision::Precise,
            scroll_gesture_phase: phase,
            click_count: 0,
            async_scroll_performed_default_action: false,
            // The IDs of UI input events start at 1. Thus, ID 0 does not match a UI event.
            id: 0,
            scrollbar_dragged_by_compositor: None,
            pointer_data: PointerInputData { properties: None },
            wheel_gesture_mark: active.wheel_gesture_mark.clone(),
        }
    }
}
