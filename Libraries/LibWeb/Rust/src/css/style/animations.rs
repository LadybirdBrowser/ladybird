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

use super::bridge::{FfiAppliedAnimationDefinition, FfiAppliedAnimationValues};
use super::tree::{StyleNodeID, TreeScopeID};
use crate::css::css_string::CssString;
use crate::css::style_compute::FfiEffectTiming;
use std::collections::HashMap;

/// Which of an element's animation lists a row belongs to, in the host's own numbering: zero for
/// the element itself, and the pseudo-element's value plus one for each pseudo-element.
pub(crate) type AnimationSlot = u8;

/// The definition that claimed no existing animation and asks for a new one.
pub(crate) const NO_MATCHED_ANIMATION: i32 = -1;

impl FfiAppliedAnimationDefinition {
    /// What the host publishes back for a definition once it has applied it: the definition's own
    /// values, field for field.
    #[must_use]
    fn of(animation: &crate::css::style_compute::FfiComputedAnimation) -> Self {
        Self {
            values: FfiAppliedAnimationValues {
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
            },
            keyframe_set: animation.keyframe_set,
            timing_function: animation.timing_function,
        }
    }

    /// Whether applying `self` to an animation that last had `published` applied would leave it
    /// exactly as it is. A scroll timeline is rebuilt from the element's surroundings every time it
    /// is applied, so a definition that names one is never called unchanged, and an animation no
    /// plan has described yet publishes a null timing function, which no computed definition has.
    /// The timing function is compared by value: a recomputed style builds a fresh allocation for
    /// a declaration that has not changed.
    #[must_use]
    fn would_change_nothing(&self, published: &Self) -> bool {
        self.values.timeline_kind != crate::css::style_compute::FfiAnimationTimelineKind::Scroll as u8
            && !published.timing_function.is_null()
            && self.values == published.values
            && self.keyframe_set == published.keyframe_set
            && unsafe {
                crate::css::style_value::rust_style_value_equals(
                    self.timing_function.cast(),
                    published.timing_function.cast(),
                )
            }
    }
}

/// One of the CSS animations the host holds: its name, and the definition the last plan applied
/// to it.
#[derive(Clone)]
pub(crate) struct CssDefinedAnimation {
    pub(crate) name: CssString,
    applied_definition: FfiAppliedAnimationDefinition,
}

/// The CSS animations the host holds in one of an element's lists, in its order.
type CssDefinedAnimationList = (AnimationSlot, Box<[CssDefinedAnimation]>);

/// Per element, the CSS animations the host holds for it and each of its pseudo-elements, in the
/// order the host holds them.
#[derive(Clone, Default)]
pub(crate) struct CssDefinedAnimations {
    /// Owning a CSS animation is rare, so only the elements that do have a row, and a row holds
    /// only the lists that are not empty.
    rows: HashMap<StyleNodeID, Vec<CssDefinedAnimationList>>,
}

impl CssDefinedAnimations {
    /// Replace one list from the names and applied definitions the host publishes, a definition per
    /// animation. An empty list drops it, so an element that stops animating stops costing anything.
    pub(crate) fn set(
        &mut self,
        node: StyleNodeID,
        slot: AnimationSlot,
        names: Box<[CssString]>,
        definitions: &[FfiAppliedAnimationDefinition],
    ) {
        debug_assert_eq!(
            definitions.len(),
            names.len(),
            "every published animation name comes with its applied definition"
        );
        let animations: Box<[CssDefinedAnimation]> = names
            .into_iter()
            .zip(definitions)
            .map(|(name, &applied_definition)| CssDefinedAnimation {
                name,
                applied_definition,
            })
            .collect();
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
                    && FfiAppliedAnimationDefinition::of(definition).would_change_nothing(&animation.applied_definition)
            })
}

/// The tree scope an `animation-name` declaration was written in, where the `@keyframes` it names
/// are looked for first.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum DeclarationScope {
    /// A declaration of the document, of another origin or of the element itself, or none at all:
    /// none of these has a scope of its own.
    Unscoped,
    /// A rule of a style sheet a shadow root's scope holds.
    Shadow(TreeScopeID),
}

/// One style scope's `@keyframes`: the host's keyframe set for each name the scope defines.
pub(crate) type KeyframesRow = HashMap<Box<[u16]>, usize>;

/// The `@keyframes` every style scope of the document defines, as each scope's rule cache resolved
/// them, with the host's keyframe set for each name.
///
/// A scope publishes its row whenever its rule cache is built, and the host builds every scope's
/// cache before a style transaction, so resolving an animation's keyframes is a lookup here rather
/// than building a rule cache in the middle of a style computation. A keyframe set is the host's
/// refcounted object, borrowed: the scope keeps a reference to every set its row names until it
/// publishes the row again or gives it up, which the engine takes in ahead of anything that
/// resolves keyframes.
#[derive(Clone, Default)]
pub(crate) struct AnimationKeyframes {
    scopes: HashMap<TreeScopeID, KeyframesRow>,
    /// Which scope a shadow root's pointer identity names. The cascade attributes the winning
    /// `animation-name` declaration to a shadow root by that identity, and the scope it names is
    /// where the declaration's `@keyframes` are looked for first.
    scope_by_shadow_root: HashMap<usize, TreeScopeID>,
}

impl AnimationKeyframes {
    /// The row of the names, packed into one buffer of code units with a length each, the way an
    /// element's animation names arrive, and the host's keyframe set for each.
    pub(crate) fn row(name_lengths: &[u32], name_units: &[u16], keyframe_sets: &[usize]) -> KeyframesRow {
        debug_assert_eq!(name_lengths.len(), keyframe_sets.len());
        let mut offset = 0;
        name_lengths
            .iter()
            .zip(keyframe_sets)
            .map(|(&length, &set)| {
                let units = &name_units[offset..offset + length as usize];
                offset += length as usize;
                (Box::from(units), set)
            })
            .collect()
    }

    /// Replaces one scope's row. An empty row gives the scope's row up.
    pub(crate) fn set(&mut self, tree_scope: TreeScopeID, shadow_root_identity: usize, row: KeyframesRow) {
        if row.is_empty() {
            self.scopes.remove(&tree_scope);
            // A scope with no row answers like one that defines nothing, so its identity stops
            // naming it, unless another shadow root has since been allocated at that address and
            // published under it.
            if self.scope_by_shadow_root.get(&shadow_root_identity) == Some(&tree_scope) {
                self.scope_by_shadow_root.remove(&shadow_root_identity);
            }
            return;
        }
        if shadow_root_identity != 0 {
            self.scope_by_shadow_root.insert(shadow_root_identity, tree_scope);
        }
        self.scopes.insert(tree_scope, row);
    }

    /// The host's keyframe set an animation of this name runs, or `None` where no scope in its chain
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
    ) -> Option<usize> {
        if self.scopes.is_empty() {
            return None;
        }
        let in_scope = |scope: TreeScopeID| self.scopes.get(&scope)?.get(name).copied();
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

/// `Bindings::FillMode`, in IDL order.
mod fill_mode {
    pub(super) const FORWARDS: u8 = 1;
    pub(super) const BACKWARDS: u8 = 2;
    pub(super) const BOTH: u8 = 3;
}

/// `Bindings::PlaybackDirection`, in IDL order.
mod playback_direction {
    pub(super) const NORMAL: u8 = 0;
    pub(super) const REVERSE: u8 = 1;
    pub(super) const ALTERNATE_REVERSE: u8 = 3;
}

/// The times the engine samples effects' timelines at: those the host sampled them at, or those a clock
/// tick at a timestamp moves the document timelines to, with the scroll timelines at the scroll progress
/// the tick samples them at.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct AnimationTimelineSamples<'a> {
    timestamp: Option<f64>,
    scroll_progress: &'a [ScrollProgress],
}

/// The scroll progress, in percent, of the scroll node of an element or of a document's viewport, by its
/// unique node id, along one axis: the time of a scroll timeline that follows it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct ScrollProgress {
    pub(crate) scroller: i64,
    pub(crate) vertical: bool,
    pub(crate) progress: f64,
}

impl<'a> AnimationTimelineSamples<'a> {
    /// The times a clock tick at `timestamp` samples at, which moves every document timeline: a timestamp less its
    /// timeline's origin time, and every scroll timeline that follows a scroller in `scroll_progress` to its progress
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
    fn timeline_time(self, timing: &FfiEffectTiming) -> Option<Option<f64>> {
        match self.timestamp {
            None => Some(timing.has_timeline_time.then_some(timing.timeline_time)),
            Some(_) if timing.has_timeline_scroller => self
                .scroll_progress
                .iter()
                .find(|scroll| {
                    scroll.scroller == timing.timeline_scroller
                        && scroll.vertical == timing.timeline_scroller_is_vertical
                })
                .map(|scroll| Some(scroll.progress)),
            Some(timestamp) => timing
                .has_timeline_origin_time
                .then_some(Some(timestamp - timing.timeline_origin_time)),
        }
    }
}

/// `AnimationEffect::Phase`.
#[derive(Clone, Copy, PartialEq)]
enum Phase {
    Before,
    Active,
    After,
    Idle,
}

/// `AK::max` and `AK::min`, which keep the first value where neither is less.
fn largest(a: f64, b: f64) -> f64 {
    if a < b { b } else { a }
}

fn smallest(a: f64, b: f64) -> f64 {
    if b < a { b } else { a }
}

/// The timing the host sampled each of an element's effects with, by the effect's identity.
pub(crate) type SampledEffectTimings = Box<[(u64, EffectTiming)]>;

/// One animation effect's timing and easing, as the host last sampled the effect.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct EffectTiming {
    pub(crate) timing: FfiEffectTiming,
    pub(crate) easing: crate::css::easing::Easing,
}

impl EffectTiming {
    /// The key the effect samples its keyframes at as the host samples it: the one its timing gives at
    /// the time the host sampled its timeline, or `host_key` where the engine cannot decide the timing,
    /// or none for an unresolved progress, which samples nothing.
    #[must_use]
    pub(crate) fn key(&self, host_key: f64) -> Option<f64> {
        self.key_at(AnimationTimelineSamples::default())
            .unwrap_or(Some(host_key))
    }

    /// The local time of the effect with its timeline at `samples`, or none where it is unresolved or the engine cannot
    /// decide the timing.
    /// https://www.w3.org/TR/web-animations-1/#local-time
    #[must_use]
    pub(crate) fn local_time_at(&self, samples: AnimationTimelineSamples) -> Option<f64> {
        let timing = &self.timing;
        if !timing.decidable {
            return None;
        }
        match (timing.has_hold_time, samples.timeline_time(timing)?) {
            (true, _) => Some(timing.hold_time),
            (false, Some(timeline_time)) if timing.has_start_time => {
                Some((timeline_time - timing.start_time) * timing.playback_rate)
            }
            (false, _) => None,
        }
    }

    fn active_time_at(&self, samples: AnimationTimelineSamples<'_>) -> Option<(Phase, Option<f64>, f64)> {
        let timing = &self.timing;
        if !timing.decidable {
            return None;
        }
        let timeline_time = samples.timeline_time(timing)?;

        // https://www.w3.org/TR/web-animations-1/#active-duration
        let active_duration = match timing.iteration_duration == 0.0 || timing.iteration_count == 0.0 {
            true => 0.0,
            false => timing.iteration_duration * timing.iteration_count,
        };
        // https://www.w3.org/TR/web-animations-1/#end-time
        let end_time = largest(timing.start_delay + active_duration + timing.end_delay, 0.0);

        // https://www.w3.org/TR/web-animations-1/#animation-current-time
        let unconstrained_time = timeline_time
            .filter(|_| timing.has_start_time)
            .map(|timeline_time| (timeline_time - timing.start_time) * timing.playback_rate);
        // https://www.w3.org/TR/web-animations-1/#update-an-animations-finished-state
        // Only a finished animation holds a time with its start time resolved. A tick moves its timeline without the
        // host updating its finished state, which lets go of that hold once the timeline goes back before the end.
        let finish_released = samples.timestamp.is_some()
            && timing.playback_rate > 0.0
            && unconstrained_time.is_some_and(|time| time < end_time);
        let local_time = match timing.has_hold_time && !finish_released {
            true => Some(timing.hold_time),
            false => unconstrained_time,
        };

        // https://www.w3.org/TR/web-animations-1/#animation-effect-phases-and-states
        let backwards = timing.playback_rate < 0.0;
        let phase = match local_time {
            None => Phase::Idle,
            Some(local_time) => {
                let before_active_boundary_time = largest(smallest(timing.start_delay, end_time), 0.0);
                let after_active_boundary_time = largest(smallest(timing.start_delay + active_duration, end_time), 0.0);
                if local_time < before_active_boundary_time || (backwards && local_time == before_active_boundary_time)
                {
                    Phase::Before
                } else if local_time > after_active_boundary_time
                    || (!backwards && local_time == after_active_boundary_time)
                {
                    Phase::After
                } else {
                    Phase::Active
                }
            }
        };

        // https://www.w3.org/TR/web-animations-1/#calculating-the-active-time
        let fills = |mode| timing.fill_mode == mode || timing.fill_mode == fill_mode::BOTH;
        let active_time = match (phase, local_time) {
            (Phase::Before, Some(local_time)) if fills(fill_mode::BACKWARDS) => {
                Some(largest(local_time - timing.start_delay, 0.0))
            }
            (Phase::Active, Some(local_time)) => Some(local_time - timing.start_delay),
            (Phase::After, Some(local_time)) if fills(fill_mode::FORWARDS) => {
                Some(largest(smallest(local_time - timing.start_delay, active_duration), 0.0))
            }
            _ => None,
        };
        Some((phase, active_time, active_duration))
    }

    /// The key the effect's keyframes are sampled at with its timeline at `samples`, which is
    /// `AnimationEffect::transformed_progress()` scaled the way the host scales it. The outer `None` is
    /// a timing the engine cannot decide; the inner one is an unresolved progress, which samples nothing.
    ///
    /// A mirror of `Animation::current_time_at()`, `AnimationEffect::resolve_timing()` and
    /// `transformed_progress()` with everything under it.
    #[must_use]
    pub(crate) fn key_at(&self, samples: AnimationTimelineSamples<'_>) -> Option<Option<f64>> {
        let (phase, active_time, active_duration) = self.active_time_at(samples)?;
        let Some(active_time) = active_time else {
            return Some(None);
        };
        let timing = &self.timing;

        // https://www.w3.org/TR/web-animations-1/#overall-progress
        let overall_progress = match timing.iteration_duration == 0.0 {
            true if phase == Phase::Before => 0.0,
            true => timing.iteration_count,
            false => active_time / timing.iteration_duration,
        } + timing.iteration_start;

        // https://www.w3.org/TR/web-animations-1/#simple-iteration-progress
        let mut simple_iteration_progress = match overall_progress.is_infinite() {
            true => timing.iteration_start % 1.0,
            false => overall_progress % 1.0,
        };
        if simple_iteration_progress == 0.0
            && matches!(phase, Phase::Active | Phase::After)
            && active_time == active_duration
            && timing.iteration_count != 0.0
        {
            simple_iteration_progress = 1.0;
        }

        // https://www.w3.org/TR/web-animations-1/#current-iteration
        let current_iteration = if phase == Phase::After && timing.iteration_count.is_infinite() {
            timing.iteration_count
        } else if simple_iteration_progress == 1.0 {
            overall_progress.floor() - 1.0
        } else {
            overall_progress.floor()
        };

        // https://www.w3.org/TR/web-animations-1/#directed-progress
        let going_forwards = match timing.playback_direction {
            playback_direction::NORMAL => true,
            playback_direction::REVERSE => false,
            direction => {
                let iteration = match direction == playback_direction::ALTERNATE_REVERSE {
                    true => current_iteration + 1.0,
                    false => current_iteration,
                };
                iteration.is_infinite() || iteration % 2.0 == 0.0
            }
        };
        let directed_progress = match going_forwards {
            true => simple_iteration_progress,
            false => 1.0 - simple_iteration_progress,
        };

        // https://www.w3.org/TR/web-animations-1/#transformed-progress
        let before_flag = (phase == Phase::Before && going_forwards) || (phase == Phase::After && !going_forwards);
        let output_progress = self.easing.evaluate_at(directed_progress, before_flag);

        // `KeyframeEffect::AnimationKeyFrameKeyScaleFactor`, and the host's clamp to what a key can hold.
        let key = output_progress * 100.0 * 1000.0;
        Some(Some(key.clamp(i64::MIN as f64, i64::MAX as f64)))
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

    /// Publish a list of animations of the given names, each with an applied definition no plan
    /// has described.
    fn set(animations: &mut CssDefinedAnimations, node: StyleNodeID, slot: AnimationSlot, names: &[&str]) {
        let undescribed = FfiAppliedAnimationDefinition {
            values: FfiAppliedAnimationValues {
                duration_is_auto: false,
                duration: 0.0,
                iteration_count: 0.0,
                direction: 0,
                play_state: 0,
                delay: 0.0,
                fill_mode: 0,
                composition: 0,
                timeline_kind: 0,
                scroll_scroller: 0,
                scroll_axis: 0,
            },
            keyframe_set: std::ptr::null(),
            timing_function: std::ptr::null(),
        };
        animations.set(node, slot, self::names(names), &vec![undescribed; names.len()]);
    }

    fn list_names(animations: &CssDefinedAnimations, node: StyleNodeID, slot: AnimationSlot) -> Vec<CssString> {
        animations
            .list(node, slot)
            .iter()
            .map(|animation| animation.name.clone())
            .collect()
    }

    /// A one-second linear animation that started at zero, sampled with its timeline at `time`.
    fn timing_at(time: f64) -> EffectTiming {
        EffectTiming {
            timing: FfiEffectTiming {
                decidable: true,
                has_timeline_origin_time: true,
                has_timeline_time: true,
                has_start_time: true,
                timeline_time: time,
                playback_rate: 1.0,
                iteration_duration: 1000.0,
                iteration_count: 1.0,
                ..FfiEffectTiming::default()
            },
            easing: crate::css::easing::Easing::default(),
        }
    }

    fn key(timing: &EffectTiming) -> Option<Option<f64>> {
        timing.key_at(AnimationTimelineSamples::default())
    }

    #[test]
    fn a_key_follows_the_timeline_through_the_active_phase() {
        assert_eq!(key(&timing_at(250.0)), Some(Some(25_000.0)));
        assert_eq!(key(&timing_at(0.0)), Some(Some(0.0)));
        assert_eq!(key(&timing_at(1500.0)), Some(None), "no fill after the end");
        assert_eq!(key(&timing_at(-1.0)), Some(None), "no fill before the start");
    }

    #[test]
    fn a_key_fills_and_holds() {
        let mut timing = timing_at(1500.0);
        timing.timing.fill_mode = fill_mode::BOTH;
        assert_eq!(key(&timing), Some(Some(100_000.0)));
        timing.timing.timeline_time = -500.0;
        assert_eq!(key(&timing), Some(Some(0.0)));
        timing.timing.has_hold_time = true;
        timing.timing.hold_time = 500.0;
        assert_eq!(key(&timing), Some(Some(50_000.0)), "a hold time wins over the timeline");
    }

    #[test]
    fn a_key_runs_each_iteration_in_its_direction() {
        let mut timing = timing_at(1250.0);
        timing.timing.iteration_count = f64::INFINITY;
        assert_eq!(key(&timing), Some(Some(25_000.0)));
        timing.timing.playback_direction = 2;
        assert_eq!(
            key(&timing),
            Some(Some(75_000.0)),
            "an alternate second iteration runs backwards"
        );
        timing.timing.playback_direction = playback_direction::REVERSE;
        assert_eq!(key(&timing), Some(Some(75_000.0)));
        timing.timing.playback_direction = playback_direction::NORMAL;
        timing.timing.playback_rate = 2.0;
        assert_eq!(key(&timing), Some(Some(50_000.0)));
    }

    #[test]
    fn a_key_is_eased_by_the_effect() {
        let mut timing = timing_at(500.0);
        timing.easing = crate::css::easing::Easing::Steps {
            interval_count: 2,
            position: crate::css::easing::STEP_POSITION_JUMP_START,
        };
        assert_eq!(key(&timing), Some(Some(100_000.0)));
    }

    #[test]
    fn an_undecidable_timing_leaves_the_key_to_the_host() {
        let mut timing = timing_at(500.0);
        timing.timing.decidable = false;
        assert_eq!(key(&timing), None);
        let unresolved = EffectTiming {
            timing: FfiEffectTiming {
                has_timeline_time: false,
                ..timing_at(500.0).timing
            },
            ..timing_at(500.0)
        };
        assert_eq!(key(&unresolved), Some(None), "an inactive timeline samples nothing");
    }

    #[test]
    fn a_tick_samples_a_scroll_timeline_where_its_scroller_is_scrolled_to() {
        let on_scroll_timeline = EffectTiming {
            timing: FfiEffectTiming {
                has_timeline_origin_time: false,
                has_timeline_scroller: true,
                timeline_scroller_is_vertical: true,
                timeline_scroller: 7,
                iteration_duration: 100.0,
                ..timing_at(25.0).timing
            },
            ..timing_at(25.0)
        };
        let scrolled = |vertical| {
            [ScrollProgress {
                scroller: 7,
                vertical,
                progress: 60.0,
            }]
        };
        assert_eq!(
            key(&on_scroll_timeline),
            Some(Some(25_000.0)),
            "where the host sampled it"
        );
        assert_eq!(
            on_scroll_timeline.key_at(AnimationTimelineSamples::at_tick(1000.0, &scrolled(true))),
            Some(Some(60_000.0))
        );
        assert_eq!(
            on_scroll_timeline.key_at(AnimationTimelineSamples::at_tick(1000.0, &scrolled(false))),
            None,
            "a tick that has no progress for its scroller does not move it"
        );
        let finished = EffectTiming {
            timing: FfiEffectTiming {
                has_hold_time: true,
                hold_time: 100.0,
                fill_mode: fill_mode::BOTH,
                ..on_scroll_timeline.timing
            },
            ..on_scroll_timeline
        };
        assert_eq!(
            finished.key_at(AnimationTimelineSamples::at_tick(1000.0, &scrolled(true))),
            Some(Some(60_000.0)),
            "a finished animation runs again as a tick scrolls back before its end"
        );
        let paused = EffectTiming {
            timing: FfiEffectTiming {
                has_start_time: false,
                hold_time: 40.0,
                ..finished.timing
            },
            ..finished
        };
        assert_eq!(
            paused.key_at(AnimationTimelineSamples::at_tick(1000.0, &scrolled(true))),
            Some(Some(40_000.0)),
            "a paused animation holds"
        );
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

    fn publish(keyframes: &mut AnimationKeyframes, scope: u32, identity: usize, rows: &[(&str, usize)]) {
        let lengths: Vec<u32> = rows
            .iter()
            .map(|(name, _)| name.encode_utf16().count() as u32)
            .collect();
        let units: Vec<u16> = rows.iter().flat_map(|(name, _)| name.encode_utf16()).collect();
        let sets: Vec<usize> = rows.iter().map(|&(_, set)| set).collect();
        keyframes.set(
            TreeScopeID(scope),
            identity,
            AnimationKeyframes::row(&lengths, &units, &sets),
        );
    }

    fn resolve(keyframes: &AnimationKeyframes, identity: usize, scope: u32, name: &str) -> Option<usize> {
        let declaration_scope = keyframes
            .scope_by_shadow_root
            .get(&identity)
            .map_or(DeclarationScope::Unscoped, |&scope| DeclarationScope::Shadow(scope));
        keyframes.resolve(
            declaration_scope,
            TreeScopeID(scope),
            &name.encode_utf16().collect::<Vec<_>>(),
        )
    }

    #[test]
    fn keyframes_resolve_in_the_declaration_scope_then_the_element_scope_then_the_document() {
        let mut keyframes = AnimationKeyframes::default();
        publish(&mut keyframes, 0, 0, &[("a", 1), ("b", 2)]);
        publish(&mut keyframes, 1, 0x100, &[("a", 3)]);
        publish(&mut keyframes, 2, 0x200, &[("a", 4), ("b", 5)]);

        assert_eq!(resolve(&keyframes, 0, 0, "a"), Some(1));
        assert_eq!(resolve(&keyframes, 0, 1, "a"), Some(3));
        assert_eq!(resolve(&keyframes, 0, 1, "b"), Some(2));
        // A `:host` rule of scope 2 styling an element of scope 1.
        assert_eq!(resolve(&keyframes, 0x200, 1, "a"), Some(4));
        assert_eq!(resolve(&keyframes, 0x200, 1, "c"), None);
    }

    #[test]
    fn a_departed_scope_gives_up_its_row_but_not_a_reused_identity() {
        let mut keyframes = AnimationKeyframes::default();
        publish(&mut keyframes, 1, 0x100, &[("a", 3)]);
        // Another shadow root allocated at the same address publishes before the first one's row
        // is given up.
        publish(&mut keyframes, 2, 0x100, &[("a", 4)]);
        publish(&mut keyframes, 1, 0x100, &[]);
        assert_eq!(keyframes.scopes.len(), 1);
        assert_eq!(resolve(&keyframes, 0x100, 0, "a"), Some(4));

        publish(&mut keyframes, 2, 0x100, &[]);
        assert!(keyframes.scopes.is_empty());
        assert!(keyframes.scope_by_shadow_root.is_empty());
    }
}
