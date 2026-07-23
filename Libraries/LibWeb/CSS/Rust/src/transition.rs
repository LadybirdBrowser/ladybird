/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS transition decisions.

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(u8)]
pub enum FfiTransitionActionKind {
    None,
    Remove,
    Cancel,
    Start,
    RemoveAndStart,
    CancelRemoveAndStartReversing,
    CancelRemoveAndStartInterrupted,
}

#[repr(C)]
pub struct FfiTransitionPropertyInput {
    pub property_id: u16,
    pub has_matching_transition: bool,
    pub before_change_value_differs: bool,
    pub before_after_transitionable: bool,
    pub has_running_transition: bool,
    pub has_completed_transition: bool,
    pub existing_end_value_differs: bool,
    pub current_value_equals_after: bool,
    pub current_after_transitionable: bool,
    pub reversing_start_value_equals_after: bool,
    pub delay: f64,
    pub duration: f64,
    pub old_timing_function_output: f64,
    pub old_reversing_shortening_factor: f64,
}

#[repr(C)]
pub struct FfiTransitionInput {
    pub properties: *const FfiTransitionPropertyInput,
    pub property_count: usize,
}

#[repr(C)]
pub struct FfiTransitionAction {
    pub property_id: u16,
    pub kind: FfiTransitionActionKind,
    pub delay: f64,
    pub active_duration: f64,
    pub reversing_shortening_factor: f64,
}

#[repr(C)]
pub struct FfiTransitionCallbacks {
    pub context: *mut std::ffi::c_void,
    pub apply_actions:
        unsafe extern "C" fn(context: *mut std::ffi::c_void, actions: *const FfiTransitionAction, count: usize),
}

fn decide_transition(input: &FfiTransitionPropertyInput) -> FfiTransitionAction {
    // https://drafts.csswg.org/css-transitions/#transition-combined-duration
    // Define the combined duration of the transition as the sum of max(matching transition duration, 0s) and the matching transition delay.
    let combined_duration = input.duration.max(0.0) + input.delay;
    let mut action = FfiTransitionAction {
        property_id: input.property_id,
        kind: FfiTransitionActionKind::None,
        delay: input.delay,
        active_duration: input.duration,
        reversing_shortening_factor: 1.0,
    };

    // https://drafts.csswg.org/css-transitions/#starting
    // For each element and property, the implementation must act as follows:

    // 1. If all of the following are true:
    // - the element does not have a running transition for the property,
    // - there is a matching transition-property value, and
    // - the before-change style is different from the after-change style for that property, and the values for the property are transitionable,
    // - the element does not have a completed transition for the property or the end value of the completed transition is different from the
    //   after-change style for the property,
    // - the combined duration is greater than 0s,
    if !input.has_running_transition
        && input.has_matching_transition
        && input.before_change_value_differs
        && input.before_after_transitionable
        && (!input.has_completed_transition || input.existing_end_value_differs)
        && combined_duration > 0.0
    {
        // then implementations must remove the completed transition (if present) from the set of completed transitions
        // and start a transition whose:
        // - start time is the time of the style change event plus the matching transition delay,
        // - end time is the start time plus the matching transition duration,
        // - start value is the value of the transitioning property in the before-change style,
        // - end value is the value of the transitioning property in the after-change style,
        // - reversing-adjusted start value is the same as the start value, and
        // - reversing shortening factor is 1.
        action.kind = if input.has_completed_transition {
            FfiTransitionActionKind::RemoveAndStart
        } else {
            FfiTransitionActionKind::Start
        };
        return action;
    }

    // 2. Otherwise, if the element has a completed transition for the property and the end value of the completed transition is different from the
    //    after-change style for the property, then implementations must remove the completed transition from the set of completed transitions.
    if input.has_completed_transition && input.existing_end_value_differs {
        action.kind = FfiTransitionActionKind::Remove;
        return action;
    }

    // 3. If the element has a running transition or completed transition for the property, and there is not a matching transition-property value,
    //    then implementations must cancel the running transition or remove the completed transition from the set of completed transitions.
    if !input.has_matching_transition {
        action.kind = if input.has_running_transition {
            FfiTransitionActionKind::Cancel
        } else if input.has_completed_transition {
            FfiTransitionActionKind::Remove
        } else {
            FfiTransitionActionKind::None
        };
        return action;
    }

    // 4. If the element has a running transition for the property, there is a matching transition-property value, and the end value of the running
    //    transition is not equal to the value of the property in the after-change style, then:
    if input.has_running_transition && input.existing_end_value_differs {
        // 1. If the current value of the property in the running transition is equal to the value of the property in the after-change style, or if
        //    these two values are not transitionable, then implementations must cancel the running transition.
        if input.current_value_equals_after || !input.current_after_transitionable {
            action.kind = FfiTransitionActionKind::Cancel;
            return action;
        }

        // 2. Otherwise, if the combined duration is less than or equal to 0s, or if the current value of the property in the running transition is
        //    not transitionable with the value of the property in the after-change style, then implementations must cancel the running transition.
        if combined_duration <= 0.0 || !input.current_after_transitionable {
            action.kind = FfiTransitionActionKind::Cancel;
            return action;
        }

        // 3. Otherwise, if the reversing-adjusted start value of the running transition is the same as the value of the property in the after-change style
        //    (see the section on reversing of transitions for why these case exists),
        if input.reversing_start_value_equals_after {
            // implementations must cancel the running transition and start a new transition whose:
            // - reversing-adjusted start value is the end value of the running transition,
            // - reversing shortening factor is the absolute value, clamped to the range [0, 1], of the sum of:
            //   1. the output of the timing function of the old transition at the time of the style change event,
            //      times the reversing shortening factor of the old transition
            //   2. 1 minus the reversing shortening factor of the old transition.
            let term_1 = input.old_timing_function_output * input.old_reversing_shortening_factor;
            let term_2 = 1.0 - input.old_reversing_shortening_factor;
            let reversing_shortening_factor = (term_1 + term_2).abs().clamp(0.0, 1.0);
            action.kind = FfiTransitionActionKind::CancelRemoveAndStartReversing;
            action.reversing_shortening_factor = reversing_shortening_factor;
            action.delay = if input.delay >= 0.0 {
                input.delay
            } else {
                reversing_shortening_factor * input.delay
            };
            // - start time is the time of the style change event plus:
            //   1. if the matching transition delay is nonnegative, the matching transition delay, or
            //   2. if the matching transition delay is negative, the product of the new transition’s reversing shortening factor and the matching transition delay,
            // - end time is the start time plus the product of the matching transition duration and the new transition’s reversing shortening factor,
            // - start value is the current value of the property in the running transition,
            // - end value is the value of the property in the after-change style,
            action.active_duration = input.duration * reversing_shortening_factor;
            return action;
        }

        // 4. Otherwise,
        // implementations must cancel the running transition and start a new transition whose:
        // - start time is the time of the style change event plus the matching transition delay,
        // - end time is the start time plus the matching transition duration,
        // - start value is the current value of the property in the running transition,
        // - end value is the value of the property in the after-change style,
        // - reversing-adjusted start value is the same as the start value, and
        // - reversing shortening factor is 1.
        action.kind = FfiTransitionActionKind::CancelRemoveAndStartInterrupted;
    }

    action
}

/// Run the CSS Transitions decision algorithm for every supplied property.
///
/// The callback is invoked exactly once. C++ retains ownership of animation objects and executes
/// the returned actions in order.
///
/// # Safety
/// `input` and `callbacks` must point to live values for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_decide_transitions(
    input: *const FfiTransitionInput,
    callbacks: *const FfiTransitionCallbacks,
) {
    crate::abort_on_panic(|| {
        crate::ffi_stats::rust_style_ffi_note_transition_decision();
        let input = unsafe { &*input };
        let callbacks = unsafe { &*callbacks };
        let properties = unsafe { std::slice::from_raw_parts(input.properties, input.property_count) };
        let actions = properties.iter().map(decide_transition).collect::<Vec<_>>();
        crate::ffi_stats::rust_style_ffi_note_transition_action_batch();
        unsafe { (callbacks.apply_actions)(callbacks.context, actions.as_ptr(), actions.len()) };
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn input() -> FfiTransitionPropertyInput {
        FfiTransitionPropertyInput {
            property_id: 42,
            has_matching_transition: true,
            before_change_value_differs: true,
            before_after_transitionable: true,
            has_running_transition: false,
            has_completed_transition: false,
            existing_end_value_differs: true,
            current_value_equals_after: false,
            current_after_transitionable: true,
            reversing_start_value_equals_after: false,
            delay: 0.0,
            duration: 100.0,
            old_timing_function_output: 0.0,
            old_reversing_shortening_factor: 1.0,
        }
    }

    #[test]
    fn starts_an_initial_transition() {
        assert_eq!(decide_transition(&input()).kind, FfiTransitionActionKind::Start);
    }

    #[test]
    fn removes_a_completed_transition_before_replacement() {
        let mut input = input();
        input.has_completed_transition = true;
        assert_eq!(decide_transition(&input).kind, FfiTransitionActionKind::RemoveAndStart);
    }

    #[test]
    fn adjusts_a_reversing_transition() {
        let mut input = input();
        input.has_running_transition = true;
        input.reversing_start_value_equals_after = true;
        input.delay = -20.0;
        input.old_timing_function_output = 0.25;
        input.old_reversing_shortening_factor = 0.5;
        let action = decide_transition(&input);
        assert_eq!(action.kind, FfiTransitionActionKind::CancelRemoveAndStartReversing);
        assert_eq!(action.reversing_shortening_factor, 0.625);
        assert_eq!(action.delay, -12.5);
        assert_eq!(action.active_duration, 62.5);
    }
}
