/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The CSS animations an element owns, mirrored for the style computation.
//!
//! Reconciling an element's `CSSAnimation` objects against its freshly computed `animation-*`
//! longhands needs to know which animations the element already owns. The names of those
//! animations are published here, so the computation decides the reconciliation from its own
//! inputs and hands the host a plan: per definition, the animation it claims, or none.
//!
//! Beside each name is the definition the last plan applied to that animation, so a plan that
//! would leave every animation as it is need not be handed to the host at all. Everything the host
//! does once a definition has found its animation - applying the timing, cancelling what no
//! definition claimed - it does from the list it already holds.
//!
//! Which `@keyframes` a definition runs is decided here too, from the keyframes each style scope
//! publishes, so the computation never reaches into a scope's rule cache.

use super::tree::{StyleNodeID, TreeScopeID};
use crate::animations::keyframes::KeyframeSet;
use crate::animations::snapshot::{CompactEffectTiming, ScrollProgress};
use crate::animations::timing::{
    self, AnimationEffectTiming, AnimationPlayState, AnimationPlayback, OptionalTimeValue, ResolvedEffectTiming,
    TimeValue, TimelineSample,
};
use crate::css::css_string::CssString;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData, retain_style_value};
use std::collections::HashMap;
use std::sync::Arc;

/// Which of an element's animation lists a row belongs to, in the host's own numbering: zero for
/// the element itself, and the pseudo-element's value plus one for each pseudo-element.
pub(crate) type AnimationSlot = u8;

/// The definition that claimed no existing animation and asks for a new one.
pub(crate) const NO_MATCHED_ANIMATION: i32 = -1;

/// What an applied animation definition holds by value, its numbers before its bytes, which pack behind them.
#[derive(Clone, Copy, PartialEq)]
struct AppliedAnimationValues {
    duration: f64,
    iteration_count: f64,
    delay: f64,
    range_start_px: f64,
    range_start_percentage: f64,
    range_end_px: f64,
    range_end_percentage: f64,
    duration_is_auto: bool,
    direction: u8,
    play_state: u8,
    fill_mode: u8,
    composition: u8,
    /// An `FfiAnimationTimelineKind`.
    timeline_kind: u8,
    scroll_scroller: u8,
    scroll_axis: u8,
    range_start_name: u8,
    range_end_name: u8,
}

const _: () = assert!(size_of::<AppliedAnimationValues>() == 72);

impl AppliedAnimationValues {
    /// The values of the definition `animation`, field for field.
    #[must_use]
    fn of(animation: &crate::css::style_compute::FfiComputedAnimation) -> Self {
        Self {
            duration_is_auto: animation.duration_is_auto,
            duration: animation.duration,
            iteration_count: animation.iteration_count,
            direction: animation.direction,
            play_state: animation.play_state,
            delay: animation.delay,
            fill_mode: animation.fill_mode,
            composition: animation.composition,
            timeline_kind: animation.timeline_kind as u8,
            scroll_scroller: animation.scroll_scroller,
            scroll_axis: animation.scroll_axis,
            range_start_name: animation.range_start_name,
            range_start_px: animation.range_start_px,
            range_start_percentage: animation.range_start_percentage,
            range_end_name: animation.range_end_name,
            range_end_px: animation.range_end_px,
            range_end_percentage: animation.range_end_percentage,
        }
    }
}

/// Whether applying `definition` to an animation that last had `applied` applied would leave it exactly as it is. A
/// scroll, view or named timeline is found from the element's surroundings every time it is applied, so a definition
/// that names one is never called unchanged. The timing function and range calculations are compared by value: a
/// recomputed style builds a fresh allocation for a declaration that has not changed.
#[must_use]
fn would_change_nothing(
    definition: &crate::css::style_compute::FfiComputedAnimation,
    applied: &AppliedAnimationDefinition,
) -> bool {
    use crate::css::style_compute::FfiAnimationTimelineKind;
    let keyframe_set = applied
        .keyframes
        .as_ref()
        .map_or(std::ptr::null(), |set| Arc::as_ptr(set).cast());
    !matches!(
        definition.timeline_kind,
        FfiAnimationTimelineKind::Scroll | FfiAnimationTimelineKind::View | FfiAnimationTimelineKind::Named
    ) && !applied.timing_function.pointer().is_null()
        && AppliedAnimationValues::of(definition) == applied.values
        && definition.keyframe_set == keyframe_set
        && [
            (definition.timing_function, &applied.timing_function),
            (definition.range_start_calculated, &applied.range_start_calculated),
            (definition.range_end_calculated, &applied.range_end_calculated),
        ]
        .into_iter()
        .all(|(value, applied_value)| {
            // SAFETY: Each is null or a value the plan holds live.
            let value = unsafe { value.cast::<crate::css::style_value::StyleValueData>().as_ref() };
            crate::css::style_value::optional_style_values_equal(value, applied_value.optional_data())
        })
}

/// The definition the style computation last applied to a CSS animation, which the animation holds, and the style
/// engine beside the animation's name, so that a plan whose every definition equals what its animation last had
/// applied changes nothing, and the engine does not hand it over.
pub(crate) struct AppliedAnimationDefinition {
    values: AppliedAnimationValues,
    /// The keyframe set the plan gave the animation's effect.
    keyframes: Option<Arc<KeyframeSet>>,
    /// The computed `animation-timing-function`.
    timing_function: RetainedStyleValueData,
    /// The calculations the edges of the attachment range resolve, or none.
    range_start_calculated: RetainedStyleValueData,
    range_end_calculated: RetainedStyleValueData,
}

impl AppliedAnimationDefinition {
    /// The definition a style computation applies as it hands `definition` over, holding what it names.
    ///
    /// # Safety
    /// The values `definition` names must be null or live style values, and its keyframe set null or one the plan
    /// holds a reference to.
    pub(crate) unsafe fn retained(definition: &crate::css::style_compute::FfiComputedAnimation) -> Self {
        let hold = |value: *const std::ffi::c_void| {
            if value.is_null() {
                return RetainedStyleValueData::none();
            }
            // SAFETY: Guaranteed by the caller; the handle takes the reference this retains.
            unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(value.cast::<StyleValueData>())) }
        };
        let keyframe_set = definition.keyframe_set.cast::<KeyframeSet>();
        Self {
            values: AppliedAnimationValues::of(definition),
            // SAFETY: Guaranteed by the caller.
            keyframes: (!keyframe_set.is_null()).then(|| unsafe { KeyframeSet::retained_from_host(keyframe_set) }),
            timing_function: hold(definition.timing_function),
            range_start_calculated: hold(definition.range_start_calculated),
            range_end_calculated: hold(definition.range_end_calculated),
        }
    }

    /// The computed `animation-timing-function`.
    #[must_use]
    pub(crate) fn timing_function(&self) -> &RetainedStyleValueData {
        &self.timing_function
    }

    /// Whether applying `self` to an animation sets the same properties as applying `other` does, whatever keyframes
    /// each gives its effect.
    #[must_use]
    pub(crate) fn applies_the_same_properties(&self, other: &Self) -> bool {
        self.values == other.values
            && [
                (&self.timing_function, &other.timing_function),
                (&self.range_start_calculated, &other.range_start_calculated),
                (&self.range_end_calculated, &other.range_end_calculated),
            ]
            .into_iter()
            .all(|(value, other_value)| {
                crate::css::style_value::optional_style_values_equal(value.optional_data(), other_value.optional_data())
            })
    }
}

/// One of the CSS animations the host holds: its name, and the definition the last plan applied
/// to it, which it shares with the animation, or none where no plan has described it yet.
#[derive(Clone)]
pub(crate) struct CssDefinedAnimation {
    pub(crate) name: CssString,
    pub(crate) applied_definition: Option<Arc<AppliedAnimationDefinition>>,
}

impl CssDefinedAnimation {
    /// The name of the CSS animation `animation` and the definition last applied to it.
    pub(crate) fn of(animation: &crate::animations::animation::AnimationCell) -> Self {
        let animation = animation.borrow();
        let state = animation.css_animation();
        Self {
            // SAFETY: The animation holds its name.
            name: unsafe { CssString::from_borrowed_raw(state.name.raw()) },
            applied_definition: state.applied_definition.clone(),
        }
    }
}

/// The CSS animations the host holds in one of an element's lists, in its order.
type CssDefinedAnimationList = (AnimationSlot, Box<[CssDefinedAnimation]>);

/// Per element, the CSS animations the host holds for it and each of its pseudo-elements, in the
/// order the host holds them.
#[derive(Clone, Default)]
pub(crate) struct CssDefinedAnimations {
    /// Owning a CSS animation is rare, so only the elements that do have a row, and a row holds
    /// only the lists that are not empty.
    rows: crate::cow_map::CowMap<StyleNodeID, Vec<CssDefinedAnimationList>>,
}

impl CssDefinedAnimations {
    /// The bytes the rows hold on the heap, with each name the first time `sizer` reaches it.
    pub(crate) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        use crate::heap_size::vec_bytes;
        let mut bytes = self.rows.heap_size();
        for lists in self.rows.values() {
            bytes += vec_bytes(lists);
            for (_, animations) in lists {
                bytes += size_of_val::<[CssDefinedAnimation]>(animations);
                bytes += animations
                    .iter()
                    .map(|animation| animation.name.heap_size(sizer))
                    .sum::<usize>();
            }
        }
        bytes
    }

    /// Replace one list with the animations the host publishes. An empty list drops it, so an element that stops
    /// animating stops costing anything.
    pub(crate) fn set(&mut self, node: StyleNodeID, slot: AnimationSlot, animations: Box<[CssDefinedAnimation]>) {
        let lists = self.rows.entry(node).or_default();
        let existing = lists.iter().position(|(list_slot, _)| *list_slot == slot);
        match (existing, animations.is_empty()) {
            (Some(index), true) => {
                lists.swap_remove(index);
            }
            (Some(index), false) => lists[index].1 = animations,
            (None, true) => {}
            (None, false) => lists.push((slot, animations)),
        }
        if lists.is_empty() {
            self.rows.remove(&node);
        }
    }

    /// One of an element's lists, in the order the host holds the animations.
    #[must_use]
    pub(crate) fn list(&self, node: StyleNodeID, slot: AnimationSlot) -> &[CssDefinedAnimation] {
        self.rows
            .get(&node)
            .and_then(|lists| lists.iter().find(|(list_slot, _)| *list_slot == slot))
            .map_or(&[], |(_, animations)| animations)
    }

    /// Whether the host holds a CSS animation for the element or any of its pseudo-elements.
    #[must_use]
    pub(crate) fn node_runs_a_css_animation(&self, node: StyleNodeID) -> bool {
        self.rows.contains_key(&node)
    }

    /// Give up the lists of an identity that retires. An identity can be minted again for another
    /// element, so a list left behind would be read as that element's.
    pub(crate) fn retire(&mut self, node: StyleNodeID) {
        self.rows.remove(&node);
    }
}

/// Match newly computed animation definitions against the animations the element already owns,
/// handing `claim` each definition that claims one, with the index of the animation it claims. A
/// definition not handed to `claim` asks for a new animation.
///
/// https://drafts.csswg.org/css-animations-1/#animations
/// The same @keyframes rule name may be repeated within an animation-name. Changes to the
/// animation-name update existing animations by iterating over the new list of animations from last
/// to first, and, for each animation, finding the last matching animation in the list of existing
/// animations. If a match is found, the existing animation is updated using the animation properties
/// corresponding to its position in the new list of animations, whilst maintaining its current
/// playback time as described above. The matching animation is removed from the existing list of
/// animations such that it will not match twice. If a match is not found, a new animation is
/// created. As a result, updating animation-name from ‘a’ to ‘a, a’ will cause the existing
/// animation for ‘a’ to become the second animation in the list and a new animation will be created
/// for the first item in the list.
pub(crate) fn match_existing_animations(
    existing: &[CssDefinedAnimation],
    definition_names: &[&CssString],
    mut claim: impl FnMut(usize, usize),
) {
    // NB: Rather than removing a matched animation from the list, mark it claimed, so that the
    //     indices stay those of the list the host holds.
    let mut claimed = vec![false; existing.len()];
    for (index, name) in definition_names.iter().enumerate().rev() {
        let Some(candidate) = (0..existing.len())
            .rev()
            .find(|&candidate| !claimed[candidate] && existing[candidate].name == **name)
        else {
            continue;
        };
        claimed[candidate] = true;
        claim(index, candidate);
    }
}

/// Whether applying a plan would leave the element's list and every animation in it as they are:
/// every definition claims the animation already in its own place and computes for it exactly what
/// that animation last had applied, and no animation is left for the plan to cancel. Such a plan
/// creates, cancels and reorders nothing, and sets each animation's timing, keyframes and place to
/// what they already are.
#[must_use]
pub(crate) fn plan_changes_nothing(
    definitions: &[crate::css::style_compute::FfiComputedAnimation],
    existing: &[CssDefinedAnimation],
) -> bool {
    definitions.len() == existing.len()
        && definitions
            .iter()
            .zip(existing)
            .enumerate()
            .all(|(index, (definition, animation))| {
                usize::try_from(definition.matched_existing_index) == Ok(index)
                    && animation
                        .applied_definition
                        .as_ref()
                        .is_some_and(|applied| would_change_nothing(definition, applied))
            })
}

/// The tree scope an `animation-name` declaration was written in, where the `@keyframes` it names
/// are looked for first.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum DeclarationScope {
    /// A declaration of the document, of another origin or of the element itself, or none at all:
    /// none of these has a scope of its own.
    Unscoped,
    /// A rule of a style sheet a shadow root's scope holds.
    Shadow(TreeScopeID),
}

/// One style scope's `@keyframes`: the keyframe set for each name the scope defines.
pub(crate) type KeyframesRow = HashMap<Box<[u16]>, Arc<KeyframeSet>>;

/// The `@keyframes` every style scope of the document defines, as each scope's rule cache resolved
/// them, with the keyframe set for each name.
///
/// A scope publishes its row whenever its rule cache is built, and the host builds every scope's
/// cache before a style update, so resolving an animation's keyframes is a lookup here rather
/// than building a rule cache in the middle of a style computation.
#[derive(Clone, Default)]
pub(crate) struct AnimationKeyframes {
    scopes: crate::cow_map::CowMap<TreeScopeID, KeyframesRow>,
}

impl AnimationKeyframes {
    /// The row of the names, packed into one buffer of code units with a length each, the way an
    /// element's animation names arrive, and the host's keyframe set for each.
    pub(crate) fn row(name_lengths: &[u32], name_units: &[u16], keyframe_sets: Vec<Arc<KeyframeSet>>) -> KeyframesRow {
        debug_assert_eq!(name_lengths.len(), keyframe_sets.len());
        let mut offset = 0;
        name_lengths
            .iter()
            .zip(keyframe_sets)
            .map(|(&length, set)| {
                let units = &name_units[offset..offset + length as usize];
                offset += length as usize;
                (Box::from(units), set)
            })
            .collect()
    }

    /// Replaces one scope's row. An empty row gives the scope's row up.
    pub(crate) fn set(&mut self, tree_scope: TreeScopeID, row: KeyframesRow) {
        if row.is_empty() {
            self.scopes.remove(&tree_scope);
            return;
        }
        self.scopes.insert(tree_scope, row);
    }

    /// The keyframe set an animation of this name runs, or `None` where no scope in its chain
    /// defines the name.
    ///
    /// The chain is the tree scope of the winning `animation-name` declaration first, because that
    /// declaration can come from a shadow-root rule - `:host()` and `::slotted()` - while the element
    /// it styles is outside that subtree, and a same-named document rule must not win over it; then
    /// the scope the element itself is in; then the document.
    #[must_use]
    pub(crate) fn resolve(
        &self,
        declaration_scope: DeclarationScope,
        element_tree_scope: TreeScopeID,
        name: &[u16],
    ) -> Option<&Arc<KeyframeSet>> {
        if self.scopes.is_empty() {
            return None;
        }
        let in_scope = |scope: TreeScopeID| self.scopes.get(&scope)?.get(name);
        let declared = match declaration_scope {
            DeclarationScope::Shadow(scope) => in_scope(scope),
            DeclarationScope::Unscoped => None,
        };
        declared
            .or_else(|| match element_tree_scope {
                TreeScopeID::DOCUMENT => None,
                scope => in_scope(scope),
            })
            .or_else(|| in_scope(TreeScopeID::DOCUMENT))
    }
}

/// The times the engine samples effects' timelines at: those the host sampled them at, or those a clock
/// tick at a timestamp moves the document timelines to, with the scroll timelines at the scroll progress
/// the tick samples them at.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct AnimationTimelineSamples<'a> {
    timestamp: Option<f64>,
    scroll_progress: &'a [ScrollProgress],
}

impl<'a> AnimationTimelineSamples<'a> {
    /// The times a clock tick at `timestamp` samples at, which moves every document timeline: a timestamp less its
    /// timeline's origin time, and every scroll timeline in `scroll_progress`, sorted by timeline, to its progress
    /// there. No other timeline is sampled with it.
    #[must_use]
    pub(crate) fn at_tick(timestamp: f64, scroll_progress: &'a [ScrollProgress]) -> Self {
        Self {
            timestamp: Some(timestamp),
            scroll_progress,
        }
    }

    /// The time `timing`'s timeline is sampled at, which may be unresolved, or `None` where these samples
    /// do not move it.
    #[inline]
    fn timeline_time(self, timing: &CompactEffectTiming) -> Option<Option<TimeValue>> {
        match self.timestamp {
            None => Some(timing.timeline_time()),
            Some(timestamp) => timing.clock().time_at_tick(timestamp, self.scroll_progress).map(Some),
        }
    }
}

/// The timing the host sampled each of an element's effects with, by the effect's identity.
pub(crate) type SampledEffectTimings = Box<[(crate::animations::effect::AnimationEffectId, CompactEffectTiming)]>;

/// One animation effect's timing and easing, as a hover's lane runs a transition of its own.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct EffectTiming {
    pub(crate) timing: CompactEffectTiming,
    pub(crate) easing: crate::css::easing::Easing,
}

impl std::ops::Deref for EffectTiming {
    type Target = CompactEffectTiming;

    fn deref(&self) -> &CompactEffectTiming {
        &self.timing
    }
}

impl std::ops::DerefMut for EffectTiming {
    fn deref_mut(&mut self) -> &mut CompactEffectTiming {
        &mut self.timing
    }
}

impl EffectTiming {
    /// The key the effect's keyframes are sampled at with its timeline at `samples` (see
    /// [`CompactEffectTiming::key_at`]).
    #[must_use]
    pub(crate) fn key_at(&self, samples: AnimationTimelineSamples<'_>) -> Option<Option<f64>> {
        self.timing.key_at(&self.easing, samples)
    }
}

/// What the timing calculations read of an effect at one moment: its timing, the playback state of its animation, its
/// local time, and its timeline then.
#[derive(Clone, Copy)]
struct SampledEffect {
    effect: AnimationEffectTiming,
    playback: AnimationPlayback,
    local_time: Option<TimeValue>,
    timeline: Option<TimelineSample>,
}

impl CompactEffectTiming {
    /// The key the effect samples its keyframes at as the host samples it, eased by `easing`: the one its timing gives
    /// at the time the host sampled its timeline, or none for an unresolved progress, which samples nothing.
    #[must_use]
    pub(crate) fn key(&self, easing: &crate::css::easing::Easing) -> Option<f64> {
        self.key_at(easing, AnimationTimelineSamples::default())
            .expect("the host's samples move every timeline")
    }

    /// The local time of the effect with its timeline at `samples`, in the unit of its timeline's times, or none
    /// where it is unresolved or the samples do not move its timeline.
    #[must_use]
    pub(crate) fn local_time_at(&self, samples: AnimationTimelineSamples) -> Option<f64> {
        let timeline_time = samples.timeline_time(self)?;
        timing::current_time_at(&self.playback(), timeline_time).map(|time| time.value)
    }

    /// Whether the effect's animation is paused as the host sampled it.
    #[must_use]
    pub(crate) fn is_paused(&self) -> bool {
        let playback = self.playback();
        let current_time = timing::current_time_at(&playback, self.timeline_time());
        timing::play_state_at(&playback, current_time, timing::end_time(&self.effect())) == AnimationPlayState::Paused
    }

    /// Holds the effect's animation at `local_time`, in milliseconds.
    pub(crate) fn hold_at(&mut self, local_time: f64) {
        self.set_hold_time(Some(TimeValue::milliseconds(local_time)));
    }

    /// Whether the effect's animation has neither a start time nor a hold time, so that it runs nothing.
    #[must_use]
    pub(crate) fn has_no_time(&self) -> bool {
        let playback = self.playback();
        playback.start_time.get().is_none() && playback.hold_time.get().is_none()
    }

    /// What the timing calculations read of the effect with its timeline at `samples`, or none where the samples do
    /// not move its timeline.
    fn sample_at(&self, samples: AnimationTimelineSamples<'_>) -> Option<SampledEffect> {
        let timeline_time = samples.timeline_time(self)?;
        let effect = self.effect();
        let playback = self.playback();

        // https://www.w3.org/TR/web-animations-1/#animation-current-time
        let unconstrained_time = timing::current_time_at(
            &AnimationPlayback {
                hold_time: OptionalTimeValue::UNRESOLVED,
                ..playback
            },
            timeline_time,
        );
        // https://www.w3.org/TR/web-animations-1/#update-an-animations-finished-state
        // Only a finished animation holds a time with its start time resolved. A tick moves its timeline without the
        // host updating its finished state, which lets go of that hold once the timeline goes back before the end, or,
        // for an animation that plays in reverse, after the start.
        let finish_released = samples.timestamp.is_some()
            && unconstrained_time.is_some_and(|time| {
                if playback.playback_rate > 0.0 {
                    time < timing::end_time(&effect)
                } else {
                    playback.playback_rate < 0.0 && time.value > 0.0
                }
            });
        let local_time = match playback.hold_time.get() {
            Some(hold_time) if !finish_released => Some(hold_time),
            _ => unconstrained_time,
        };
        let timeline = self.timeline().map(|timeline| TimelineSample {
            current_time: timeline_time.into(),
            ..timeline
        });
        Some(SampledEffect {
            effect,
            playback,
            local_time,
            timeline,
        })
    }

    /// The effect's timing, and its phase, active time and overall progress with its timeline at `samples`, or none
    /// where the samples do not move its timeline.
    /// The phase is determined with the endpoint-inclusive active interval flag `endpoint_inclusive`.
    fn resolve_at(
        &self,
        samples: AnimationTimelineSamples<'_>,
        endpoint_inclusive: timing::EndpointInclusiveActiveInterval,
    ) -> Option<(AnimationEffectTiming, ResolvedEffectTiming)> {
        let sampled = self.sample_at(samples)?;
        let resolved = timing::resolve_timing_with(
            &sampled.effect,
            sampled.local_time,
            Some(&sampled.playback),
            sampled.timeline.as_ref(),
            sampled.effect.fill_mode,
            endpoint_inclusive,
        );
        Some((sampled.effect, resolved))
    }

    /// The key the effect's keyframes are sampled at with its timeline at `samples`, eased by `easing`, which is the
    /// transformed progress as a percentage scaled by `keyframes::KEY_SCALE_FACTOR`. The outer `None` is a timeline the
    /// samples do not move; the inner one is an unresolved progress, which samples nothing.
    #[must_use]
    pub(crate) fn key_at(
        &self,
        easing: &crate::css::easing::Easing,
        samples: AnimationTimelineSamples<'_>,
    ) -> Option<Option<f64>> {
        self.key_with(easing, samples, timing::EndpointInclusiveActiveInterval::No)
    }

    /// https://drafts.csswg.org/web-animations-1/#commit-computed-styles
    /// The key the effect's keyframes are sampled at as the host sampled it, eased by `easing`, with the
    /// endpoint-inclusive active interval flag set when determining its phase, as committing computed styles samples
    /// it. None is an unresolved progress, which samples nothing.
    #[must_use]
    pub(crate) fn endpoint_inclusive_key(&self, easing: &crate::css::easing::Easing) -> Option<f64> {
        self.key_with(
            easing,
            AnimationTimelineSamples::default(),
            timing::EndpointInclusiveActiveInterval::Yes,
        )
        .expect("the host's samples move every timeline")
    }

    fn key_with(
        &self,
        easing: &crate::css::easing::Easing,
        samples: AnimationTimelineSamples<'_>,
        endpoint_inclusive: timing::EndpointInclusiveActiveInterval,
    ) -> Option<Option<f64>> {
        let (effect, resolved) = self.resolve_at(samples, endpoint_inclusive)?;
        let Some(output_progress) = timing::transformed_progress(&effect, &resolved, easing) else {
            return Some(None);
        };

        // NB: The transformed progress, scaled as a key, and clamped to what a key can hold.
        let key = crate::animations::keyframes::scaled_offset(output_progress);
        Some(Some(key.clamp(i64::MIN as f64, i64::MAX as f64)))
    }

    /// https://drafts.csswg.org/web-animations-1/#side-effects-of-animation
    /// For every property targeted by at least one animation effect that is current or in effect, and which is associated
    /// with an animation whose replace state is not removed, the user agent must act as if the will-change property on
    /// the effect target includes the property.
    ///
    /// NB: A timeline the samples do not move stands where the host sampled it.
    pub(crate) fn implies_will_change(&self, samples: AnimationTimelineSamples<'_>) -> bool {
        let sampled = self
            .sample_at(samples)
            .or_else(|| self.sample_at(AnimationTimelineSamples::default()))
            .expect("the host's samples move every timeline");
        timing::is_relevant(
            &sampled.playback,
            sampled.timeline.as_ref(),
            Some(&sampled.effect),
            sampled.local_time,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn names(names: &[&str]) -> Box<[CssString]> {
        names
            .iter()
            .map(|name| CssString::from_utf16(&name.encode_utf16().collect::<Vec<_>>()))
            .collect()
    }

    /// Publish a list of animations of the given names, none of which a plan has described.
    fn set(animations: &mut CssDefinedAnimations, node: StyleNodeID, slot: AnimationSlot, names: &[&str]) {
        animations.set(
            node,
            slot,
            self::names(names)
                .into_iter()
                .map(|name| CssDefinedAnimation {
                    name,
                    applied_definition: None,
                })
                .collect(),
        );
    }

    fn list_names(animations: &CssDefinedAnimations, node: StyleNodeID, slot: AnimationSlot) -> Vec<CssString> {
        animations
            .list(node, slot)
            .iter()
            .map(|animation| animation.name.clone())
            .collect()
    }

    use crate::animations::snapshot::{EffectTimingSnapshot, TimelineClock, TimelineClockSource};
    use crate::animations::timeline::AnimationTimelineId;
    use crate::animations::timing::{AnimationEffectTiming, AnimationReplaceState, FillMode};

    impl EffectTiming {
        /// Changes the timing as the host would publish it.
        fn change(&mut self, change: impl FnOnce(&mut EffectTimingSnapshot)) {
            let mut snapshot = self.timing.snapshot();
            change(&mut snapshot);
            self.timing = CompactEffectTiming::from(&snapshot);
        }
    }

    fn ms(time: f64) -> OptionalTimeValue {
        Some(TimeValue::milliseconds(time)).into()
    }

    fn percent(time: f64) -> OptionalTimeValue {
        Some(TimeValue::percentage(time)).into()
    }

    /// A one-second linear animation on a document timeline that started at zero, sampled with its timeline at
    /// `time`.
    fn timing_at(time: f64) -> EffectTiming {
        EffectTiming {
            timing: CompactEffectTiming::from(&EffectTimingSnapshot {
                effect: AnimationEffectTiming {
                    iteration_duration: TimeValue::milliseconds(1000.0),
                    fill_mode: FillMode::None,
                    ..AnimationEffectTiming::default()
                },
                playback: AnimationPlayback {
                    start_time: ms(0.0),
                    ..AnimationPlayback::default()
                },
                timeline: TimelineSample {
                    current_time: ms(time),
                    is_monotonically_increasing: true,
                    ..TimelineSample::default()
                },
                has_timeline: true,
                clock: TimelineClock {
                    source: TimelineClockSource::Document,
                    ..TimelineClock::default()
                },
            }),
            easing: crate::css::easing::Easing::default(),
        }
    }

    /// A linear animation over the whole of a scroll timeline, sampled with its timeline at `progress`.
    fn scroll_timing_at(progress: f64) -> EffectTiming {
        EffectTiming {
            timing: CompactEffectTiming::from(&EffectTimingSnapshot {
                effect: AnimationEffectTiming {
                    start_delay: TimeValue::percentage(0.0),
                    end_delay: TimeValue::percentage(0.0),
                    iteration_duration: TimeValue::percentage(100.0),
                    fill_mode: FillMode::None,
                    ..AnimationEffectTiming::default()
                },
                playback: AnimationPlayback {
                    start_time: percent(0.0),
                    ..AnimationPlayback::default()
                },
                timeline: TimelineSample {
                    current_time: percent(progress),
                    minimum_timeline_time: percent(0.0),
                    maximum_timeline_time: percent(100.0),
                    is_monotonically_increasing: false,
                },
                has_timeline: true,
                clock: TimelineClock {
                    source: TimelineClockSource::Scroll,
                    origin_time: 0.0,
                    scroll_timeline: AnimationTimelineId(7),
                },
            }),
            easing: crate::css::easing::Easing::default(),
        }
    }

    fn key(timing: &EffectTiming) -> Option<Option<f64>> {
        timing.key_at(AnimationTimelineSamples::default())
    }

    #[test]
    fn an_animation_implies_will_change_during_its_delay_and_while_filling_forwards() {
        for fill in [FillMode::None, FillMode::Backwards, FillMode::Forwards, FillMode::Both] {
            let mut animation = timing_at(0.0);
            animation.change(|timing| timing.effect.start_delay = TimeValue::milliseconds(1000.0));
            animation.change(|timing| timing.effect.fill_mode = fill);
            for time in [0.0, 1000.0, 1500.0] {
                assert!(animation.implies_will_change(AnimationTimelineSamples::at_tick(time, &[])));
            }
            for time in [2000.0, 3000.0] {
                assert_eq!(
                    animation.implies_will_change(AnimationTimelineSamples::at_tick(time, &[])),
                    matches!(fill, FillMode::Forwards | FillMode::Both),
                );
            }
        }
    }

    #[test]
    fn reverse_playback_implies_will_change_when_current_or_filling_backwards() {
        let mut animation = timing_at(0.0);
        animation.change(|timing| timing.playback.start_time = ms(5000.0));
        animation.change(|timing| timing.effect.start_delay = TimeValue::milliseconds(1000.0));
        animation.change(|timing| timing.playback.playback_rate = -1.0);
        assert!(animation.implies_will_change(AnimationTimelineSamples::at_tick(2500.0, &[])));
        assert!(animation.implies_will_change(AnimationTimelineSamples::at_tick(3500.0, &[])));
        assert!(!animation.implies_will_change(AnimationTimelineSamples::at_tick(4500.0, &[])));
        animation.change(|timing| timing.effect.fill_mode = FillMode::Backwards);
        assert!(animation.implies_will_change(AnimationTimelineSamples::at_tick(4500.0, &[])));
        animation.change(|timing| timing.playback.replace_state = AnimationReplaceState::Removed);
        assert!(!animation.implies_will_change(AnimationTimelineSamples::at_tick(2500.0, &[])));
        assert!(!animation.implies_will_change(AnimationTimelineSamples::at_tick(4500.0, &[])));
    }

    #[test]
    fn an_animation_on_a_scroll_timeline_implies_will_change_after_its_end() {
        // An animation on a timeline that is not monotonically increasing is current whenever it is not idle, which
        // holds for one past its end without a fill.
        let mut animation = scroll_timing_at(80.0);
        animation.change(|timing| timing.effect.iteration_duration = TimeValue::percentage(50.0));
        assert_eq!(key(&animation), Some(None));
        assert!(animation.implies_will_change(AnimationTimelineSamples::default()));
        let mut idle = animation.clone();
        idle.change(|timing| timing.playback = AnimationPlayback::default());
        assert!(!idle.implies_will_change(AnimationTimelineSamples::default()));
    }

    #[test]
    fn a_timeline_a_tick_does_not_move_implies_will_change_where_the_host_sampled_it() {
        let mut animation = timing_at(500.0);
        animation.change(|timing| timing.clock.source = TimelineClockSource::Unticked);
        assert!(animation.implies_will_change(AnimationTimelineSamples::at_tick(5000.0, &[])));
        assert_eq!(animation.key_at(AnimationTimelineSamples::at_tick(5000.0, &[])), None);
    }

    #[test]
    fn a_key_is_where_the_host_sampled_the_timeline() {
        let timing = timing_at(500.0);
        assert_eq!(timing.timing.key(&timing.easing), Some(50_000.0));
        let mut unresolved = timing_at(500.0);
        unresolved.change(|timing| timing.timeline.current_time = OptionalTimeValue::UNRESOLVED);
        assert_eq!(key(&unresolved), Some(None), "an inactive timeline samples nothing");
    }

    #[test]
    fn a_tick_samples_a_scroll_timeline_where_its_scroller_is_scrolled_to() {
        let on_scroll_timeline = scroll_timing_at(25.0);
        let scrolled = |timeline| {
            [ScrollProgress {
                timeline: AnimationTimelineId(timeline),
                progress: 60.0,
            }]
        };
        assert_eq!(
            key(&on_scroll_timeline),
            Some(Some(25_000.0)),
            "where the host sampled it"
        );
        assert_eq!(
            on_scroll_timeline.key_at(AnimationTimelineSamples::at_tick(1000.0, &scrolled(7))),
            Some(Some(60_000.0))
        );
        assert_eq!(
            on_scroll_timeline.key_at(AnimationTimelineSamples::at_tick(1000.0, &scrolled(8))),
            None,
            "a tick that has no progress for its timeline does not move it"
        );
        let mut finished = on_scroll_timeline.clone();
        finished.change(|timing| timing.playback.hold_time = percent(100.0));
        finished.change(|timing| timing.effect.fill_mode = FillMode::Both);
        assert_eq!(
            finished.key_at(AnimationTimelineSamples::at_tick(1000.0, &scrolled(7))),
            Some(Some(60_000.0)),
            "a finished animation runs again as a tick scrolls back before its end"
        );
        let mut finished_in_reverse = on_scroll_timeline.clone();
        finished_in_reverse.change(|timing| timing.playback.start_time = percent(100.0));
        finished_in_reverse.change(|timing| timing.playback.playback_rate = -1.0);
        finished_in_reverse.change(|timing| timing.playback.hold_time = percent(0.0));
        finished_in_reverse.change(|timing| timing.effect.fill_mode = FillMode::Both);
        assert_eq!(
            finished_in_reverse.key_at(AnimationTimelineSamples::at_tick(1000.0, &scrolled(7))),
            Some(Some(40_000.0)),
            "an animation finished in reverse runs again as a tick scrolls back after its start"
        );
        let mut paused = finished.clone();
        paused.change(|timing| timing.playback.start_time = OptionalTimeValue::UNRESOLVED);
        paused.change(|timing| timing.playback.hold_time = percent(40.0));
        assert!(paused.is_paused());
        assert_eq!(
            paused.key_at(AnimationTimelineSamples::at_tick(1000.0, &scrolled(7))),
            Some(Some(40_000.0)),
            "a paused animation holds"
        );
    }

    #[test]
    fn an_attachment_range_shortens_a_scroll_timeline_animation() {
        // An animation attached to the range from 20% to 70% of its timeline.
        let mut timing = scroll_timing_at(45.0);
        timing.change(|timing| timing.effect.iteration_duration = TimeValue::percentage(50.0));
        timing.change(|timing| timing.playback.start_time = percent(20.0));
        assert_eq!(key(&timing), Some(Some(50_000.0)));
    }

    #[test]
    fn a_list_is_replaced_per_slot_and_dropped_when_empty() {
        let node = StyleNodeID::from_raw(1).unwrap();
        let mut animations = CssDefinedAnimations::default();
        set(&mut animations, node, 0, &["a", "b"]);
        set(&mut animations, node, 3, &["c"]);
        set(&mut animations, node, 0, &["b"]);
        assert_eq!(list_names(&animations, node, 0), &names(&["b"])[..]);
        assert_eq!(list_names(&animations, node, 3), &names(&["c"])[..]);

        set(&mut animations, node, 0, &[]);
        assert!(animations.list(node, 0).is_empty());
        set(&mut animations, node, 3, &[]);
        assert!(animations.rows.is_empty());
    }

    fn matches(existing: &[&str], definitions: &[&str]) -> Vec<Option<usize>> {
        let node = StyleNodeID::from_raw(1).unwrap();
        let mut animations = CssDefinedAnimations::default();
        set(&mut animations, node, 0, existing);
        let existing = animations.list(node, 0);
        let definitions = names(definitions);
        let definitions: Vec<&CssString> = definitions.iter().collect();
        let mut matches = vec![None; definitions.len()];
        match_existing_animations(existing, &definitions, |definition, animation| {
            matches[definition] = Some(animation);
        });
        matches
    }

    #[test]
    fn an_empty_existing_list_creates_every_animation() {
        assert_eq!(matches(&[], &["a", "b"]), [None, None]);
    }

    #[test]
    fn a_repeated_name_takes_the_last_unclaimed_animation_first() {
        // `a` becoming `a, a` keeps the existing animation as the second entry and creates the first.
        assert_eq!(matches(&["a"], &["a", "a"]), [None, Some(0)]);
        assert_eq!(matches(&["a", "a"], &["a", "a"]), [Some(0), Some(1)]);
        assert_eq!(matches(&["a", "a"], &["a"]), [Some(1)]);
    }

    #[test]
    fn a_removed_name_claims_nothing() {
        assert_eq!(matches(&["a", "b"], &["b"]), [Some(1)]);
        assert_eq!(matches(&["a", "b"], &["c"]), [None]);
    }

    #[test]
    fn a_reordered_list_claims_by_name() {
        assert_eq!(matches(&["a", "b", "c"], &["c", "a", "b"]), [Some(2), Some(0), Some(1)]);
    }

    #[test]
    fn a_retired_identity_holds_no_lists_when_reissued() {
        let node = StyleNodeID::from_raw(1).unwrap();
        let mut animations = CssDefinedAnimations::default();
        set(&mut animations, node, 0, &["a"]);
        set(&mut animations, node, 2, &["b"]);
        animations.retire(node);
        assert!(animations.list(node, 0).is_empty());
        assert!(animations.list(node, 2).is_empty());
        assert!(animations.rows.is_empty());
    }

    fn publish(keyframes: &mut AnimationKeyframes, scope: u32, rows: &[(&str, &Arc<KeyframeSet>)]) {
        let lengths: Vec<u32> = rows
            .iter()
            .map(|(name, _)| name.encode_utf16().count() as u32)
            .collect();
        let units: Vec<u16> = rows.iter().flat_map(|(name, _)| name.encode_utf16()).collect();
        let sets = rows.iter().map(|&(_, set)| set.clone()).collect();
        keyframes.set(TreeScopeID(scope), AnimationKeyframes::row(&lengths, &units, sets));
    }

    fn resolve(
        keyframes: &AnimationKeyframes,
        declaration: Option<u32>,
        scope: u32,
        name: &str,
    ) -> Option<*const KeyframeSet> {
        let declaration_scope = declaration.map_or(DeclarationScope::Unscoped, |scope| {
            DeclarationScope::Shadow(TreeScopeID(scope))
        });
        keyframes
            .resolve(
                declaration_scope,
                TreeScopeID(scope),
                &name.encode_utf16().collect::<Vec<_>>(),
            )
            .map(Arc::as_ptr)
    }

    fn keyframe_sets<const N: usize>() -> [Arc<KeyframeSet>; N] {
        std::array::from_fn(|_| Arc::new(KeyframeSet::from_processed_keyframes(Vec::new(), None)))
    }

    fn address(set: &Arc<KeyframeSet>) -> Option<*const KeyframeSet> {
        Some(Arc::as_ptr(set))
    }

    #[test]
    fn keyframes_resolve_in_the_declaration_scope_then_the_element_scope_then_the_document() {
        let [one, two, three, four, five] = keyframe_sets();
        let mut keyframes = AnimationKeyframes::default();
        publish(&mut keyframes, 0, &[("a", &one), ("b", &two)]);
        publish(&mut keyframes, 1, &[("a", &three)]);
        publish(&mut keyframes, 2, &[("a", &four), ("b", &five)]);

        assert_eq!(resolve(&keyframes, None, 0, "a"), address(&one));
        assert_eq!(resolve(&keyframes, None, 1, "a"), address(&three));
        assert_eq!(resolve(&keyframes, None, 1, "b"), address(&two));
        // A `:host` rule of scope 2 styling an element of scope 1.
        assert_eq!(resolve(&keyframes, Some(2), 1, "a"), address(&four));
        assert_eq!(resolve(&keyframes, Some(2), 1, "c"), None);
    }

    #[test]
    fn a_departed_scope_gives_up_its_row() {
        let [three, four] = keyframe_sets();
        let mut keyframes = AnimationKeyframes::default();
        publish(&mut keyframes, 1, &[("a", &three)]);
        publish(&mut keyframes, 2, &[("a", &four)]);
        publish(&mut keyframes, 1, &[]);
        assert_eq!(keyframes.scopes.len(), 1);
        assert_eq!(resolve(&keyframes, Some(2), 0, "a"), address(&four));

        publish(&mut keyframes, 2, &[]);
        assert!(keyframes.scopes.is_empty());
    }
}
