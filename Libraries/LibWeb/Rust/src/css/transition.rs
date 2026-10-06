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

#[derive(Clone, Copy)]
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
    pub has_running_transition: bool,
    pub has_completed_transition: bool,
    pub delay: f64,
    pub duration: f64,
    pub old_timing_function_output: f64,
    pub old_reversing_shortening_factor: f64,
}

#[repr(C)]
pub struct FfiTransitionInput {
    /// The context the transitions decide in, but for the transform reference box, which the render owner reads.
    pub context: crate::css::animation::FfiAnimationContext,
    pub properties: *mut FfiTransitionPropertyInput,
    pub property_count: usize,
    /// The target's style node, or 0 when it has none.
    pub target_node: u32,
    /// The target's pseudo-element kind, or `u8::MAX` for an element.
    pub target_pseudo_kind: u8,
    /// The slot of the row of the box of the target's element, a `NodeSlotId`'s index, invalid where it has none.
    pub element_box_slot: u32,
}

#[derive(Clone, Copy)]
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

fn values_equal(
    first: *const crate::css::style_value::StyleValueData,
    second: *const crate::css::style_value::StyleValueData,
) -> bool {
    assert!(!first.is_null());
    assert!(!second.is_null());
    let (first, second) = unsafe { (&*first, &*second) };
    std::ptr::eq(first, second) || first == second
}

fn decide_transition(
    context: &crate::css::animation::FfiAnimationContext,
    input: &FfiTransitionPropertyInput,
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

fn value_is_current_color(value: *const crate::css::style_value::StyleValueData) -> bool {
    matches!(
        unsafe { value.as_ref() },
        Some(crate::css::style_value::StyleValueData::Keyword { keyword })
            if *keyword == crate::css::style_compute::keyword::CURRENTCOLOR
    )
}

fn computed_value(
    table: &crate::css::computed_longhand_table::ComputedLonghandTable,
    overlay: Option<&crate::css::animated_overlay::AnimatedOverlay>,
    property_id: u16,
) -> *const crate::css::style_value::StyleValueData {
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
    property: &mut FfiTransitionPropertyInput,
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

/// A transition step's question to the style engine: what each property the host prepared does to the transitions
/// of the step's target as its style changes from the record `before` to the record `after`, the record the target
/// installed. The transitions resolve their lengths against `after`.
pub(crate) struct TransitionDecision {
    pub(crate) before: u64,
    pub(crate) after: u64,
    pub(crate) context: crate::css::animation::FfiAnimationContext,
    /// The target's style node, where the target is an element: only an element's own record inherits from its
    /// inheritance parent.
    pub(crate) element: Option<crate::css::style::tree::StyleNodeID>,
}

impl TransitionDecision {
    /// Runs the CSS Transitions decision algorithm for every property, writing the values it compared into the
    /// property, and its decision into the action beside it. The transitions resolve their transforms against
    /// `reference_box`, the transform reference box of the box of the target's element, where it was laid out.
    pub(crate) fn decide(
        self,
        engine: &crate::css::style::StyleEngine,
        reference_box: Option<crate::css::css_pixels::CssPixelRect>,
        properties: &mut [FfiTransitionPropertyInput],
        actions: &mut [FfiTransitionAction],
    ) {
        let Self {
            before,
            after,
            mut context,
            element,
        } = self;
        if let Some(length) = engine.transition_length_resolution_context(after) {
            context.has_length_resolution_context = true;
            context.length_resolution_context = length;
        }
        context.set_transform_reference_box(reference_box);
        let (Some(before), Some(after)) = (engine.style_record_view(before), engine.style_record_view(after)) else {
            debug_assert!(false, "the records a transition step decides over remain live");
            return;
        };
        // SAFETY: A live record's table and overlay live as long as the record.
        let (Some(before_table), before_overlay, Some(after_table), after_overlay) = (unsafe {
            (
                before.longhand_table.as_ref(),
                before.animated_overlay.as_ref(),
                after.longhand_table.as_ref(),
                after.animated_overlay.as_ref(),
            )
        }) else {
            debug_assert!(false, "the records of a transition step carry longhand tables");
            return;
        };
        for (property, action) in properties.iter_mut().zip(actions) {
            let inherited_animation = element
                .filter(|_| {
                    after_overlay
                        .and_then(|overlay| overlay.get(property.property_id))
                        .is_none_or(|entry| !entry.inherited)
                })
                .and_then(|element| engine.inherited_animated_value(element, after_table, property.property_id));
            let values_originate_from_current_color = prepare_transition_values(
                (before_table, before_overlay),
                after_table,
                after_overlay,
                inherited_animation,
                property,
            );
            *action = decide_transition(&context, property, values_originate_from_current_color);
        }
    }
}

/// The transition step a style transaction decided beside the row of an element that owes one, which the host's step
/// reads rather than asking where it decides over the same inputs.
pub(crate) struct DecidedTransitionStep {
    node: u32,
    before: u64,
    after: u64,
    current_color: *const crate::css::style_value::StyleValueData,
    properties: Box<[FfiTransitionPropertyInput]>,
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
        use crate::css::property_metadata::property_id as prop;
        use crate::css::style::bridge::element_adjustment_fact::HAS_ANIMATIONS;

        if !row.owes_a_transition_step || row.owes_an_animation_plan || row.pseudo_kind != u8::MAX {
            return None;
        }
        let node = crate::css::style::tree::StyleNodeID::from_raw(row.style_node)?;
        if engine.element_adjustment_facts(node) & HAS_ANIMATIONS != 0 {
            return None;
        }
        let before = match engine.transition_baseline(node, u8::MAX) {
            0 => row.old_style_record,
            baseline => baseline,
        };
        let after = row.new_style_record;
        let before_view = engine.style_record_view(before)?;
        let after_view = engine.style_record_view(after)?;
        // SAFETY: A live record's table lives as long as the record.
        let after_table = unsafe { after_view.longhand_table.as_ref() }?;
        if before_view.dependency_flags & crate::css::computed_longhand_table::IN_DISPLAY_NONE_SUBTREE != 0
            || !after_view.animated_overlay.is_null()
            || crate::css::style_compute::transition_delay_and_duration_are_single_zero(after_table)
        {
            return None;
        }
        let entries = crate::css::style_compute::transition_entries(after_table);
        if entries.is_empty()
            || entries
                .iter()
                .any(|entry| entry.property_id == prop::TRANSFORM || after_table.is_inherited(entry.property_id))
        {
            return None;
        }
        let mut properties: Box<[_]> = entries
            .iter()
            .map(|entry| FfiTransitionPropertyInput {
                property_id: entry.property_id,
                before_change_value: std::ptr::null(),
                after_change_value: std::ptr::null(),
                current_value: std::ptr::null(),
                existing_end_value: std::ptr::null(),
                reversing_adjusted_start_value: std::ptr::null(),
                has_matching_transition: true,
                allow_discrete: entry.behavior == crate::css::css_enums::transition_behavior::ALLOW_DISCRETE,
                has_running_transition: false,
                has_completed_transition: false,
                delay: entry.delay,
                duration: entry.duration,
                old_timing_function_output: 0.0,
                old_reversing_shortening_factor: 1.0,
            })
            .collect();
        let mut actions: Box<[_]> = entries
            .iter()
            .map(|entry| FfiTransitionAction {
                property_id: entry.property_id,
                kind: FfiTransitionActionKind::None,
                delay: 0.0,
                active_duration: 0.0,
                reversing_shortening_factor: 1.0,
            })
            .collect();
        // The current color is the after-change style's, which holds no animated value.
        let current_color = after_table.effective_value(None, prop::COLOR, true).value.cast();
        let context = crate::css::animation::FfiAnimationContext {
            allow_discrete: false,
            current_color,
            has_length_resolution_context: false,
            length_resolution_context: Default::default(),
            has_transform_reference_box: false,
            transform_reference_box_width: 0.0,
            transform_reference_box_height: 0.0,
        };
        TransitionDecision {
            before,
            after,
            context,
            element: Some(node),
        }
        .decide(engine, None, &mut properties, &mut actions);
        let fresh_sample = FreshTransitionSample::of_step(engine, node, after, &entries, &properties, &actions);
        Some(Self {
            node: row.style_node,
            before,
            after,
            current_color,
            properties,
            actions,
            fresh_sample: std::cell::Cell::new(fresh_sample),
        })
    }

    pub(crate) fn node(&self) -> u32 {
        self.node
    }

    /// Answers the step `decision` asks of `properties` from this one, where it asks the same: from the same records, in
    /// the same current color, for the same transitions, none of which the element has yet. Writes the values each
    /// transition compared into its property and the decision into the action beside it, and answers whether it did.
    pub(crate) fn answer(
        &self,
        decision: &TransitionDecision,
        properties: &mut [FfiTransitionPropertyInput],
        actions: &mut [FfiTransitionAction],
    ) -> bool {
        let asks_the_same = decision.before == self.before
            && decision.after == self.after
            && values_equal(decision.context.current_color, self.current_color)
            && properties.len() == self.properties.len()
            && properties.iter().zip(&self.properties).all(|(asked, decided)| {
                asked.property_id == decided.property_id
                    && asked.has_matching_transition
                    && !asked.has_running_transition
                    && !asked.has_completed_transition
                    && asked.allow_discrete == decided.allow_discrete
                    && asked.delay.to_bits() == decided.delay.to_bits()
                    && asked.duration.to_bits() == decided.duration.to_bits()
            });
        if asks_the_same {
            properties.copy_from_slice(&self.properties);
            actions.copy_from_slice(&self.actions);
        }
        asks_the_same
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
    /// Samples the transitions `actions` start over `after`, each from the values the decision compared to the easing
    /// its entry names, as the host starts them: delayed, filling backwards, and held at their start.
    fn of_step(
        engine: &mut crate::css::style::StyleEngine,
        node: crate::css::style::tree::StyleNodeID,
        after: u64,
        entries: &[FfiTransitionEntry],
        properties: &[FfiTransitionPropertyInput],
        actions: &[FfiTransitionAction],
    ) -> Option<Self> {
        use crate::css::animation::{FfiAnimationPreparationEffect, FfiSampledAnimationEffect};
        use crate::css::style::effect_descriptions::PublishedEffect;
        use crate::css::style_value::{RetainedStyleValueData, retain_style_value};

        let mut fresh = Vec::new();
        let mut composed = crate::css::style_compute::SampledEffects::new();
        for ((entry, property), action) in entries.iter().zip(properties).zip(actions) {
            match action.kind {
                FfiTransitionActionKind::None => continue,
                FfiTransitionActionKind::Start => {}
                _ => return None,
            }
            // SAFETY: The entry's timing function lives in the record the step moves to.
            let easing = crate::css::style::effect_descriptions::easing_from_computed_timing_function(unsafe {
                &*entry.timing_function
            })?;
            let timing = crate::css::style::animations::EffectTiming {
                timing: crate::css::style_compute::FfiEffectTiming {
                    decidable: true,
                    has_timeline_time: false,
                    has_timeline_origin_time: false,
                    has_timeline_scroller: false,
                    timeline_scroller_is_vertical: false,
                    timeline_scroller: 0,
                    has_start_time: false,
                    has_hold_time: true,
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
            };
            let identity = fresh.len() as u64 + 1;
            composed.push(FfiSampledAnimationEffect {
                effect: FfiAnimationPreparationEffect {
                    identity,
                    generation: 0,
                },
                current_key: timing.key(0.0)?,
            });
            // SAFETY: The decision compared live values, which the records it decided over hold.
            let [start, end] = [property.before_change_value, property.after_change_value]
                .map(|value| unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(value)) });
            fresh.push(PublishedEffect::transition(identity, property.property_id, start, end));
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
                    .key(effect.current_key)
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

/// One `transition-property` entry's attributes, as they apply to one physical longhand.
#[repr(C)]
pub struct FfiTransitionEntry {
    pub property_id: u16,
    pub delay: f64,
    pub duration: f64,
    pub timing_function: *const crate::css::style_value::StyleValueData,
    pub behavior: u8,
}

#[repr(C)]
pub struct FfiTransitionEntries {
    pub entries: *mut FfiTransitionEntry,
    pub count: usize,
}

/// The transitions a computed longhand table declares, per physical longhand they name. What the
/// timing functions point at is borrowed from the table.
///
/// # Safety
/// `longhand_table` must point to a live computed longhand table. The result must be released with
/// `rust_transition_entries_release`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_transition_entries(longhand_table: *const std::ffi::c_void) -> FfiTransitionEntries {
    let table = unsafe { &*longhand_table.cast::<crate::css::computed_longhand_table::ComputedLonghandTable>() };
    let entries = Box::into_raw(crate::css::style_compute::transition_entries(table).into_boxed_slice());
    FfiTransitionEntries {
        entries: entries.cast(),
        count: entries.len(),
    }
}

/// # Safety
/// `entries` must come from `rust_transition_entries` and not have been released before.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_transition_entries_release(entries: FfiTransitionEntries) {
    drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(entries.entries, entries.count)) });
}

/// Whether `rust_transition_entries` would answer any entry for the table, stopping at the first.
///
/// # Safety
/// `longhand_table` must point to a live computed longhand table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_transition_has_entries(longhand_table: *const std::ffi::c_void) -> bool {
    crate::css::style_compute::has_transition_entries(unsafe {
        &*longhand_table.cast::<crate::css::computed_longhand_table::ComputedLonghandTable>()
    })
}

/// Whether the table's `transition-delay` and `transition-duration` are each the single value `0s`,
/// which is how nearly every element declares no transition at all.
///
/// # Safety
/// `longhand_table` must point to a live computed longhand table.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_transition_delay_and_duration_are_single_zero(
    longhand_table: *const std::ffi::c_void,
) -> bool {
    crate::css::style_compute::transition_delay_and_duration_are_single_zero(unsafe {
        &*longhand_table.cast::<crate::css::computed_longhand_table::ComputedLonghandTable>()
    })
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
            decide_transition(&animation_context(), &input(&before, &after, &before), false).kind,
            FfiTransitionActionKind::Start
        );
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
            decide_transition(&animation_context(), &input(&before, &after, &before), false).kind,
            FfiTransitionActionKind::None
        );
    }

    #[test]
    fn current_color_origins_are_equivalent() {
        let before = crate::css::style_value::StyleValueData::Number { value: 0.0 };
        let after = crate::css::style_value::StyleValueData::Number { value: 1.0 };
        let input = input(&before, &after, &before);
        assert_eq!(
            decide_transition(&animation_context(), &input, true).kind,
            FfiTransitionActionKind::None
        );
    }

    #[test]
    fn nested_current_color_origin_is_recognized() {
        let current_color = crate::css::style_value::RetainedStyleValueData::from_owned(
            crate::css::style_value::StyleValueData::Keyword {
                keyword: crate::css::style_compute::keyword::CURRENTCOLOR,
            },
        );
        let nested = crate::css::style_value::StyleValueData::ValueList {
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
        let before = crate::css::style_value::StyleValueData::Number { value: 0.0 };
        let after = crate::css::style_value::StyleValueData::Number { value: 1.0 };
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
        let action = decide_transition(&animation_context(), &input, false);
        assert_eq!(action.kind, FfiTransitionActionKind::CancelRemoveAndStartReversing);
        assert_eq!(action.reversing_shortening_factor, 0.625);
        assert_eq!(action.delay, -12.5);
        assert_eq!(action.active_duration, 62.5);
    }
}
