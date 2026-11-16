/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Whether a frame can skip the style update or the tick of an effect, from its record and what the host knows of its
//! target.

use super::animation::AnimationHandle;
use super::compositor_update::is_transform_family;
use super::effect::{AnimationEffectCell, EffectFlag, PerFrameStyleUpdateSkipDecision, PerFrameStyleUpdateSkipKey};
use super::host_timing::FfiAnimationKind;
use super::rendering_trace::note_rendering_trace_event;
use super::timing::{self, AnimationEffectPhase, AnimationPlayState};
use crate::css::property_metadata::property_id;
use std::cell::OnceCell;
use std::ffi::c_void;
use std::ptr::NonNull;

// NB: What the host is asked of, or told about, an element, by bit: the facts it answers, and the counters it bumps.

/// The node document of the element has its layout up to date.
pub const TARGET_FACT_LAYOUT_IS_UP_TO_DATE: u32 = 1 << 0;
/// The element is connected.
pub const TARGET_FACT_IS_CONNECTED: u32 = 1 << 1;
/// The element has a layout box.
pub const TARGET_FACT_HAS_LAYOUT_BOX: u32 = 1 << 2;
/// The element is in the SVG namespace.
pub const TARGET_FACT_IS_SVG: u32 = 1 << 3;
/// A navigable container the element's document is nested through has an up to date layout and no box.
pub const TARGET_FACT_INSIDE_BOXLESS_CONTAINER: u32 = 1 << 4;
/// Nothing shows the style of the element, as a display: none ancestor leaves it without a box, and no SVG
/// element in its subtree can be referenced from elsewhere.
pub const TARGET_FACT_UNSEEN_IN_DISPLAY_NONE_SUBTREE: u32 = 1 << 5;
/// The element has a box with hidden visibility, and nothing in its box's subtree is visible.
pub const TARGET_FACT_HIDDEN_WITHOUT_VISIBLE_DESCENDANT: u32 = 1 << 6;
/// A listener on the element, the nodes its events bubble to or its window hears a CSS animation event that needs
/// a tick.
pub const TARGET_FACT_HEARS_CSS_ANIMATION_EVENTS: u32 = 1 << 7;
/// The same, for the iteration events alone.
pub const TARGET_FACT_HEARS_CSS_ANIMATION_ITERATION_EVENTS: u32 = 1 << 8;
/// A listener on the element, an SVG animation element, hears its beginEvent, endEvent or repeatEvent.
pub const TARGET_FACT_HEARS_TIME_EVENTS: u32 = 1 << 9;
/// The same, for its repeatEvent alone.
pub const TARGET_FACT_HEARS_REPEAT_EVENT: u32 = 1 << 10;
/// What the host found whether a frame can skip a style update from, for the key it answers.
pub const TARGET_FACT_SKIP_KEY: u32 = 1 << 11;

/// What the host answers of an element: the facts it was asked of that hold, and the skip key where it was asked for
/// one.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiTargetFacts {
    pub facts: u32,
    pub key: PerFrameStyleUpdateSkipKey,
}

unsafe extern "C" {
    fn web_animation_target_facts(element: NonNull<c_void>, wanted: u32) -> FfiTargetFacts;
}

/// What the host answers of `wanted` of the host's `DOM::Element` `element`, which something the caller holds keeps
/// alive.
pub(crate) fn target_facts(element: NonNull<c_void>, wanted: u32) -> FfiTargetFacts {
    // SAFETY: Guaranteed by the caller.
    unsafe { web_animation_target_facts(element, wanted) }
}

/// Whether `fact` holds of `element`.
pub(crate) fn target_fact_holds(element: NonNull<c_void>, fact: u32) -> bool {
    target_facts(element, fact).facts & fact != 0
}

/// Whether a frame can skip the style update of `effect`.
pub(crate) fn can_skip_per_frame_style_update(effect: &AnimationEffectCell) -> bool {
    let record = effect.borrow();
    if record.has_flag(EffectFlag::CompositorDriven) || record.has_flag(EffectFlag::CompositorReplaced) {
        return true;
    }
    let offscreen_throttled = record.has_flag(EffectFlag::OffscreenThrottled);
    let keyframes = record.keyframes().cloned();
    let target_properties = keyframes
        .as_ref()
        .map_or(&[][..], |keyframes| keyframes.target_properties());
    if offscreen_throttled && !target_properties.iter().copied().any(is_transform_family) {
        return true;
    }

    let Some(target) = record.target_element() else {
        return false;
    };
    let has_pseudo_element = record.pseudo_element().is_some();
    let timing = record.timing();
    let animation = record.associated_animation();
    drop(record);

    // An animation inside a box-less iframe can't affect its embedding document's rendering. Check the containers
    // before the target-local cache, since their layout can change without changing the target's style or layout box.
    // Reverse playback and effects with a finite end time need ticks to finish, even with an infinite iteration count.
    if let Some(animation) = &animation
        && timing.iteration_count.is_infinite()
        && {
            let animation = animation.borrow();
            animation.playback().playback_rate > 0.0 && animation.associated_effect_end().value.is_infinite()
        }
        && target_fact_holds(target, TARGET_FACT_INSIDE_BOXLESS_CONTAINER)
    {
        return true;
    }

    let facts = target_facts(
        target,
        TARGET_FACT_LAYOUT_IS_UP_TO_DATE | TARGET_FACT_IS_CONNECTED | TARGET_FACT_SKIP_KEY,
    );
    let layout_is_up_to_date = facts.facts & TARGET_FACT_LAYOUT_IS_UP_TO_DATE != 0;
    let is_connected = facts.facts & TARGET_FACT_IS_CONNECTED != 0;
    let key = facts.key;
    let cache_result = |result: bool| {
        if layout_is_up_to_date {
            effect
                .borrow_mut()
                .note_per_frame_style_update_skip_decision(key, result);
        }
        result
    };
    if layout_is_up_to_date {
        let decision = effect.borrow().per_frame_style_update_skip_decision(&key);
        match decision {
            PerFrameStyleUpdateSkipDecision::CanSkip | PerFrameStyleUpdateSkipDecision::CannotSkip => {
                note_rendering_trace_event("animation style skip cache hit");
                return decision == PerFrameStyleUpdateSkipDecision::CanSkip;
            }
            PerFrameStyleUpdateSkipDecision::Stale => {
                note_rendering_trace_event("animation style skip cache miss");
            }
            PerFrameStyleUpdateSkipDecision::Unknown => {}
        }
    }

    // Restrict throttling within a rendered document to infinite transform-only effects until animation overflow
    // updates can also be throttled safely.
    if !timing.iteration_count.is_infinite() || has_pseudo_element {
        return cache_result(false);
    }

    if !is_connected {
        return cache_result(true);
    }
    // NB: A custom property can feed the display of the target through var().
    let may_animate_display = keyframes.as_ref().is_some_and(|keyframes| {
        keyframes.target_properties().contains(&property_id::DISPLAY)
            || !keyframes.target_custom_properties().is_empty()
    });
    if layout_is_up_to_date
        && !may_animate_display
        && target_fact_holds(target, TARGET_FACT_UNSEEN_IN_DISPLAY_NONE_SUBTREE)
    {
        return cache_result(true);
    }
    let Some(keyframes) = keyframes else {
        return false;
    };
    if (keyframes.target_properties().is_empty() && keyframes.target_custom_properties().is_empty())
        || !keyframes.target_custom_properties().is_empty()
        || !keyframes.target_properties().iter().copied().all(is_transform_family)
    {
        return cache_result(false);
    }

    let facts = target_facts(target, TARGET_FACT_HAS_LAYOUT_BOX | TARGET_FACT_IS_SVG).facts;
    // NB: Transforms cannot affect rendering without a layout box. This also covers SVG content inside
    //     display-none subtrees and closed details elements.
    if layout_is_up_to_date && facts & TARGET_FACT_HAS_LAYOUT_BOX == 0 {
        return cache_result(true);
    }
    if facts & TARGET_FACT_IS_SVG != 0 {
        return cache_result(false);
    }

    if offscreen_throttled {
        return cache_result(true);
    }

    if !layout_is_up_to_date {
        return false;
    }
    cache_result(target_fact_holds(target, TARGET_FACT_HIDDEN_WITHOUT_VISIBLE_DESCENDANT))
}

/// Whether a frame can skip the tick of `effect`.
pub(crate) fn can_skip_per_frame_animation_tick(effect: &AnimationEffectCell) -> bool {
    let record = effect.borrow();
    let animation = record.associated_animation();
    let timing = record.timing();
    let target = record.target_element();
    let compositor_handled =
        record.has_flag(EffectFlag::CompositorDriven) || record.has_flag(EffectFlag::CompositorReplaced);
    let observation_relevant = record.has_flag(EffectFlag::ObservationRelevantCompositorAnimation);
    drop(record);
    // NB: The phase of the effect is at the current time of its associated animation. Resolve it once, where needed.
    let phase_cell = OnceCell::new();
    let phase = || {
        *phase_cell.get_or_init(|| match &animation {
            Some(animation) => animation.borrow().effect_phase(),
            None => timing::resolve_timing(&timing, None, None, None).phase,
        })
    };

    if let Some(animation) = &animation
        && {
            let animation = animation.borrow();
            let playback = animation.playback();
            !playback.has_pending_play_task
                && !playback.has_pending_pause_task
                && animation.play_state() == AnimationPlayState::Running
                && playback.playback_rate > 0.0
                && animation.has_monotonically_increasing_timeline()
        }
        && phase() == AnimationEffectPhase::Before
    {
        return true;
    }

    if !compositor_handled && !can_skip_per_frame_style_update(effect) {
        return false;
    }

    // NB: Playing backward, an effect with an infinite iteration count finishes at time zero, and with a zero iteration
    //     duration it finishes at its end time. No timer wakes the main thread for either.
    if let Some(animation) = &animation
        && timing.iteration_count.is_infinite()
        && {
            let animation = animation.borrow();
            animation.playback().playback_rate < 0.0 || !animation.associated_effect_end().value.is_infinite()
        }
    {
        return false;
    }

    // A compositor-handled one-iteration effect has a document timer that wakes the main thread at its active end.
    if compositor_handled && (!timing.iteration_count.is_infinite() || observation_relevant) {
        return true;
    }

    // NB: Infinite effects cannot reach their natural end, and finite offscreen paint effects have an end timer.
    //     Neither needs a continuous tick for animationend listeners.
    // NB: Starting or canceling an active animation requests an update independently of playback.
    //     Only iteration events require future updates while a visually throttled effect runs.
    !css_animation_events_are_heard(
        animation.as_ref(),
        animation.as_ref().map(|animation| animation.borrow().ffi_kind()),
        target,
        phase() == AnimationEffectPhase::Active,
    )
}

/// Whether a listener hears the CSS animation events of an effect targeting `target`, associated with `animation` of
/// `kind`, that need a tick: only its iteration events if `only_iteration_events`.
fn css_animation_events_are_heard(
    animation: Option<&AnimationHandle>,
    kind: Option<FfiAnimationKind>,
    target: Option<NonNull<c_void>>,
    only_iteration_events: bool,
) -> bool {
    // NB: SMIL animations raise time events at their animation element instead, which do not bubble.
    if let Some(animation) = animation
        && kind == Some(FfiAnimationKind::Smil)
    {
        let animation_element = super::ffi::smil_animation_element(animation);
        let fact = if only_iteration_events {
            TARGET_FACT_HEARS_REPEAT_EVENT
        } else {
            TARGET_FACT_HEARS_TIME_EVENTS
        };
        return target_fact_holds(animation_element, fact);
    }

    // Script animations do not dispatch CSS animation events, even when an ancestor listens for them.
    if animation.is_some() && kind != Some(FfiAnimationKind::CssAnimation) {
        return false;
    }

    let target = target.expect("an effect whose events are heard has a target");
    let fact = if only_iteration_events {
        TARGET_FACT_HEARS_CSS_ANIMATION_ITERATION_EVENTS
    } else {
        TARGET_FACT_HEARS_CSS_ANIMATION_EVENTS
    };
    target_fact_holds(target, fact)
}

#[cfg(test)]
mod ffi_test_stubs {
    #[unsafe(no_mangle)]
    extern "C" fn web_animation_target_facts(
        _element: std::ptr::NonNull<std::ffi::c_void>,
        _wanted: u32,
    ) -> super::FfiTargetFacts {
        unreachable!("the tests ask the host nothing of elements")
    }
}
