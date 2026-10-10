/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS transition decisions.

use crate::css::style_value::StyleValueData;

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(u8)]
pub enum FfiTransitionActionKind {
    None,
    Remove,
    Cancel,
    Start,
    RemoveAndStart,
    CancelRemoveAndStart,
}

/// A transition the target holds for a property, running or completed, as of the style change event.
#[repr(C)]
pub struct FfiExistingTransition {
    pub property_id: u16,
    /// Whether the transition is running rather than completed.
    pub running: bool,
    pub end_value: *const StyleValueData,
    pub reversing_adjusted_start_value: *const StyleValueData,
    /// The output of the transition's timing function at the time of the style change event, where it is running.
    pub timing_function_output: f64,
    pub reversing_shortening_factor: f64,
}

#[repr(C)]
pub struct FfiTransitionInput {
    /// The transitions the target holds, each for a different property.
    pub existing_transitions: *const FfiExistingTransition,
    pub existing_transition_count: usize,
    /// The target's style node.
    pub target_node: u32,
    /// The target's pseudo-element kind, or `u8::MAX` for an element.
    pub target_pseudo_kind: u8,
    /// The slot of the row of the box of the target's element, a `NodeSlotId`'s index, invalid where it has none.
    pub element_box_slot: u32,
}

/// What a transition step does to one property's transitions. Where it starts a transition, the values and timing
/// function the transition runs with, which the records the step decided over and the existing transition hold;
/// null otherwise.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiTransitionAction {
    pub property_id: u16,
    pub kind: FfiTransitionActionKind,
    pub delay: f64,
    pub active_duration: f64,
    pub reversing_shortening_factor: f64,
    pub start_value: *const StyleValueData,
    pub end_value: *const StyleValueData,
    pub reversing_adjusted_start_value: *const StyleValueData,
    pub timing_function: *const StyleValueData,
}

/// One property a transition step decides over: its matching `transition-property` entry, if any, the transition the
/// target holds for it, if any, and the values the decision compares.
#[derive(Clone, Copy)]
struct TransitionProperty {
    property_id: u16,
    before_change_value: *const StyleValueData,
    after_change_value: *const StyleValueData,
    current_value: *const StyleValueData,
    existing_end_value: *const StyleValueData,
    reversing_adjusted_start_value: *const StyleValueData,
    timing_function: *const StyleValueData,
    has_matching_transition: bool,
    allow_discrete: bool,
    has_running_transition: bool,
    has_completed_transition: bool,
    delay: f64,
    duration: f64,
    old_timing_function_output: f64,
    old_reversing_shortening_factor: f64,
}

impl TransitionProperty {
    fn new(property_id: u16, entry: Option<&TransitionEntry>, existing: Option<&FfiExistingTransition>) -> Self {
        Self {
            property_id,
            before_change_value: std::ptr::null(),
            after_change_value: std::ptr::null(),
            current_value: std::ptr::null(),
            existing_end_value: existing.map_or(std::ptr::null(), |existing| existing.end_value),
            reversing_adjusted_start_value: existing
                .map_or(std::ptr::null(), |existing| existing.reversing_adjusted_start_value),
            timing_function: entry.map_or(std::ptr::null(), |entry| entry.timing_function),
            has_matching_transition: entry.is_some(),
            allow_discrete: entry
                .is_some_and(|entry| entry.behavior == crate::css::css_enums::transition_behavior::ALLOW_DISCRETE),
            has_running_transition: existing.is_some_and(|existing| existing.running),
            has_completed_transition: existing.is_some_and(|existing| !existing.running),
            delay: entry.map_or(0.0, |entry| entry.delay),
            duration: entry.map_or(0.0, |entry| entry.duration),
            old_timing_function_output: existing.map_or(0.0, |existing| existing.timing_function_output),
            old_reversing_shortening_factor: existing.map_or(1.0, |existing| existing.reversing_shortening_factor),
        }
    }
}

impl FfiTransitionAction {
    /// Has the action start a transition of `property` from `start_value` to its after-change value.
    fn start(
        mut self,
        kind: FfiTransitionActionKind,
        property: &TransitionProperty,
        start_value: *const StyleValueData,
        reversing_adjusted_start_value: *const StyleValueData,
    ) -> Self {
        self.kind = kind;
        self.start_value = start_value;
        self.end_value = property.after_change_value;
        self.reversing_adjusted_start_value = reversing_adjusted_start_value;
        self.timing_function = property.timing_function;
        self
    }
}

fn property_values_are_transitionable(
    context: &crate::css::animation::FfiAnimationContext,
    property_id: u16,
    old_value: *const StyleValueData,
    new_value: *const StyleValueData,
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

fn values_equal(first: *const StyleValueData, second: *const StyleValueData) -> bool {
    assert!(!first.is_null());
    assert!(!second.is_null());
    let (first, second) = unsafe { (&*first, &*second) };
    std::ptr::eq(first, second) || first == second
}

fn decide_transition(
    context: &crate::css::animation::FfiAnimationContext,
    input: &TransitionProperty,
    values_originate_from_current_color: bool,
) -> FfiTransitionAction {
    let before_change_value_differs = input.has_matching_transition
        && !values_originate_from_current_color
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
        start_value: std::ptr::null(),
        end_value: std::ptr::null(),
        reversing_adjusted_start_value: std::ptr::null(),
        timing_function: std::ptr::null(),
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
        let kind = if input.has_completed_transition {
            FfiTransitionActionKind::RemoveAndStart
        } else {
            FfiTransitionActionKind::Start
        };
        return action.start(kind, input, input.before_change_value, input.before_change_value);
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
            return action.start(
                FfiTransitionActionKind::CancelRemoveAndStart,
                input,
                input.current_value,
                input.existing_end_value,
            );
        }

        // 4. Otherwise,
        // implementations must cancel the running transition and start a new transition whose:
        // - start time is the time of the style change event plus the matching transition delay,
        // - end time is the start time plus the matching transition duration,
        // - start value is the current value of the property in the running transition,
        // - end value is the value of the property in the after-change style,
        // - reversing-adjusted start value is the same as the start value, and
        // - reversing shortening factor is 1.
        return action.start(
            FfiTransitionActionKind::CancelRemoveAndStart,
            input,
            input.current_value,
            input.current_value,
        );
    }

    action
}

fn value_is_current_color(value: *const StyleValueData) -> bool {
    matches!(
        unsafe { value.as_ref() },
        Some(StyleValueData::Keyword { keyword })
            if *keyword == crate::css::style_compute::keyword::CURRENTCOLOR
    )
}

fn computed_value(
    table: &crate::css::computed_longhand_table::ComputedLonghandTable,
    overlay: Option<&crate::css::animated_overlay::AnimatedOverlay>,
    property_id: u16,
) -> *const StyleValueData {
    if let Some(entry) = overlay.and_then(|overlay| overlay.get(property_id))
        && crate::css::animated_overlay::overlay_wins(entry, table.is_important(property_id))
    {
        return entry.value_pointer();
    }
    table
        .get(property_id)
        .expect("a transition property must have a computed value")
        .pointer()
}

/// Whether the records `first` and `second` compute `property_id` to different values, without their animations.
pub(crate) fn records_compute_differently(
    engine: &crate::css::style::StyleEngine,
    first: u64,
    second: u64,
    property_id: u16,
) -> bool {
    let value = |record| {
        let view = engine.style_record_view(record)?;
        // SAFETY: A live record's table lives as long as the record.
        let table = unsafe { view.longhand_table.as_ref() }?;
        Some(table.get(property_id)?.pointer())
    };
    match (value(first), value(second)) {
        (Some(first), Some(second)) => !values_equal(first, second),
        _ => true,
    }
}

fn originates_from_current_color(
    table: &crate::css::computed_longhand_table::ComputedLonghandTable,
    property_id: u16,
) -> bool {
    let value = table
        .retained_inheritance_dependent_values()
        .find_map(|(property, value)| (property == property_id).then_some(value))
        .filter(|value| crate::css::style_value::retained_value_depends_on_current_color(value))
        .or_else(|| table.get(property_id))
        .expect("a transition property must have a computed value");
    crate::css::style_value::retained_value_depends_on_current_color(value)
}

fn prepare_transition_values(
    before_style: (
        &crate::css::computed_longhand_table::ComputedLonghandTable,
        Option<&crate::css::animated_overlay::AnimatedOverlay>,
    ),
    after_table: &crate::css::computed_longhand_table::ComputedLonghandTable,
    after_overlay: Option<&crate::css::animated_overlay::AnimatedOverlay>,
    inherited_animation: Option<crate::css::style::InheritedAnimatedValue<'_>>,
    property: &mut TransitionProperty,
) -> bool {
    let (before_table, before_overlay) = before_style;
    property.before_change_value = computed_value(before_table, before_overlay, property.property_id);
    if !property.has_matching_transition {
        return false;
    }
    // NB: A record the engine derived holds an inherited animated value in its table, where one
    //     the host computed holds the ancestor's base value and an inherited overlay entry: the
    //     ancestor's entry and base value stand in for that entry and table value.
    if let Some(entry) = after_overlay
        .and_then(|overlay| overlay.get(property.property_id))
        .filter(|entry| !entry.result_of_transition && !entry.post_compute_adjustment)
        .or(match inherited_animation {
            Some(crate::css::style::InheritedAnimatedValue::Animation(entry)) => Some(entry),
            _ => None,
        })
    {
        property.before_change_value = entry.value_pointer();
        property.after_change_value = entry.value_pointer();
        let originates_from_current_color = value_is_current_color(entry.value_pointer());
        if property.has_running_transition {
            property.current_value = computed_value(after_table, after_overlay, property.property_id);
        }
        return originates_from_current_color;
    } else if let Some(crate::css::style::InheritedAnimatedValue::BeneathTransitions(base_value)) = inherited_animation
    {
        property.after_change_value = std::ptr::from_ref(base_value);
    } else {
        property.after_change_value = computed_value(after_table, None, property.property_id);
    }
    if property.has_running_transition {
        property.current_value = computed_value(after_table, after_overlay, property.property_id);
    }
    originates_from_current_color(before_table, property.property_id)
        && originates_from_current_color(after_table, property.property_id)
}

/// A transition step's question to the style engine: what the step does to the transitions of its target as its style
/// changes from the record `before` to the record `after`, the record the target installed. The transitions resolve
/// their lengths against `after`.
pub(crate) struct TransitionDecision {
    pub(crate) before: u64,
    pub(crate) after: u64,
    /// The target's style node, where the target is an element: only an element's own record inherits from its
    /// inheritance parent.
    pub(crate) element: Option<crate::css::style::tree::StyleNodeID>,
}

impl TransitionDecision {
    /// Runs the CSS Transitions decision algorithm for every property the after-change style's `transition-*` values
    /// name, then for every property of an `existing` transition they do not name, and answers one action per property
    /// in that order. The transitions resolve their transforms against `reference_box`, the transform reference box of
    /// the box of the target's element, where it was laid out.
    pub(crate) fn decide(
        self,
        engine: &crate::css::style::StyleEngine,
        reference_box: Option<crate::css::css_pixels::CssPixelRect>,
        existing: &[FfiExistingTransition],
    ) -> Vec<FfiTransitionAction> {
        // SAFETY: A live record's table lives as long as the record.
        let Some(after_table) = engine
            .style_record_view(self.after)
            .and_then(|after| unsafe { after.longhand_table.as_ref() })
        else {
            debug_assert!(false, "the records of a transition step carry longhand tables");
            return Vec::new();
        };
        // A declaration whose delay and duration are each the single value 0s starts nothing, so it matters only to a
        // target holding a transition it could cancel.
        if existing.is_empty() && crate::css::style_compute::transition_delay_and_duration_are_single_zero(after_table)
        {
            return Vec::new();
        }
        let entries = crate::css::style_compute::transition_entries(after_table);
        self.decide_entries(engine, reference_box, &entries, existing)
    }

    /// As `decide`, for the after-change style's `entries`.
    fn decide_entries(
        self,
        engine: &crate::css::style::StyleEngine,
        reference_box: Option<crate::css::css_pixels::CssPixelRect>,
        entries: &[TransitionEntry],
        existing: &[FfiExistingTransition],
    ) -> Vec<FfiTransitionAction> {
        self.decide_over(engine, reference_box, None, entries, existing, &[])
    }

    /// As `decide_entries`, with `after_overlay` in place of the after-change record's overlay where it is given: the
    /// current values of the target's running transitions, which the record holds none of.
    fn decide_over(
        self,
        engine: &crate::css::style::StyleEngine,
        reference_box: Option<crate::css::css_pixels::CssPixelRect>,
        after_overlay: Option<&crate::css::animated_overlay::AnimatedOverlay>,
        entries: &[TransitionEntry],
        existing: &[FfiExistingTransition],
        ended: &[u16],
    ) -> Vec<FfiTransitionAction> {
        let Self { before, after, element } = self;
        let length_resolution_context = engine.transition_length_resolution_context(after);
        let (Some(before), Some(after)) = (engine.style_record_view(before), engine.style_record_view(after)) else {
            debug_assert!(false, "the records a transition step decides over remain live");
            return Vec::new();
        };
        // SAFETY: A live record's table and overlay live as long as the record.
        let (Some(before_table), before_overlay, Some(after_table), record_after_overlay) = (unsafe {
            (
                before.longhand_table.as_ref(),
                before.animated_overlay.as_ref(),
                after.longhand_table.as_ref(),
                after.animated_overlay.as_ref(),
            )
        }) else {
            debug_assert!(false, "the records of a transition step carry longhand tables");
            return Vec::new();
        };
        let after_overlay = after_overlay.or(record_after_overlay);
        let mut context = crate::css::animation::FfiAnimationContext {
            allow_discrete: false,
            current_color: after_table
                .effective_value(after_overlay, crate::css::property_metadata::property_id::COLOR, true)
                .value
                .cast(),
            has_length_resolution_context: length_resolution_context.is_some(),
            length_resolution_context: length_resolution_context.unwrap_or_default(),
            has_transform_reference_box: false,
            transform_reference_box_width: 0.0,
            transform_reference_box_height: 0.0,
        };
        context.set_transform_reference_box(reference_box);
        let existing_for = |property_id| existing.iter().find(|existing| existing.property_id == property_id);
        let matched = entries
            .iter()
            .map(|entry| (entry.property_id, Some(entry), existing_for(entry.property_id)));
        let unmatched = existing
            .iter()
            .filter(|existing| entries.iter().all(|entry| entry.property_id != existing.property_id))
            .map(|existing| (existing.property_id, None, Some(existing)));
        matched
            .chain(unmatched)
            .map(|(property_id, entry, existing)| {
                let mut property = TransitionProperty::new(property_id, entry, existing);
                let inherited_animation = element
                    .filter(|_| {
                        after_overlay
                            .and_then(|overlay| overlay.get(property_id))
                            .is_none_or(|entry| !entry.inherited)
                    })
                    .and_then(|element| engine.inherited_animated_value(element, after_table, property_id));
                let values_originate_from_current_color = prepare_transition_values(
                    (before_table, before_overlay),
                    after_table,
                    after_overlay,
                    inherited_animation,
                    &mut property,
                );
                // A transition that ended by the time of the style change event no longer moves the before-change
                // style, whose record may hold a value of it from before it ended.
                if ended.contains(&property_id)
                    && std::ptr::eq(
                        property.before_change_value,
                        computed_value(before_table, before_overlay, property_id),
                    )
                {
                    property.before_change_value = computed_value(before_table, None, property_id);
                }
                decide_transition(&context, &property, values_originate_from_current_color)
            })
            .collect()
    }
}

/// What the transition step a style transaction's row owes decides, where nothing but the engine's records decides it
/// (see [`DecidedTransitionStep::of_row`]).
pub(crate) struct RowTransitions {
    pub(crate) node: crate::css::style::tree::StyleNodeID,
    pub(crate) before: u64,
    pub(crate) after: u64,
    actions: Vec<FfiTransitionAction>,
    /// The time of the style change event, in the document timeline's milliseconds, where the caller knew it.
    at: Option<f64>,
    /// The transitions the element runs, the host's or those a lane's hover started, as they stood when the step was
    /// decided.
    running: Vec<RunningTransitionAt>,
    /// The host's transitions the earlier steps of a lane's hover on the element saw.
    host_seen_before: HostTransitionsSeen,
    /// The values of the running transitions over the after-change style, which the decision read their current values
    /// from, kept until the values it compared are retained.
    #[expect(dead_code, reason = "the actions point into it")]
    after_overlay: Option<Box<crate::css::animated_overlay::AnimatedOverlay>>,
}

/// A transition an element runs, as it stands at the time a step decides at: one the host runs, as the engine's
/// description of its effect says, or one a lane's hover started, as the lane runs it.
struct RunningTransitionAt {
    /// The identity and generation of its effect, which a sample of it composes: the host's, or the lane's own.
    identity: u64,
    generation: u64,
    /// The identity of the host's effect, where the host runs it.
    host_identity: Option<u64>,
    property_id: u16,
    /// The key its keyframes are sampled at then, or none once it has ended.
    key: Option<f64>,
    /// Its local time then, which it runs on from.
    local_time: f64,
    timing: crate::css::style::animations::EffectTiming,
    start_value: *const StyleValueData,
    end_value: *const StyleValueData,
    reversing_adjusted_start_value: *const StyleValueData,
    reversing_shortening_factor: f64,
    /// The timing function it eases with, where a lane's hover started it: the host keeps that of its own.
    timing_function: *const StyleValueData,
    /// The time it started at, in the document timeline's milliseconds: that of the style change event that started it,
    /// where a lane's hover started it.
    started_at: f64,
}

/// The key a published keyframe at 100% sits at: `KeyframeEffect::AnimationKeyFrameKeyScaleFactor` times 100.
const FULL_KEY: f64 = 100.0 * 1000.0;

/// The transitions the host runs on the element `node` names, as they stand at `at`, the time a step decides at, in
/// the document timeline's milliseconds: every animation of the element must be one the engine describes as such.
fn host_transitions_at(
    engine: &crate::css::style::StyleEngine,
    node: crate::css::style::tree::StyleNodeID,
    at: f64,
) -> Result<Vec<RunningTransitionAt>, &'static str> {
    use crate::css::style::animations::AnimationTimelineSamples;
    let effects = engine.element_animation_effects(node, 0);
    if effects.is_empty() {
        return Err("an element with animations the host has not described");
    }
    let samples = AnimationTimelineSamples::at_tick(at, &[]);
    effects
        .iter()
        .filter_map(|effect| {
            if !effect.is_transition {
                return Some(Err("an element with animations"));
            }
            let Some(timing) = effect.timing.clone() else {
                return Some(Err("a transition the host has not sampled"));
            };
            // An idle transition, one with neither a start time nor a hold time, runs nothing.
            if timing.timing.decidable && !timing.timing.has_start_time && !timing.timing.has_hold_time {
                return None;
            }
            Some(host_transition_at(effect, timing, samples, at))
        })
        .collect()
}

/// The transition the host runs of the effect `effect`, with `timing`, as it stands with its timeline at `samples`, the
/// time `at` a step decides at. A transition whose play is pending holds its start until the host's next rendering
/// update resolves its start time: it runs from the time the step decides at, which the host takes in. One that script
/// paused, or set to run at another rate, the lane cannot run on as the host does.
fn host_transition_at(
    effect: &crate::css::style::effect_descriptions::PublishedEffect,
    timing: crate::css::style::animations::EffectTiming,
    samples: crate::css::style::animations::AnimationTimelineSamples<'_>,
    at: f64,
) -> Result<RunningTransitionAt, &'static str> {
    if timing.timing.paused {
        return Err("a paused transition");
    }
    if timing.timing.playback_rate != 1.0 {
        return Err("a transition that runs at another rate");
    }
    let reversing = effect
        .reversing
        .as_ref()
        .ok_or("a transition the host has not described")?;
    let key = timing
        .key_at(samples)
        .ok_or("a transition whose timing the engine cannot decide")?;
    let local_time = timing
        .local_time_at(samples)
        .ok_or("a transition whose timing the engine cannot decide")?;
    let (property_id, start_value, end_value) =
        transition_values(effect).ok_or("a transition the host has not described")?;
    Ok(RunningTransitionAt {
        identity: effect.identity,
        generation: effect.generation,
        host_identity: Some(effect.identity),
        property_id,
        key,
        local_time,
        timing,
        start_value,
        end_value,
        reversing_adjusted_start_value: reversing.adjusted_start_value.pointer(),
        reversing_shortening_factor: reversing.shortening_factor,
        timing_function: std::ptr::null(),
        started_at: at - local_time,
    })
}

/// The property a transition's effect animates, and the values it runs between, where its two keyframes each declare a
/// value of the same property.
fn transition_values(
    effect: &crate::css::style::effect_descriptions::PublishedEffect,
) -> Option<(u16, *const StyleValueData, *const StyleValueData)> {
    use crate::css::style::effect_descriptions::PublishedValue;
    let [start, end] = effect.keyframes.as_ref() else {
        return None;
    };
    let value = |keyframe| match effect.declarations_of(keyframe) {
        [declaration] => match &declaration.value {
            PublishedValue::Declared(value) => Some((declaration.property_id, value.pointer())),
            PublishedValue::ElementValue => None,
        },
        _ => None,
    };
    let (property_id, start_value) = value(start)?;
    let (end_property_id, end_value) = value(end)?;
    (property_id == end_property_id).then_some((property_id, start_value, end_value))
}

impl RowTransitions {
    /// Whether the before-change style of the step `row` owes is under `display: none`, from which the step starts
    /// nothing.
    fn moves_from_display_none(
        engine: &crate::css::style::StyleEngine,
        row: &crate::css::style::bridge::FfiStyleDelta,
    ) -> bool {
        let Some(node) = crate::css::style::tree::StyleNodeID::from_raw(row.style_node) else {
            return false;
        };
        if !row.owes_a_transition_step || row.owes_an_animation_plan || row.pseudo_kind != u8::MAX {
            return false;
        }
        let before = match engine.transition_baseline(node, u8::MAX) {
            0 => row.old_style_record,
            baseline => baseline,
        };
        engine.style_record_view(before).is_some_and(|view| {
            view.dependency_flags & crate::css::computed_longhand_table::IN_DISPLAY_NONE_SUBTREE != 0
        })
    }

    /// Decides the step `row` owes, with the transform reference box of the element's box where one is known, or
    /// answers why the host has to.
    ///
    /// `at`, the time of the style change event in the document timeline's milliseconds where the caller knows it,
    /// decides the step of an element whose animations are all transitions the engine describes, as they stand then,
    /// and of one whose transitions name values it inherits from ancestors that run none. `lane` names the transitions
    /// a lane's hover started on the element, which it runs in place of the host's, where it runs any: the step decides
    /// over those as they stand at `at`.
    fn decide(
        engine: &mut crate::css::style::StyleEngine,
        row: &crate::css::style::bridge::FfiStyleDelta,
        reference_box: Option<crate::css::css_pixels::CssPixelRect>,
        at: Option<f64>,
        lane: Option<LaneTransitions<'_>>,
    ) -> Result<Self, &'static str> {
        use crate::css::property_metadata::property_id as prop;
        use crate::css::style::bridge::element_adjustment_fact::HAS_ANIMATIONS;

        if !row.owes_a_transition_step || row.owes_an_animation_plan || row.pseudo_kind != u8::MAX {
            return Err("a row that owes no step of its own");
        }
        let node = crate::css::style::tree::StyleNodeID::from_raw(row.style_node).ok_or("no style node")?;
        let (running, lane_effects, host_seen_before) =
            match (lane, engine.element_adjustment_facts(node) & HAS_ANIMATIONS != 0, at) {
                (Some(lane), _, Some(at)) => (
                    lane.transitions.running_at(at - lane.start_time)?,
                    Some(&lane.transitions.effects[..]),
                    lane.transitions.host_seen.clone(),
                ),
                (Some(_), _, None) => return Err("transitions a lane runs"),
                (None, false, _) => (Vec::new(), None, HostTransitionsSeen::default()),
                (None, true, Some(at)) => (
                    host_transitions_at(engine, node, at)?,
                    None,
                    HostTransitionsSeen::default(),
                ),
                (None, true, None) => return Err("an element with animations"),
            };
        let before = match engine.transition_baseline(node, u8::MAX) {
            0 => row.old_style_record,
            baseline => baseline,
        };
        let after = row.new_style_record;
        let before_view = engine.style_record_view(before).ok_or("a record that is gone")?;
        let after_view = engine.style_record_view(after).ok_or("a record that is gone")?;
        // SAFETY: A live record's table lives as long as the record.
        let after_table = unsafe { after_view.longhand_table.as_ref() }.ok_or("a record without a table")?;
        if before_view.dependency_flags & crate::css::computed_longhand_table::IN_DISPLAY_NONE_SUBTREE != 0 {
            return Err("a style under display: none");
        }
        if !after_view.animated_overlay.is_null() {
            return Err("an animated record");
        }
        // A declaration whose delay and duration are each the single value 0s starts nothing, so it matters only to an
        // element running transitions it could cancel, as `TransitionDecision::decide` has it.
        let entries = if running.is_empty()
            && crate::css::style_compute::transition_delay_and_duration_are_single_zero(after_table)
        {
            Vec::new()
        } else {
            crate::css::style_compute::transition_entries(after_table)
        };
        // A transform interpolates against the box it transforms.
        if reference_box.is_none() && entries.iter().any(|entry| entry.property_id == prop::TRANSFORM) {
            return Err("a transition of transform");
        }
        // An inherited value moves with the animations of the ancestors it comes from, which the host composes. Only a
        // caller that knows the time of the style change takes the values of ancestors that run none.
        if entries.iter().any(|entry| after_table.is_inherited(entry.property_id))
            && (at.is_none() || engine.inheritance_ancestors_animate(node))
        {
            return Err("a transition of an inherited value");
        }
        // The transitions the element runs, as the host's step hands them to the decision.
        let existing: Vec<_> = running
            .iter()
            .map(|transition| FfiExistingTransition {
                property_id: transition.property_id,
                running: transition.key.is_some(),
                end_value: transition.end_value,
                reversing_adjusted_start_value: transition.reversing_adjusted_start_value,
                // The output of the timing function of a running transition at the time of the style change event.
                timing_function_output: transition.key.map_or(0.0, |key| key / FULL_KEY),
                reversing_shortening_factor: transition.reversing_shortening_factor,
            })
            .collect();
        // The after-change style holds the current values of the transitions the element runs, as the overlay of the
        // record the host installed holds them.
        let after_overlay = match (running.is_empty(), at) {
            (false, Some(_)) => {
                use crate::css::animation::{FfiAnimationPreparationEffect, FfiSampledAnimationEffect};
                let mut composed = crate::css::style_compute::SampledEffects::new();
                for transition in &running {
                    if let Some(current_key) = transition.key {
                        composed.push(FfiSampledAnimationEffect {
                            effect: FfiAnimationPreparationEffect {
                                identity: transition.identity,
                                generation: transition.generation,
                            },
                            current_key,
                        });
                    }
                }
                let mut overlay = Box::new(crate::css::animated_overlay::AnimatedOverlay::default());
                engine
                    .sample_over_record(node, after, &mut overlay, lane_effects, composed, reference_box)
                    .map_err(|_| "a transition sample the host composes")?;
                Some(overlay)
            }
            _ => None,
        };
        let ended: smallvec::SmallVec<[u16; 4]> = running
            .iter()
            .filter(|transition| {
                let timing = &transition.timing.timing;
                transition.key.is_none() && transition.local_time >= timing.start_delay + timing.iteration_duration
            })
            .map(|transition| transition.property_id)
            .collect();
        let actions = TransitionDecision {
            before,
            after,
            element: Some(node),
        }
        .decide_over(
            engine,
            reference_box,
            after_overlay.as_deref(),
            &entries,
            &existing,
            &ended,
        );
        Ok(Self {
            node,
            before,
            after,
            actions,
            at,
            running,
            host_seen_before,
            after_overlay,
        })
    }
}

/// The host's transitions the steps of a lane's hover on an element saw, by their effects' identities.
#[derive(Clone, Default)]
pub(crate) struct HostTransitionsSeen {
    /// Those a step ended as they ran, in place of which the host runs what the steps decided.
    pub(crate) cancelled: smallvec::SmallVec<[u64; 2]>,
    /// Every one a step decided beside, whether it ran then, ended by itself, or a step ended it: one the steps neither
    /// ended nor left running ended by itself.
    pub(crate) seen: smallvec::SmallVec<[u64; 2]>,
}

/// The transitions a lane's hover started on an element, which the lane runs in place of those the host runs on it,
/// with when it started them, in the document timeline's milliseconds.
#[derive(Clone, Copy)]
pub(crate) struct LaneTransitions<'a> {
    pub(crate) transitions: &'a HoverTransitions,
    pub(crate) start_time: f64,
}

/// The transition step a style transaction decided beside the row of an element that owes one, which the host's step
/// reads rather than asking where it decides over the same inputs.
pub(crate) struct DecidedTransitionStep {
    node: u32,
    before: u64,
    after: u64,
    actions: Box<[FfiTransitionAction]>,
    /// The first sample of the transitions the step starts, until a host step that reads the decision takes it.
    fresh_sample: std::cell::Cell<Option<FreshTransitionSample>>,
}

// SAFETY: The values a decision points at live in the records it decided over, which the host keeps live across the drain
// of the transaction that decided it, the only time it reads them.
unsafe impl Send for DecidedTransitionStep {}

impl DecidedTransitionStep {
    /// Decides the step `row` owes where the host's step decides over nothing but what the engine holds: the element
    /// runs no animation and owes no animation plan, so the record the row moves it to is its after-change style; its
    /// before-change style is the one the epoch keeps for it, or else the record it held; and no property its
    /// transitions name reads a value it inherits, which an ancestor's animation can move before the host decides, or
    /// its box, which a frame can lay out first. Each transition is decided as one the element has none of yet, which
    /// the host checks.
    pub(crate) fn of_row(
        engine: &mut crate::css::style::StyleEngine,
        row: &crate::css::style::bridge::FfiStyleDelta,
    ) -> Option<Self> {
        let RowTransitions {
            node,
            before,
            after,
            actions,
            ..
        } = RowTransitions::decide(engine, row, None, None, None).ok()?;
        let fresh_sample = FreshTransitionSample::of_step(engine, node, after, &actions);
        Some(Self {
            node: row.style_node,
            before,
            after,
            actions: actions.into_boxed_slice(),
            fresh_sample: std::cell::Cell::new(fresh_sample),
        })
    }

    pub(crate) fn node(&self) -> u32 {
        self.node
    }

    /// Answers the step `decision` asks from this one, where it asks the same: from the same records, of a target with
    /// no `existing` transition.
    pub(crate) fn answer(
        &self,
        decision: &TransitionDecision,
        existing: &[FfiExistingTransition],
    ) -> Option<&[FfiTransitionAction]> {
        (decision.before == self.before && decision.after == self.after && existing.is_empty()).then_some(&self.actions)
    }

    /// The first sample of the transitions the step starts, which only the host step that read its decision samples.
    pub(crate) fn take_fresh_sample(&self) -> Option<FreshTransitionSample> {
        self.fresh_sample.take()
    }
}

/// The first sample of the transitions a decided step starts, taken beside the step over the record it moves its
/// element to, which the host's sample of them reads rather than asks. Each transition has only just started, so it
/// samples at the key its delay and easing give at its start, whatever the time.
pub(crate) struct FreshTransitionSample {
    after: u64,
    /// The key each transition the step starts samples at, in the order the step starts them.
    keys: Box<[f64]>,
    overlay: crate::css::animated_overlay::AnimatedOverlay,
    result: crate::css::style_compute::FfiHostAnimationSampleResult,
}

impl FreshTransitionSample {
    /// Samples the transitions `actions` start over `after`, each between the values and with the easing the action
    /// names, as the host starts them: delayed, filling backwards, and held at their start.
    fn of_step(
        engine: &mut crate::css::style::StyleEngine,
        node: crate::css::style::tree::StyleNodeID,
        after: u64,
        actions: &[FfiTransitionAction],
    ) -> Option<Self> {
        use crate::css::animation::{FfiAnimationPreparationEffect, FfiSampledAnimationEffect};

        let fresh = started_transition_effects(actions)?;
        let mut composed = crate::css::style_compute::SampledEffects::new();
        for effect in &fresh {
            composed.push(FfiSampledAnimationEffect {
                effect: FfiAnimationPreparationEffect {
                    identity: effect.identity,
                    generation: 0,
                },
                current_key: effect.timing.as_ref()?.key(Some(0.0))?,
            });
        }
        if composed.is_empty() {
            return None;
        }
        let keys = composed.iter().map(|effect| effect.current_key).collect();
        let mut overlay = crate::css::animated_overlay::AnimatedOverlay::default();
        let result = engine
            .sample_over_record(node, after, &mut overlay, Some(&fresh), composed, None)
            .ok()?;
        // The preparation is keyed by the step's own effects, which the host's are not.
        overlay.animation_preparation = None;
        Some(Self {
            after,
            keys,
            overlay,
            result,
        })
    }

    /// Answers the host's sample `input`, where it samples what this one did: the transitions the step started, over
    /// the record it moved its element to, each at the key this one sampled it at. Writes the overlay into the working
    /// set and answers what the sample found, with the timing each effect was sampled with, which the engine keeps.
    ///
    /// # Safety
    /// As for `rust_sample_animation_effects`.
    pub(crate) unsafe fn answer(
        self,
        input: &crate::css::style_compute::FfiHostAnimationSample,
    ) -> Option<(
        crate::css::style_compute::FfiHostAnimationSampleResult,
        crate::css::style::animations::SampledEffectTimings,
    )> {
        if input.style_record != self.after || !input.animated_overlay.is_null() {
            return None;
        }
        // SAFETY: Guaranteed by the caller.
        let effects = unsafe { crate::css::custom_properties::ffi_slice(input.effects, input.effect_count) };
        if effects.len() != self.keys.len() {
            return None;
        }
        let timings: Box<[_]> = effects
            .iter()
            .map(|effect| {
                (
                    effect.identity,
                    crate::css::style::animations::EffectTiming {
                        timing: effect.timing,
                        // SAFETY: As above.
                        easing: unsafe { crate::css::easing::Easing::from_descriptor(&effect.easing) },
                    },
                )
            })
            .collect();
        let samples_alike = timings
            .iter()
            .zip(effects)
            .zip(&self.keys)
            .all(|(((_, timing), effect), key)| {
                timing
                    .key(effect.current_key.into())
                    .is_some_and(|host_key| host_key.to_bits() == key.to_bits())
            });
        if !samples_alike {
            return None;
        }
        if self.result.outcome == crate::css::style_compute::FfiHostAnimationSampleOutcome::Evaluated {
            // SAFETY: As above.
            let overlay = unsafe { (input.prepare_overlay_for_mutation)(input.callback_context) };
            // SAFETY: The working set hands over its overlay, uniquely owned for the sample.
            unsafe { *overlay.cast::<crate::css::animated_overlay::AnimatedOverlay>() = self.overlay };
        }
        Some((self.result, timings))
    }
}

/// The effects of the transitions `actions` start, each between the values and with the easing the action names, with
/// the timing the host starts them with: delayed, filling backwards, and held at their start. None where an action does
/// other than start a transition, or an easing cannot be read.
fn started_transition_effects(
    actions: &[FfiTransitionAction],
) -> Option<Vec<crate::css::style::effect_descriptions::PublishedEffect>> {
    use crate::css::style::effect_descriptions::PublishedEffect;
    use crate::css::style_value::{RetainedStyleValueData, retain_style_value};

    let mut effects = Vec::new();
    for action in actions {
        match action.kind {
            FfiTransitionActionKind::None => continue,
            FfiTransitionActionKind::Start => {}
            _ => return None,
        }
        // SAFETY: The action's timing function lives in the record the step moves to.
        let easing = crate::css::style::effect_descriptions::easing_from_computed_timing_function(unsafe {
            &*action.timing_function
        })?;
        let identity = effects.len() as u64 + 1;
        // SAFETY: The decision compared live values, which the records it decided over hold.
        let [start, end] = [action.start_value, action.end_value]
            .map(|value| unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(value)) });
        effects.push(PublishedEffect::transition(
            identity,
            action.property_id,
            start,
            end,
            Some(started_transition_timing(action, easing)),
        ));
    }
    Some(effects)
}

/// The transitions the move of a hover's row leaves its element running, which the render owner shows in the element's
/// box, sampling them itself: those the move started, and those it ran before that run on. The host takes them in from
/// the time they started here.
#[derive(Clone)]
pub(crate) struct HoverTransitions {
    pub(crate) node: crate::css::style::tree::StyleNodeID,
    /// The record the row moved its element to, the after-change style, which the transitions sample over.
    pub(crate) after: u64,
    effects: Vec<crate::css::style::effect_descriptions::PublishedEffect>,
    /// How each of the effects runs.
    runs: Vec<HoverTransitionRun>,
    /// The properties the transitions animate.
    properties: Vec<u16>,
    /// How long after they start the transitions have all ended, in milliseconds.
    pub(crate) duration: f64,
    /// Whether the render owner decided the step over transitions the element ran, the host's or those a hover started,
    /// as they stood when it started.
    pub(crate) decided_over_running: bool,
    /// The host's transitions the hover's steps on the element saw, this one's and those before it.
    pub(crate) host_seen: HostTransitionsSeen,
    /// The properties of the running transitions of the host's that the step ended.
    pub(crate) ended_host_properties: smallvec::SmallVec<[u16; 2]>,
    /// The element's descendants that inherit an inherited property the transitions animate, in tree order, each with
    /// the properties it inherits, whose values the render owner composes over them as it samples the transitions.
    pub(crate) inheriting: crate::css::style::hover_lane::InheritingDescendants,
    /// Whether every element in the element's subtree inherits what the transitions animate, so that the render owner
    /// repaints it all as it samples them.
    pub(crate) inheriting_covers_subtree: bool,
}

/// How a transition a hover's step leaves its element running runs.
#[derive(Clone)]
struct HoverTransitionRun {
    timing: crate::css::style::animations::EffectTiming,
    /// Its local time as the step happens: none for one the step starts, and how far it ran for one that runs on.
    local_time: f64,
    reversing_adjusted_start_value: crate::css::style_value::RetainedStyleValueData,
    reversing_shortening_factor: f64,
    /// The timing function it eases with, where a hover started it.
    timing_function: Option<crate::css::style_value::RetainedStyleValueData>,
    /// The identity of the host's effect, where the host runs it.
    host_identity: Option<u64>,
    /// The time it started at, in the document timeline's milliseconds: that of the style change event that started it,
    /// where a hover started it.
    started_at: f64,
}

/// A timing a transition starts with: delayed, filling backwards, and held at its start.
fn started_transition_timing(
    action: &FfiTransitionAction,
    easing: crate::css::easing::Easing,
) -> crate::css::style::animations::EffectTiming {
    crate::css::style::animations::EffectTiming {
        timing: crate::css::style_compute::FfiEffectTiming {
            decidable: true,
            has_timeline_time: false,
            has_timeline_origin_time: false,
            has_timeline_scroller: false,
            timeline_scroller_is_vertical: false,
            timeline_scroller: 0,
            has_start_time: false,
            has_hold_time: true,
            paused: false,
            // `Bindings::FillMode::Backwards` and `Bindings::PlaybackDirection::Normal`.
            fill_mode: 2,
            playback_direction: 0,
            timeline_time: 0.0,
            timeline_origin_time: 0.0,
            start_time: 0.0,
            hold_time: 0.0,
            playback_rate: 1.0,
            start_delay: action.delay,
            end_delay: 0.0,
            iteration_duration: action.active_duration,
            iteration_count: 1.0,
            iteration_start: 0.0,
        },
        easing,
    }
}

/// The effects of the transitions a step decided over the transitions an element runs leaves it running, each with how
/// it runs: those it starts, between the values its actions name, and those that run on, as they ran.
fn hover_transition_effects(
    decision: &RowTransitions,
) -> Result<
    (
        Vec<crate::css::style::effect_descriptions::PublishedEffect>,
        Vec<HoverTransitionRun>,
    ),
    &'static str,
> {
    use crate::css::style::effect_descriptions::PublishedEffect;
    use crate::css::style_value::{RetainedStyleValueData, retain_style_value};
    use FfiTransitionActionKind as Kind;
    // SAFETY: The values a decision compared live in the records it decided over, or in the effects of the transitions
    //         it decided over, which the caller keeps live.
    let retain = |value: *const StyleValueData| {
        (!value.is_null()).then(|| unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(value)) })
    };
    let mut effects = Vec::new();
    let mut runs = Vec::new();
    for action in &decision.actions {
        let running = decision
            .running
            .iter()
            .find(|transition| transition.property_id == action.property_id);
        let identity = effects.len() as u64 + 1;
        match action.kind {
            Kind::None => {
                // A running transition the step leaves runs on as it ran. So does one a hover started that has ended,
                // which the host takes in as the screen showed it, a completed transition of the property.
                if let Some(transition) =
                    running.filter(|transition| transition.key.is_some() || transition.host_identity.is_none())
                {
                    let mut timing = transition.timing.clone();
                    timing.timing.has_hold_time = true;
                    effects.push(PublishedEffect::transition(
                        identity,
                        action.property_id,
                        retain(transition.start_value).ok_or("a transition without values")?,
                        retain(transition.end_value).ok_or("a transition without values")?,
                        Some(timing.clone()),
                    ));
                    runs.push(HoverTransitionRun {
                        timing,
                        local_time: transition.local_time,
                        reversing_adjusted_start_value: retain(transition.reversing_adjusted_start_value)
                            .ok_or("a transition without values")?,
                        reversing_shortening_factor: transition.reversing_shortening_factor,
                        timing_function: retain(transition.timing_function),
                        host_identity: transition.host_identity,
                        started_at: transition.started_at,
                    });
                }
                continue;
            }
            Kind::Remove | Kind::Cancel => continue,
            Kind::Start | Kind::RemoveAndStart | Kind::CancelRemoveAndStart => {}
        }
        // SAFETY: The action's timing function lives in the record the step moves to.
        let easing = crate::css::style::effect_descriptions::easing_from_computed_timing_function(unsafe {
            &*action.timing_function
        })
        .ok_or("a timing function the engine cannot read")?;
        let timing = started_transition_timing(action, easing);
        effects.push(PublishedEffect::transition(
            identity,
            action.property_id,
            retain(action.start_value).ok_or("a transition without values")?,
            retain(action.end_value).ok_or("a transition without values")?,
            Some(timing.clone()),
        ));
        runs.push(HoverTransitionRun {
            timing,
            local_time: 0.0,
            reversing_adjusted_start_value: retain(action.reversing_adjusted_start_value)
                .ok_or("a transition without values")?,
            reversing_shortening_factor: action.reversing_shortening_factor,
            timing_function: retain(action.timing_function),
            host_identity: None,
            started_at: decision.at.ok_or("a step at no time")?,
        });
    }
    Ok((effects, runs))
}

// SAFETY: The effects retain the values they interpolate, which nothing writes, and the render owner alone reads them.
unsafe impl Send for HoverTransitions {}

// SAFETY: Nothing writes the transitions once the lane runs them, which the host reads beside it as it takes them in.
unsafe impl Sync for HoverTransitions {}

/// One transition a lane's hover leaves an element running, as the host takes it in.
#[repr(C)]
pub struct FfiLaneTransition {
    pub property_id: u16,
    /// The identity of the effect of the host's transition it is, which the host keeps, or 0 for one the lane started.
    pub host_identity: u64,
    /// The time it started at, in the document timeline's milliseconds: that of the style change event that started it,
    /// where the lane started it, and where the lane took one the host runs to start, which the host takes in where its
    /// play is still pending.
    pub start_time: f64,
    pub delay: f64,
    pub duration: f64,
    pub start_value: *const StyleValueData,
    pub end_value: *const StyleValueData,
    pub reversing_adjusted_start_value: *const StyleValueData,
    pub reversing_shortening_factor: f64,
    /// The timing function it eases with, or null where the host runs it.
    pub timing_function: *const StyleValueData,
}

impl HoverTransitions {
    /// The transitions the move of `row` leaves its element running, decided as the host's step decides them where
    /// nothing but the engine's records decides it, none where the move starts none and the element runs none, or why
    /// the host has to decide them.
    ///
    /// `at` is the time of the style change event in the document timeline's milliseconds, at which the decision reads
    /// the transitions the element runs: those `lane` names, which a lane's hover started on it, where it does, and
    /// those the host runs on it otherwise.
    pub(crate) fn of_row(
        engine: &mut crate::css::style::StyleEngine,
        row: &crate::css::style::bridge::FfiStyleDelta,
        reference_box: Option<crate::css::css_pixels::CssPixelRect>,
        at: f64,
        lane: Option<LaneTransitions<'_>>,
    ) -> Result<Option<Self>, &'static str> {
        // NB: As the host's step, a move from a style under `display: none` starts no transition, and an element there
        //     runs none.
        if RowTransitions::moves_from_display_none(engine, row) {
            return Ok(None);
        }
        let decision = RowTransitions::decide(engine, row, reference_box, Some(at), lane)?;
        let (effects, runs) = hover_transition_effects(&decision)?;
        let (node, after) = (decision.node, decision.after);
        let decided_over_running = !decision.running.is_empty();
        if effects.is_empty() && !decided_over_running {
            return Ok(None);
        }
        let duration = runs
            .iter()
            .map(|run| run.timing.timing.start_delay + run.timing.timing.iteration_duration - run.local_time)
            .fold(0.0, f64::max);
        let properties = effects
            .iter()
            .filter_map(|effect| effect.declarations_of(effect.keyframes.first()?).first())
            .map(|declaration| declaration.property_id)
            .collect();
        // A running transition of the host's the step leaves no run of ended with the step.
        let mut host_seen = decision.host_seen_before;
        let mut ended_host_properties = smallvec::SmallVec::new();
        for transition in &decision.running {
            let Some(identity) = transition.host_identity else {
                continue;
            };
            if !host_seen.seen.contains(&identity) {
                host_seen.seen.push(identity);
            }
            if transition.key.is_some() && runs.iter().all(|run| run.host_identity != Some(identity)) {
                ended_host_properties.push(transition.property_id);
                if !host_seen.cancelled.contains(&identity) {
                    host_seen.cancelled.push(identity);
                }
            }
        }
        Ok(Some(Self {
            node,
            after,
            effects,
            runs,
            properties,
            duration,
            decided_over_running,
            host_seen,
            ended_host_properties,
            inheriting: Vec::new(),
            inheriting_covers_subtree: false,
        }))
    }

    /// The transitions as they stand `elapsed` milliseconds after they started, as a step then decides over them.
    fn running_at(&self, elapsed: f64) -> Result<Vec<RunningTransitionAt>, &'static str> {
        self.effects
            .iter()
            .zip(&self.runs)
            .map(|(effect, run)| {
                let (property_id, start_value, end_value) =
                    transition_values(effect).ok_or("a transition without values")?;
                let local_time = run.local_time + elapsed;
                let mut timing = run.timing.clone();
                timing.timing.hold_time = local_time;
                let key = timing
                    .key_at(Default::default())
                    .ok_or("a transition whose timing the engine cannot decide")?;
                Ok(RunningTransitionAt {
                    identity: effect.identity,
                    generation: 0,
                    host_identity: run.host_identity,
                    property_id,
                    key,
                    local_time,
                    timing: run.timing.clone(),
                    start_value,
                    end_value,
                    reversing_adjusted_start_value: run.reversing_adjusted_start_value.pointer(),
                    reversing_shortening_factor: run.reversing_shortening_factor,
                    timing_function: run
                        .timing_function
                        .as_ref()
                        .map_or(std::ptr::null(), |timing_function| timing_function.pointer()),
                    started_at: run.started_at,
                })
            })
            .collect()
    }

    /// Each transition, as the host runs it from the update that takes it in.
    pub(crate) fn lane_transitions(&self) -> impl Iterator<Item = FfiLaneTransition> + '_ {
        self.effects.iter().zip(&self.runs).filter_map(|(effect, run)| {
            let (property_id, start_value, end_value) = transition_values(effect)?;
            Some(FfiLaneTransition {
                property_id,
                host_identity: run.host_identity.unwrap_or(0),
                start_time: run.started_at,
                delay: run.timing.timing.start_delay,
                duration: run.timing.timing.iteration_duration,
                start_value,
                end_value,
                reversing_adjusted_start_value: run.reversing_adjusted_start_value.pointer(),
                reversing_shortening_factor: run.reversing_shortening_factor,
                timing_function: run
                    .timing_function
                    .as_ref()
                    .map_or(std::ptr::null(), |timing_function| timing_function.pointer()),
            })
        })
    }

    /// The properties of the running transitions of the host's that the step leaves running.
    pub(crate) fn kept_host_properties(&self) -> impl Iterator<Item = u16> + '_ {
        self.effects
            .iter()
            .zip(&self.runs)
            .filter(|(_, run)| run.host_identity.is_some())
            .filter_map(|(effect, _)| transition_values(effect).map(|(property, _, _)| property))
    }

    /// The properties the transitions animate.
    pub(crate) fn properties(&self) -> &[u16] {
        &self.properties
    }

    /// The inherited properties the transitions animate, which the element's descendants inherit.
    pub(crate) fn inherited_properties(&self) -> smallvec::SmallVec<[u16; 2]> {
        self.properties
            .iter()
            .copied()
            .filter(|&property| crate::css::property_metadata::property_is_inherited(property))
            .collect()
    }

    /// The non-inherited style groups the transitions animate, which the element's children read from the composition
    /// only where they inherit them explicitly; none where one is in no group.
    pub(crate) fn non_inherited_style_groups(&self) -> Option<u32> {
        self.properties
            .iter()
            .filter(|&&property| !crate::css::property_metadata::property_is_inherited(property))
            .try_fold(0u32, |groups, &property| {
                let group = crate::css::property_metadata::property_style_group_index(property)?;
                Some(groups | 1 << group)
            })
    }

    /// The record the transitions show `elapsed` milliseconds after they started, composed over the after-change
    /// style, pinned for the element's box.
    pub(crate) fn sample(
        &self,
        engine: &mut crate::css::style::StyleEngine,
        elapsed: f64,
        transform_reference_box: Option<crate::css::css_pixels::CssPixelRect>,
        scroll_snaps: bool,
    ) -> Result<
        (
            crate::css::style::layout_style::DerivedStyleRecord,
            crate::css::animated_overlay::AnimatedOverlay,
        ),
        crate::css::style::engine_sample::NeedsHost,
    > {
        use crate::css::animation::{FfiAnimationPreparationEffect, FfiSampledAnimationEffect};
        use crate::css::style::engine_sample::NeedsHost;
        let mut composed = crate::css::style_compute::SampledEffects::new();
        for (effect, run) in self.effects.iter().zip(&self.runs) {
            let mut timing = run.timing.clone();
            timing.timing.hold_time = run.local_time + elapsed;
            let Some(current_key) = timing.key_at(Default::default()).ok_or(NeedsHost)? else {
                continue;
            };
            composed.push(FfiSampledAnimationEffect {
                effect: FfiAnimationPreparationEffect {
                    identity: effect.identity,
                    generation: 0,
                },
                current_key,
            });
        }
        engine.sample_composed_over_record(
            self.node,
            self.after,
            crate::css::animated_overlay::AnimatedOverlay::default(),
            Some(&self.effects),
            composed,
            transform_reference_box,
            // The render owner records the frames that show the sample, with the visual contexts it moves, and
            // composes the values the descendants inherit over them.
            crate::css::style::SampleBounds {
                scroll_snaps,
                subtree_follows: self.inheriting_covers_subtree,
            },
        )
    }
}

/// One `transition-property` entry's attributes, as they apply to one physical longhand. The timing function is
/// borrowed from the computed longhand table the entry was read from.
pub(crate) struct TransitionEntry {
    pub(crate) property_id: u16,
    pub(crate) delay: f64,
    pub(crate) duration: f64,
    pub(crate) timing_function: *const StyleValueData,
    pub(crate) behavior: u8,
}

/// Whether the table's `transition-*` values give any longhand a matching entry, stopping at the first. A declaration
/// whose delay and duration are each the single value `0s`, which is how nearly every element declares no transition
/// at all, starts nothing, so it gives none unless the element `has_existing_transitions` it could still cancel.
///
/// # Safety
/// `longhand_table` must point to a live computed longhand table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_transition_has_matching_entries(
    longhand_table: *const std::ffi::c_void,
    has_existing_transitions: bool,
) -> bool {
    let table = unsafe { &*longhand_table.cast::<crate::css::computed_longhand_table::ComputedLonghandTable>() };
    (has_existing_transitions || !crate::css::style_compute::transition_delay_and_duration_are_single_zero(table))
        && crate::css::style_compute::has_transition_entries(table)
}

#[cfg(test)]
#[allow(clippy::arc_with_non_send_sync)]
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
        before_change_value: &StyleValueData,
        after_change_value: &StyleValueData,
        current_value: &StyleValueData,
    ) -> TransitionProperty {
        TransitionProperty {
            property_id: by_computed_value_property(),
            before_change_value,
            after_change_value,
            current_value,
            existing_end_value: std::ptr::null(),
            reversing_adjusted_start_value: std::ptr::null(),
            timing_function: std::ptr::null(),
            has_matching_transition: true,
            allow_discrete: false,
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
        let before = StyleValueData::Number { value: 0.0 };
        let after = StyleValueData::Number { value: 1.0 };
        assert_eq!(
            decide_transition(&animation_context(), &input(&before, &after, &before), false).kind,
            FfiTransitionActionKind::Start
        );
    }

    #[test]
    fn equal_nested_values_do_not_start_a_transition() {
        let nested_value = || {
            let number = std::sync::Arc::into_raw(std::sync::Arc::new(StyleValueData::Number { value: 0.5 }));
            StyleValueData::OpacityValue {
                value: unsafe { crate::css::style_value::RetainedStyleValueData::from_retained_pointer(number) },
            }
        };
        let before = nested_value();
        let after = nested_value();
        assert_eq!(
            decide_transition(&animation_context(), &input(&before, &after, &before), false).kind,
            FfiTransitionActionKind::None
        );
    }

    #[test]
    fn current_color_origins_are_equivalent() {
        let before = StyleValueData::Number { value: 0.0 };
        let after = StyleValueData::Number { value: 1.0 };
        let input = input(&before, &after, &before);
        assert_eq!(
            decide_transition(&animation_context(), &input, true).kind,
            FfiTransitionActionKind::None
        );
    }

    #[test]
    fn nested_current_color_origin_is_recognized() {
        let current_color = crate::css::style_value::RetainedStyleValueData::from_owned(StyleValueData::Keyword {
            keyword: crate::css::style_compute::keyword::CURRENTCOLOR,
        });
        let nested = StyleValueData::ValueList {
            values: crate::css::style_value::RetainedStyleValueDataList::from_retained_values(vec![current_color]),
            separator: 0,
            collapsible: false,
        };
        let mut table = crate::css::computed_longhand_table::ComputedLonghandTable::new();
        table.set(
            by_computed_value_property(),
            crate::css::style_value::RetainedStyleValueData::from_owned(nested),
            -1,
        );

        assert!(originates_from_current_color(&table, by_computed_value_property()));
    }

    #[test]
    fn removes_a_completed_transition_before_replacement() {
        let before = StyleValueData::Number { value: 0.0 };
        let after = StyleValueData::Number { value: 1.0 };
        let mut input = input(&before, &after, &before);
        input.has_completed_transition = true;
        input.existing_end_value = &raw const before;
        assert_eq!(
            decide_transition(&animation_context(), &input, false).kind,
            FfiTransitionActionKind::RemoveAndStart
        );
    }

    #[test]
    fn adjusts_a_reversing_transition() {
        let before = StyleValueData::Number { value: 0.0 };
        let after = StyleValueData::Number { value: 1.0 };
        let current = StyleValueData::Number { value: 0.5 };
        let mut input = input(&before, &after, &current);
        input.has_running_transition = true;
        input.existing_end_value = &raw const before;
        input.reversing_adjusted_start_value = &raw const after;
        input.delay = -20.0;
        input.old_timing_function_output = 0.25;
        input.old_reversing_shortening_factor = 0.5;
        let action = decide_transition(&animation_context(), &input, false);
        assert_eq!(action.kind, FfiTransitionActionKind::CancelRemoveAndStart);
        assert_eq!(action.reversing_shortening_factor, 0.625);
        assert_eq!(action.delay, -12.5);
        assert_eq!(action.active_duration, 62.5);
    }

    #[test]
    fn hover_transitions_publish_timing_without_changing_sampling() {
        use crate::css::style::tree::StyleNodeID;
        use crate::css::style_value::RetainedStyleValueData;
        use FfiTransitionActionKind as Kind;

        let start = RetainedStyleValueData::from_owned(StyleValueData::Number { value: 1.0 });
        let end = RetainedStyleValueData::from_owned(StyleValueData::Number { value: 0.5 });
        let easing = RetainedStyleValueData::from_owned(StyleValueData::Keyword {
            keyword: crate::css::css_enums::keyword::LINEAR,
        });
        for (kind, local_time, key, next_key) in [
            (Kind::None, 600.0, 50_000.0, 75_000.0),
            (Kind::Start, 0.0, 0.0, 15_000.0),
            (Kind::RemoveAndStart, 0.0, 0.0, 15_000.0),
            (Kind::CancelRemoveAndStart, 0.0, 0.0, 15_000.0),
        ] {
            let action = FfiTransitionAction {
                property_id: crate::css::property_metadata::property_id::OPACITY,
                kind,
                delay: 100.0,
                active_duration: 1000.0,
                reversing_shortening_factor: 1.0,
                start_value: start.pointer(),
                end_value: end.pointer(),
                reversing_adjusted_start_value: start.pointer(),
                timing_function: easing.pointer(),
            };
            let mut timing = started_transition_timing(&action, Default::default());
            timing.timing.hold_time = local_time;
            let running = if kind == Kind::None {
                vec![RunningTransitionAt {
                    identity: 1,
                    generation: 0,
                    host_identity: None,
                    property_id: action.property_id,
                    key: Some(key),
                    local_time,
                    timing: timing.clone(),
                    start_value: start.pointer(),
                    end_value: end.pointer(),
                    reversing_adjusted_start_value: start.pointer(),
                    reversing_shortening_factor: 1.0,
                    timing_function: easing.pointer(),
                    started_at: 500.0,
                }]
            } else {
                Vec::new()
            };
            let decision = RowTransitions {
                node: StyleNodeID::element(1),
                before: 1,
                after: 2,
                actions: vec![action],
                at: Some(500.0),
                running,
                host_seen_before: HostTransitionsSeen::default(),
                after_overlay: None,
            };
            let (effects, runs) = hover_transition_effects(&decision).unwrap();
            let published_timing = effects[0].timing.as_ref().unwrap();
            assert!(published_timing.implies_will_change(Default::default()));
            assert_eq!(published_timing.timing, timing.timing);
            assert_eq!(runs[0].timing.timing, timing.timing);
            assert_eq!(runs[0].local_time, local_time);
            assert_eq!(runs[0].timing.key_at(Default::default()), Some(Some(key)));
            let mut sampled_timing = runs[0].timing.clone();
            sampled_timing.timing.hold_time = local_time + 250.0;
            assert_eq!(sampled_timing.key_at(Default::default()), Some(Some(next_key)));
        }
    }
}
