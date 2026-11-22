/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the animated style updates and the rendering updates of a document decide of the animations associated with
//! it.

use super::animation::{AnimationCell, AnimationHandle};
use super::composite_order::composite_order;
use super::compositor_update::{
    next_throttled_animation_iteration_event_time, request_observation_sample, set_needs_animated_style_update,
};
use super::document_animations::{DocumentAnimationSetKind, DocumentAnimations};
use super::effect::{AnimationEffectCell, EffectFlag, EffectHandle, ObservationSampleRequest};
use super::element_animations::ElementAnimations;
use super::ffi::{
    FfiKeepAlive, HostEffectPerformer, host_tree_order, run_procedure, web_animation_compare_elements_in_tree_order,
};
use super::host_timing::FfiAnimationKind;
use super::throttling::{can_skip_per_frame_animation_tick, can_skip_per_frame_style_update};
use super::timing::{AnimationPlayState, OptionalNumber, TimeValueType};
use std::ffi::c_void;

/// What sampling the effects that need an animated style update of a document asks of the host, which hands each
/// function `context`.
#[repr(C)]
pub struct FfiAnimatedStyleSampleHost {
    pub context: *mut c_void,
    pub keep_alive: FfiKeepAlive,
    /// Readies the host to sample the effects.
    pub begin: unsafe extern "C" fn(context: *mut c_void),
    /// Samples `effect` into the style of its target.
    pub update: unsafe extern "C" fn(context: *mut c_void, effect: &AnimationEffectCell),
    /// Has the host publish what it sampled.
    pub end: unsafe extern "C" fn(context: *mut c_void),
}

/// What sampling the effects that need an animated style update of a document found.
#[repr(C)]
pub struct FfiAnimatedStyleSampleOutcome {
    /// Whether there were any effects to sample.
    pub sampled: bool,
    /// Whether an effect that can skip its per-frame style update was left unsampled.
    pub has_throttled_animation_style_update: bool,
}

/// Samples the effects whose animated style a document's next animated style update samples, in the composite order
/// of their animations, or, where `force_throttled`, those of every animation associated with it that is not idle,
/// and the idle ones that need it, skipping those that can skip their per-frame style update and that no observation
/// asked for unless `force_throttled`. `has_throttled` is whether an earlier update left a throttled effect unsampled.
///
/// # Safety
/// `animations` must be what a live document knows of animations, which the host may reach as it samples.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_sample_effects_needing_style_update(
    animations: *mut DocumentAnimations,
    force_throttled: bool,
    has_throttled: bool,
    host: &FfiAnimatedStyleSampleHost,
) -> FfiAnimatedStyleSampleOutcome {
    let mut effects: Vec<(AnimationHandle, EffectHandle)> = Vec::new();
    {
        // SAFETY: Guaranteed by the caller, and the borrow ends before the host is called.
        let animations = unsafe { &mut *animations };
        if force_throttled {
            for animation in animations.associated_animations() {
                let Some(effect) = animation.borrow().effect().cloned() else {
                    continue;
                };
                // NB: Dirty idle effects still need to remove their last sampled values after cancellation.
                if animation.borrow().play_state() == AnimationPlayState::Idle
                    && !animations.needing_animated_style_update().contains(&effect)
                {
                    continue;
                }
                effects.push((animation, effect));
            }
            animations.needing_animated_style_update().take_each(|_| {});
        } else {
            animations.needing_animated_style_update().take_each(|effect| {
                // NB: A canceled animation still needs to remove its last sampled values.
                if let Some(animation) = effect.borrow().associated_animation() {
                    effects.push((animation, effect.clone()));
                }
            });
        }
    }

    if effects.is_empty() {
        // A compositor-driven effect can remain throttled when every dirty effect was canceled before this sample.
        // Preserve the flag so a later style or geometry read can still request a current main-thread sample.
        return FfiAnimatedStyleSampleOutcome {
            sampled: false,
            has_throttled_animation_style_update: has_throttled,
        };
    }

    // NB: The effects stay alive for as long as the update holds them.
    for (animation, _) in &effects {
        // SAFETY: The host keeps alive an animation it is handed for as long as this call lasts.
        unsafe { (host.keep_alive)(host.context, animation) };
    }

    let has_requested_observation_sample = effects
        .iter()
        .any(|(_, effect)| effect.borrow().has_flag(EffectFlag::NeedsObservationSample));

    // A timeline script observed ahead of its current time in this task is sampled at that time too, so that a style
    // read agrees with the time script read, however many reads the task makes. Where an observation asked for the
    // sample, all of them are.
    DocumentAnimations::at_observed_times(animations, force_throttled || has_requested_observation_sample, || {
        // SAFETY: The host answers synchronously.
        unsafe { (host.begin)(host.context) };
        // SAFETY: As above.
        let has_throttled_animation_style_update =
            unsafe { sample_effects_in_composite_order(effects, force_throttled, has_throttled, host) };
        // SAFETY: As above.
        unsafe { (host.end)(host.context) };
        FfiAnimatedStyleSampleOutcome {
            sampled: true,
            has_throttled_animation_style_update,
        }
    })
}

/// Samples `effects` through `host` in the composite order of their animations, answering whether an effect that can
/// skip its per-frame style update was left unsampled.
///
/// # Safety
/// The host must sample synchronously.
unsafe fn sample_effects_in_composite_order(
    mut effects: Vec<(AnimationHandle, EffectHandle)>,
    force_throttled: bool,
    has_throttled: bool,
    host: &FfiAnimatedStyleSampleHost,
) -> bool {
    let mut tree_order = host_tree_order(web_animation_compare_elements_in_tree_order);
    effects.sort_by(|(a, _), (b, _)| {
        if a.ptr_eq(b) {
            return std::cmp::Ordering::Equal;
        }
        composite_order(&a.borrow(), &b.borrow(), &mut tree_order)
    });

    let mut has_throttled_animation_style_update = !force_throttled && has_throttled;
    for (_, effect) in &effects {
        let observation_sample_requested = effect.borrow_mut().set_flag(EffectFlag::NeedsObservationSample, false);
        if can_skip_per_frame_style_update(effect) {
            has_throttled_animation_style_update = true;
            if !force_throttled && !observation_sample_requested {
                continue;
            }
        }
        // SAFETY: The host answers synchronously, and the effect stays alive through the call.
        unsafe { (host.update)(host.context, effect) };
    }
    has_throttled_animation_style_update
}

/// A change of the sets of the effects a document's animated style updates sample.
#[expect(dead_code, reason = "C++ constructs the variants")]
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum EffectSetsChange {
    /// Sets aside the effects the next animated style update samples while the transaction of a style update that
    /// flew drains.
    SetAside,
    /// Restores the effects set aside.
    Restore,
    /// Forgets the effects the next animated style update samples, and those to ask for an observation sample.
    Clear,
    /// Asks for an observation sample of each effect that asked for one while the animated style update that finishes
    /// ran.
    FinishUpdate,
}

/// Makes `change` to the sets of the effects the animated style updates of a document sample.
///
/// # Safety
/// `animations` must be what a live document knows of animations, which the host may reach as it is asked for
/// samples.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_change_effect_sets(
    animations: *mut DocumentAnimations,
    change: EffectSetsChange,
) {
    // SAFETY: Guaranteed by the caller, and the borrow ends before the host is called.
    let document = unsafe { &mut *animations };
    match change {
        EffectSetsChange::SetAside => document.set_aside_effects_needing_animated_style_update(false),
        EffectSetsChange::Restore => document.set_aside_effects_needing_animated_style_update(true),
        EffectSetsChange::Clear => {
            document.needing_animated_style_update().take_each(|_| {});
            document
                .needing_animated_style_update_after_current_update()
                .take_each(|_| {});
        }
        EffectSetsChange::FinishUpdate => {
            let mut effects = Vec::new();
            document
                .needing_animated_style_update_after_current_update()
                .take_each(|effect| effects.push(effect.clone()));
            for effect in &effects {
                request_observation_sample(effect);
            }
        }
    }
}

/// Asks for an observation sample of each effect of the animations associated with an element, whose lists
/// `element` are, on behalf of a read of its style or layout in the task of generation `task_generation`: those
/// the next animated style update of the document samples anyway, unless `is_updating_animated_style`, and those that
/// can skip their per-frame style update, after the current update where `is_updating_animated_style`, and once per
/// task otherwise.
///
/// # Safety
/// `document` must be what the live document of the element knows of animations, which the host may reach as it is
/// asked for samples.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_element_animations_request_observation_samples(
    element: &ElementAnimations,
    document: *mut DocumentAnimations,
    is_updating_animated_style: bool,
    task_generation: u64,
) {
    let effects: Vec<EffectHandle> = element
        .associated()
        .iter()
        .filter_map(|animation| animation.borrow().effect().cloned())
        .collect();
    for effect in &effects {
        // NB: An effect that changed since it was last sampled, such as one script seeked after an earlier read in
        //     this task, is sampled for the read whether or not it can skip per-frame style updates now: the sample
        //     decides that again, from a layout that may have moved since.
        // SAFETY: Guaranteed by the caller; the borrow ends before the host is called.
        if !is_updating_animated_style
            && unsafe { &mut *document }
                .needing_animated_style_update()
                .contains(effect)
        {
            request_observation_sample(effect);
            continue;
        }
        if !can_skip_per_frame_style_update(effect) {
            continue;
        }
        if is_updating_animated_style {
            // SAFETY: Guaranteed by the caller.
            unsafe { &mut *document }
                .needing_animated_style_update_after_current_update()
                .insert(effect);
            continue;
        }
        let request = effect.borrow_mut().request_observation_sample_in_task(task_generation);
        if request == ObservationSampleRequest::NewlyRequested {
            set_needs_animated_style_update(effect);
        }
    }
}

/// Something a document asks of the animations associated with it.
#[expect(dead_code, reason = "C++ constructs the variants")]
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum DocumentAnimationsQuery {
    /// The number of the animations associated with the document.
    AssociatedCount,
    /// Whether the associated effect of one of them can skip its per-frame style update, as one or zero.
    AnyCanSkipPerFrameStyleUpdate,
    /// Whether a scroll timeline is associated with the document, as one or zero.
    AnyScrollTimeline,
}

/// Answers `query` of the animations associated with a document.
///
/// # Safety
/// `animations` must be what a live document knows of animations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_query(
    animations: *mut DocumentAnimations,
    query: DocumentAnimationsQuery,
) -> usize {
    // SAFETY: Guaranteed by the caller, and the borrow ends before the host is called.
    let document = unsafe { &mut *animations };
    match query {
        DocumentAnimationsQuery::AssociatedCount => {
            document.set_mut(DocumentAnimationSetKind::Associated).iter().count()
        }
        DocumentAnimationsQuery::AnyCanSkipPerFrameStyleUpdate => usize::from(
            document
                .set_mut(DocumentAnimationSetKind::Associated)
                .iter()
                .any(|animation| {
                    let effect = animation.borrow().effect().cloned();
                    effect.is_some_and(|effect| can_skip_per_frame_style_update(&effect))
                }),
        ),
        DocumentAnimationsQuery::AnyScrollTimeline => usize::from(
            document
                .timelines
                .iter()
                .any(|timeline| timeline.borrow().scroll().is_some()),
        ),
    }
}

/// What keeps the rendering updates of a document coming while its animations run.
#[repr(C)]
pub struct FfiAnimationFramePump {
    /// The delay after which the next iteration event of a throttled CSS animation is due, where one is.
    pub wakeup_delay: OptionalNumber,
    /// Whether an animation needs the next rendering update.
    pub request_frame: bool,
}

/// Decides how the running animations associated with a document of a monotonically increasing timeline keep its
/// rendering updates coming: by asking for the next where one needs main-thread painting or observable animation
/// events, or by waking up as the next iteration event of a throttled CSS animation is due, noting of each effect
/// whether a frame skipped its tick.
///
/// # Safety
/// `animations` must be what a live document knows of animations.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_animations_frame_pump(
    animations: *mut DocumentAnimations,
) -> FfiAnimationFramePump {
    // SAFETY: Guaranteed by the caller, and the borrow ends before the host is called.
    let associated = unsafe { &mut *animations }.associated_animations();
    let mut wakeup_delay: Option<f64> = None;
    for animation in &associated {
        let effect = {
            let record = animation.borrow();
            if !record.has_monotonically_increasing_timeline() || record.play_state() != AnimationPlayState::Running {
                continue;
            }
            record.effect().cloned()
        };
        if let Some(effect) = effect {
            if can_skip_per_frame_animation_tick(&effect) {
                effect
                    .borrow_mut()
                    .set_flag(EffectFlag::PerFrameAnimationTickWasSkipped, true);
                continue;
            }
            effect
                .borrow_mut()
                .set_flag(EffectFlag::PerFrameAnimationTickWasSkipped, false);
            let state = animation.borrow().timing_state(None);
            if let Some(iteration_event_time) = next_throttled_animation_iteration_event_time(&state, &effect)
                && let Some(current_time) = state.current_time.get()
                && current_time.r#type == TimeValueType::Milliseconds
            {
                let delay = (iteration_event_time - current_time.value) / state.playback.playback_rate;
                if delay > 0.0 {
                    wakeup_delay = Some(wakeup_delay.map_or(delay, |earliest| earliest.min(delay)));
                    continue;
                }
            }
        }
        return FfiAnimationFramePump {
            wakeup_delay: wakeup_delay.into(),
            request_frame: true,
        };
    }
    FfiAnimationFramePump {
        wakeup_delay: wakeup_delay.into(),
        request_frame: false,
    }
}

/// Whether `animation` is a CSS animation or transition that the style of a rendering update began, which the update
/// makes ready once its style and layout are done.
fn rendering_update_makes_ready(animation: &AnimationCell) -> bool {
    let record = animation.borrow();
    let Some(effect) = record.effect() else {
        return false;
    };
    let playback = record.playback();
    (playback.has_pending_play_task || playback.has_pending_pause_task)
        && matches!(
            record.ffi_kind(),
            FfiAnimationKind::CssAnimation | FfiAnimationKind::CssTransition
        )
        && !effect.borrow().may_run_on_the_compositor()
}

/// Runs the pending play tasks of the CSS animations and transitions associated with the timelines of a document whose
/// animations are `animations` that the style of a rendering update began, which the update makes ready once its style
/// and layout are done, having the host keep each alive through `keep_alive` with `context`.
///
/// # Safety
/// `animations` must be what a live document knows of animations, and `keep_alive` must keep alive an animation it is
/// handed with `context` for as long as the call lasts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn animations_document_run_pending_css_play_tasks(
    animations: *mut DocumentAnimations,
    context: *mut c_void,
    keep_alive: FfiKeepAlive,
    perform: HostEffectPerformer,
) {
    // SAFETY: Guaranteed by the caller, and the borrow ends here.
    let timelines = unsafe { &*animations }.timelines.clone();
    let mut pending: Vec<AnimationHandle> = Vec::new();
    // NB: An animation with a pending task needs updates, so only those are looked at.
    for timeline in &timelines {
        pending.extend(
            timeline
                .associated_animations_needing_update()
                .into_iter()
                .filter(|animation| rendering_update_makes_ready(animation)),
        );
    }
    for animation in &pending {
        // SAFETY: Guaranteed by the caller.
        unsafe { keep_alive(context, animation) };
    }
    for animation in &pending {
        run_procedure(animation, perform, |procedures| {
            procedures.run_pending_play_task_if_ready();
        });
    }
}

/// Whether an animation associated with a timeline of a document whose animations are `animations` has a rendering
/// update hold the tasks of the document until the whole of it has run: any, where the update did not take in the style
/// of the document, or else one the update makes ready or one the compositor runs, which the update's frame publishes.
#[unsafe(no_mangle)]
pub extern "C" fn animations_document_timelines_hold_rendering_update_tasks(
    animations: &DocumentAnimations,
    style_taken_in: bool,
) -> bool {
    animations.timelines.iter().any(|timeline| {
        timeline.associated_animations().borrow().iter().any(|animation| {
            !style_taken_in
                || rendering_update_makes_ready(&animation)
                || animation
                    .borrow()
                    .effect()
                    .is_some_and(|effect| effect.borrow().has_flag(EffectFlag::CompositorDriven))
        })
    })
}

#[cfg(test)]
mod ffi_test_stubs {
    #[unsafe(no_mangle)]
    extern "C" fn web_animation_compare_elements_in_tree_order(
        _first: *mut std::ffi::c_void,
        _second: *mut std::ffi::c_void,
    ) -> i32 {
        unreachable!("the tests sample no animated style")
    }
}
