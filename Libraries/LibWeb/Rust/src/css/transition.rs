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
    pub before_change_value: *const crate::css::style_value::StyleValueData,
    pub after_change_value: *const crate::css::style_value::StyleValueData,
    pub current_value: *const crate::css::style_value::StyleValueData,
    pub existing_end_value: *const crate::css::style_value::StyleValueData,
    pub reversing_adjusted_start_value: *const crate::css::style_value::StyleValueData,
    pub has_matching_transition: bool,
    pub allow_discrete: bool,
    pub before_change_value_originates_from_current_color: bool,
    pub after_change_value_originates_from_current_color: bool,
    pub has_running_transition: bool,
    pub has_completed_transition: bool,
    pub delay: f64,
    pub duration: f64,
    pub old_timing_function_output: f64,
    pub old_reversing_shortening_factor: f64,
}

#[repr(C)]
pub struct FfiTransitionInput {
    pub context: crate::css::animation::FfiAnimationContext,
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

fn property_values_are_transitionable(
    context: &crate::css::animation::FfiAnimationContext,
    property_id: u16,
    old_value: *const crate::css::style_value::StyleValueData,
    new_value: *const crate::css::style_value::StyleValueData,
    allow_discrete: bool,
) -> bool {
    let animation_type = crate::css::property_metadata::property_animation_type(property_id);

    // https://drafts.csswg.org/css-transitions/#transitionable
    // When comparing the before-change style and after-change style for a given property, the property values are transitionable if they have an animation type that is neither not animatable nor discrete.
    if animation_type == crate::css::animation::ANIMATION_TYPE_NONE
        || !allow_discrete && animation_type == crate::css::animation::ANIMATION_TYPE_DISCRETE
    {
        return false;
    }
    if allow_discrete {
        return true;
    }

    let result = crate::css::animation::interpolate_value(
        Some(context),
        property_id,
        unsafe { &*old_value },
        unsafe { &*new_value },
        0.5,
    );
    assert!(result.handled);
    if !result.value.is_null() {
        unsafe { crate::css::style_value::release_style_value(result.value) };
        return true;
    }
    false
}

fn decide_transition(
    context: &crate::css::animation::FfiAnimationContext,
    input: &FfiTransitionPropertyInput,
) -> FfiTransitionAction {
    let values_equal = |first: *const crate::css::style_value::StyleValueData,
                        second: *const crate::css::style_value::StyleValueData| {
        assert!(!first.is_null());
        assert!(!second.is_null());
        let (first, second) = unsafe { (&*first, &*second) };
        std::ptr::eq(first, second) || first == second
    };
    let before_change_value_differs = input.has_matching_transition
        && !(input.before_change_value_originates_from_current_color
            && input.after_change_value_originates_from_current_color)
        && !values_equal(input.before_change_value, input.after_change_value);
    let existing_end_value_differs = input.has_matching_transition
        && (input.has_running_transition || input.has_completed_transition)
        && !values_equal(input.existing_end_value, input.after_change_value);
    let current_value_equals_after = input.has_matching_transition
        && input.has_running_transition
        && values_equal(input.current_value, input.after_change_value);
    let reversing_start_value_equals_after = input.has_matching_transition
        && input.has_running_transition
        && values_equal(input.reversing_adjusted_start_value, input.after_change_value);

    // https://drafts.csswg.org/css-transitions/#transition-combined-duration
    // Define the combined duration of the transition as the sum of max(matching transition duration, 0s) and the matching transition delay.
    let combined_duration = input.duration.max(0.0) + input.delay;
    let before_after_transitionable = input.has_matching_transition
        && before_change_value_differs
        && property_values_are_transitionable(
            context,
            input.property_id,
            input.before_change_value,
            input.after_change_value,
            input.allow_discrete,
        );
    let current_after_transitionable = current_value_equals_after
        || input.has_running_transition
            && existing_end_value_differs
            && property_values_are_transitionable(
                context,
                input.property_id,
                input.current_value,
                input.after_change_value,
                input.allow_discrete,
            );
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
        && before_change_value_differs
        && before_after_transitionable
        && (!input.has_completed_transition || existing_end_value_differs)
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
    if input.has_completed_transition && existing_end_value_differs {
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
    if input.has_running_transition && existing_end_value_differs {
        // 1. If the current value of the property in the running transition is equal to the value of the property in the after-change style, or if
        //    these two values are not transitionable, then implementations must cancel the running transition.
        if current_value_equals_after || !current_after_transitionable {
            action.kind = FfiTransitionActionKind::Cancel;
            return action;
        }

        // 2. Otherwise, if the combined duration is less than or equal to 0s, or if the current value of the property in the running transition is
        //    not transitionable with the value of the property in the after-change style, then implementations must cancel the running transition.
        if combined_duration <= 0.0 || !current_after_transitionable {
            action.kind = FfiTransitionActionKind::Cancel;
            return action;
        }

        // 3. Otherwise, if the reversing-adjusted start value of the running transition is the same as the value of the property in the after-change style
        //    (see the section on reversing of transitions for why these case exists),
        if reversing_start_value_equals_after {
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
/// C++ retains ownership of animation objects and executes the returned actions in order.
///
/// # Safety
/// `input` must point to a live value for the duration of the call. When the input contains
/// properties, `actions` must point at writable storage for `property_count` actions.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_decide_transitions(input: *const FfiTransitionInput, actions: *mut FfiTransitionAction) {
    crate::abort_on_panic(|| {
        crate::css::ffi_stats::rust_style_ffi_note_transition_decision();
        let input = unsafe { &*input };
        let properties = if input.property_count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(input.properties, input.property_count) }
        };
        for (index, property) in properties.iter().enumerate() {
            unsafe { actions.add(index).write(decide_transition(&input.context, property)) };
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn animation_context() -> crate::css::animation::FfiAnimationContext {
        let font_metrics = || crate::css::animation::FfiAnimationFontMetrics {
            font_size: 0.0,
            x_height: 0.0,
            cap_height: 0.0,
            zero_advance: 0.0,
            line_height: 0.0,
        };
        crate::css::animation::FfiAnimationContext {
            allow_discrete: false,
            current_color: std::ptr::null(),
            has_length_resolution_context: false,
            length_resolution_context: crate::css::animation::FfiAnimationLengthResolutionContext {
                viewport_width: 0.0,
                viewport_height: 0.0,
                font_metrics: font_metrics(),
                root_font_metrics: font_metrics(),
                font_metrics_depend_on_viewport_metrics: false,
                root_font_metrics_depend_on_viewport_metrics: false,
            },
            has_transform_reference_box: false,
            transform_reference_box_width: 0.0,
            transform_reference_box_height: 0.0,
        }
    }

    fn by_computed_value_property() -> u16 {
        (crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
            ..=crate::css::property_metadata::LAST_LONGHAND_PROPERTY_ID)
            .find(|property_id| {
                crate::css::property_metadata::property_animation_type(*property_id)
                    == crate::css::animation::ANIMATION_TYPE_BY_COMPUTED_VALUE
            })
            .unwrap()
    }

    fn input(
        before_change_value: &crate::css::style_value::StyleValueData,
        after_change_value: &crate::css::style_value::StyleValueData,
        current_value: &crate::css::style_value::StyleValueData,
    ) -> FfiTransitionPropertyInput {
        FfiTransitionPropertyInput {
            property_id: by_computed_value_property(),
            before_change_value,
            after_change_value,
            current_value,
            existing_end_value: std::ptr::null(),
            reversing_adjusted_start_value: std::ptr::null(),
            has_matching_transition: true,
            allow_discrete: false,
            before_change_value_originates_from_current_color: false,
            after_change_value_originates_from_current_color: false,
            has_running_transition: false,
            has_completed_transition: false,
            delay: 0.0,
            duration: 100.0,
            old_timing_function_output: 0.0,
            old_reversing_shortening_factor: 1.0,
        }
    }

    #[test]
    fn starts_an_initial_transition() {
        let before = crate::css::style_value::StyleValueData::Number { value: 0.0 };
        let after = crate::css::style_value::StyleValueData::Number { value: 1.0 };
        assert_eq!(
            decide_transition(&animation_context(), &input(&before, &after, &before)).kind,
            FfiTransitionActionKind::Start
        );
    }

    #[test]
    fn accepts_an_empty_transition_batch() {
        let input = FfiTransitionInput {
            context: animation_context(),
            properties: std::ptr::null(),
            property_count: 0,
        };
        unsafe { rust_decide_transitions(&raw const input, std::ptr::null_mut()) };
    }

    #[test]
    fn equal_nested_values_do_not_start_a_transition() {
        let nested_value = || {
            let number =
                std::sync::Arc::into_raw(std::sync::Arc::new(crate::css::style_value::StyleValueData::Number {
                    value: 0.5,
                }));
            crate::css::style_value::StyleValueData::OpacityValue {
                value: unsafe { crate::css::style_value::RetainedStyleValueData::from_retained_pointer(number) },
            }
        };
        let before = nested_value();
        let after = nested_value();
        assert_eq!(
            decide_transition(&animation_context(), &input(&before, &after, &before)).kind,
            FfiTransitionActionKind::None
        );
    }

    #[test]
    fn current_color_origins_are_equivalent() {
        let before = crate::css::style_value::StyleValueData::Number { value: 0.0 };
        let after = crate::css::style_value::StyleValueData::Number { value: 1.0 };
        let mut input = input(&before, &after, &before);
        input.before_change_value_originates_from_current_color = true;
        input.after_change_value_originates_from_current_color = true;
        assert_eq!(
            decide_transition(&animation_context(), &input).kind,
            FfiTransitionActionKind::None
        );
    }

    #[test]
    fn removes_a_completed_transition_before_replacement() {
        let before = crate::css::style_value::StyleValueData::Number { value: 0.0 };
        let after = crate::css::style_value::StyleValueData::Number { value: 1.0 };
        let mut input = input(&before, &after, &before);
        input.has_completed_transition = true;
        input.existing_end_value = &raw const before;
        assert_eq!(
            decide_transition(&animation_context(), &input).kind,
            FfiTransitionActionKind::RemoveAndStart
        );
    }

    #[test]
    fn adjusts_a_reversing_transition() {
        let before = crate::css::style_value::StyleValueData::Number { value: 0.0 };
        let after = crate::css::style_value::StyleValueData::Number { value: 1.0 };
        let current = crate::css::style_value::StyleValueData::Number { value: 0.5 };
        let mut input = input(&before, &after, &current);
        input.has_running_transition = true;
        input.existing_end_value = &raw const before;
        input.reversing_adjusted_start_value = &raw const after;
        input.delay = -20.0;
        input.old_timing_function_output = 0.25;
        input.old_reversing_shortening_factor = 0.5;
        let action = decide_transition(&animation_context(), &input);
        assert_eq!(action.kind, FfiTransitionActionKind::CancelRemoveAndStartReversing);
        assert_eq!(action.reversing_shortening_factor, 0.625);
        assert_eq!(action.delay, -12.5);
        assert_eq!(action.active_duration, 62.5);
    }
}
