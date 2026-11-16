/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The animation effects an element holds, described for the style engine.
//!
//! Sampling an element's animations interpolates what its effects' keyframes declare. A description
//! holds the keyframe set its effect runs, which it shares with everything else that runs it, beside
//! what depends on the effect: the composite operation and the easing a keyframe whose own is `auto`
//! or missing runs instead, which a sample resolves per keyframe. What depends on the element, a
//! value or an easing still to be substituted against it, travels as written in the set.

use super::animations::{AnimationSlot, EffectTiming};
use super::tree::StyleNodeID;
use crate::animations::easing::{CompactEasing, KEYWORD_CUBIC_BEZIERS, keyword_cubic_bezier};
use crate::animations::effect::AnimationEffectId;
use crate::animations::keyframes::{
    CompositeOperationOrAuto, Keyframe, KeyframeCustomDeclaration, KeyframeDeclaration, KeyframeSet, KeyframeValue,
    ResourceContext,
};
use crate::animations::snapshot::{CompactEffectTiming, EffectTimingSnapshot};
use crate::css::animation::{CustomPropertyInterval, FfiCompositeOperation};
use crate::css::custom_properties::CustomPropertyRegistry;
use crate::css::easing::{Easing, FfiLinearEasingPoint};
use crate::css::style_value::EasingPayload;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
use std::collections::HashMap;
use std::sync::{Arc, Mutex};

impl EffectDescription {
    /// The values the effect's keyframes declare, and those they run their easing by.
    pub(crate) fn declared_values(&self) -> impl Iterator<Item = &StyleValueData> {
        self.sampled_keyframes()
            .into_iter()
            .flat_map(|keyframes| keyframes.declared_values())
    }
}

/// The easing a computed `animation-timing-function` value describes, as the host's
/// `EasingFunction::from_style_value` reads it, or `None` for a value that describes none.
pub(crate) fn easing_from_computed_timing_function(value: &StyleValueData) -> Option<Easing> {
    use crate::css::css_enums::keyword;
    let cubic_bezier = |x1, y1, x2, y2| Easing::CubicBezier { x1, y1, x2, y2 };
    let StyleValueData::Easing(easing_payload) = value else {
        let StyleValueData::Keyword { keyword } = value else {
            return None;
        };
        if *keyword == keyword::LINEAR {
            return Some(Easing::default());
        }
        return KEYWORD_CUBIC_BEZIERS
            .iter()
            .position(|&(cubic_bezier_keyword, _)| cubic_bezier_keyword == *keyword)
            .map(keyword_cubic_bezier);
    };
    let EasingPayload {
        kind,
        step_position,
        x1,
        y1,
        x2,
        y2,
        number_of_intervals,
        ..
    } = &**easing_payload;
    // Each argument reads the way the host's `numeric()` reads it, which resolves a calculation on
    // the spot.
    let numeric = |value: &RetainedStyleValueData| match value.data() {
        StyleValueData::Number { value } => Some(*value),
        StyleValueData::Integer { value } => Some(*value as f64),
        StyleValueData::Percentage { value } => Some(*value),
        calculated @ StyleValueData::Calculated { .. } => {
            crate::css::calc::resolve_calculated_number_without_context(calculated)
                .or_else(|| crate::css::calc::resolve_calculated_percentage_without_context(calculated))
        }
        _ => None,
    };
    match kind {
        0 => {
            // The stops are canonicalized first, which resolves each one's calculated values and
            // interpolates the inputs it was not given.
            let canonical = crate::css::absolutize::canonicalize_linear_easing(value);
            let StyleValueData::Easing(easing_payload) = canonical.data() else {
                return None;
            };
            let EasingPayload { linear_stops, .. } = &**easing_payload;
            Some(Easing::Linear(
                linear_stops
                    .as_slice()
                    .iter()
                    .map(|stop| {
                        Some(FfiLinearEasingPoint {
                            input: numeric(stop.input())? / 100.0,
                            output: numeric(stop.output())?,
                        })
                    })
                    .collect::<Option<_>>()?,
            ))
        }
        1 => Some(cubic_bezier(numeric(x1)?, numeric(y1)?, numeric(x2)?, numeric(y2)?)),
        2 => Some(Easing::Steps {
            #[expect(clippy::cast_possible_truncation)]
            interval_count: numeric(number_of_intervals)?.round_ties_even() as i32,
            position: *step_position,
        }),
        _ => None,
    }
}

/// Maps an interval's declaration indices to its substituted values.
type CustomPropertyIntervalValues = HashMap<[usize; 2], [RetainedStyleValueData; 2]>;

/// What an effect's animation is, as far as sampling the effect goes.
#[derive(Clone)]
pub(crate) enum EffectAnimation {
    /// No animation: the effect is associated with none, and samples nothing.
    Unassociated,
    /// Any animation but a CSS transition, with the easing a keyframe that declares none runs: a CSS animation's
    /// `animation-timing-function`, and `linear` for any other.
    Animation(CompactEasing),
    /// A CSS transition the host runs, with what a transition that reverses it starts from.
    Transition(TransitionReversing),
    /// A CSS transition a hover's lane starts or runs on itself, whose run holds what reverses it.
    LaneTransition,
}

/// One animation effect, described for the style engine to sample it from: the keyframes it runs, its timing function
/// and composite operation, and what its animation is. The host's record of the effect keeps these in its description,
/// which every list of the engine's that holds the effect shares. A description never changes once anything but the
/// record holds it: a change describes the effect anew, so that two descriptions of the effect differ by identity.
#[derive(Clone)]
pub(crate) struct EffectDescription {
    pub(crate) identity: AnimationEffectId,
    /// https://www.w3.org/TR/web-animations-1/#keyframe
    /// Shared with everything else that runs the keyframes, or none for an effect with no keyframes.
    pub(crate) keyframe_set: Option<Arc<KeyframeSet>>,
    /// https://www.w3.org/TR/web-animations-1/#time-transformations
    pub(crate) timing_function: CompactEasing,
    pub(crate) animation: EffectAnimation,
    /// https://www.w3.org/TR/web-animations-1/#dom-keyframeeffect-composite
    /// The effect's composite operation, which a keyframe whose own is `auto` runs.
    pub(crate) composite: FfiCompositeOperation,
}

/// One of an element's animation effects as a list of the style engine's holds it: its description, beside what the
/// engine keeps of it between samples.
#[derive(Clone)]
pub(crate) struct PublishedEffect {
    description: Arc<EffectDescription>,
    /// Substituted interval values retained for this description of the effect, for an effect whose keyframes declare
    /// custom properties.
    custom_property_interval_values: Option<Arc<Mutex<CustomPropertyIntervalValues>>>,
    /// The timing the host last sampled the effect with, which moves without the description.
    pub(crate) timing: Option<CompactEffectTiming>,
}

const _: () = assert!(size_of::<PublishedEffect>() == 128);

impl std::ops::Deref for PublishedEffect {
    type Target = EffectDescription;

    fn deref(&self) -> &EffectDescription {
        &self.description
    }
}

/// What a transition that reverses the one an effect belongs to starts from.
/// https://drafts.csswg.org/css-transitions/#reversing-adjusted-start-value
#[derive(Clone)]
pub(crate) struct TransitionReversing {
    pub(crate) adjusted_start_value: RetainedStyleValueData,
    pub(crate) shortening_factor: f64,
}

impl PublishedEffect {
    /// The effect `description` describes, as a list of the engine's first holds it.
    pub(crate) fn new(description: Arc<EffectDescription>) -> Self {
        Self {
            custom_property_interval_values: description
                .sampled_keyframes()
                .is_some_and(|keyframes| !keyframes.custom_declarations().is_empty())
                .then(Default::default),
            description,
            timing: None,
        }
    }

    /// The effect of a CSS transition of `property_id` from `start` to `end` a hover's lane starts or runs on: two
    /// keyframes, each running the linear easing, that replace the value beneath them, with `timing`.
    pub(crate) fn lane_transition(
        identity: AnimationEffectId,
        property_id: u16,
        start: RetainedStyleValueData,
        end: RetainedStyleValueData,
        timing: EffectTiming,
    ) -> Self {
        let EffectTiming { timing, easing } = timing;
        Self {
            description: Arc::new(EffectDescription {
                identity,
                keyframe_set: Some(Arc::new(KeyframeSet::for_transition(
                    crate::animations::keyframes::KeyframeProperty::Id(property_id),
                    start,
                    end,
                    1.0,
                ))),
                timing_function: CompactEasing::of(easing),
                animation: EffectAnimation::LaneTransition,
                composite: FfiCompositeOperation::Replace,
            }),
            custom_property_interval_values: None,
            timing: Some(timing),
        }
    }

    /// The description of the effect, which the host's record of it shares.
    #[must_use]
    pub(crate) fn description(&self) -> &Arc<EffectDescription> {
        &self.description
    }

    /// The key the effect samples its keyframes at as the host last sampled it, or none where the host has not sampled
    /// it or its progress is unresolved.
    #[must_use]
    pub(crate) fn key(&self) -> Option<f64> {
        self.timing.as_ref()?.key(self.timing_function.as_easing())
    }

    /// The timing and easing the host last sampled the effect with, where it sampled it.
    #[must_use]
    pub(crate) fn effect_timing(&self) -> Option<EffectTiming> {
        Some(EffectTiming {
            timing: self.timing?,
            easing: self.timing_function.easing(),
        })
    }

    /// Look up retained interval values, also reporting whether the interval retains its values across samples.
    pub(crate) fn retained_custom_property_interval_values(
        &self,
        declaration_indices: [usize; 2],
        has_interpolable_syntax: bool,
    ) -> (Option<[RetainedStyleValueData; 2]>, bool) {
        let has_neutral_endpoint = declaration_indices
            .iter()
            .any(|&index| matches!(self.custom_declaration_at(index).value, KeyframeValue::ElementValue));

        if has_interpolable_syntax || has_neutral_endpoint {
            return (None, false);
        }

        // AD-HOC: Match Chromium's retention of substituted interval values for discrete interpolations
        // without checking substitution dependencies. Chromium also does this for some standard
        // properties via CSSDefaultInterpolationType; we currently limit it to custom properties.
        let values = self
            .custom_property_interval_values
            .as_ref()
            .expect("an effect interpolating custom properties keeps their interval values")
            .lock()
            .unwrap()
            .get(&declaration_indices)
            .cloned();
        (values, true)
    }

    /// Retain fresh interval values, or discard cached values for a cyclic registered interval.
    pub(crate) fn update_retained_custom_property_interval_values(
        &self,
        interval: CustomPropertyInterval,
        registry: &CustomPropertyRegistry,
    ) {
        let declaration_indices = interval.declaration_indices();

        let is_cyclic_registered_interval = interval.cyclic.contains(&true) && {
            // SAFETY: The interval retains the name while it is borrowed for the lookup.
            let name = unsafe { ak::Utf16FlyString::from_raw_borrowed(interval.name.raw()) };
            registry.is_registered(&name)
        };

        let interval_cache = self
            .custom_property_interval_values
            .as_ref()
            .expect("an effect interpolating custom properties keeps their interval values");

        // NB: Discard cached values for a cyclic registered interval so breaking the cycle converts its current declarations.
        if is_cyclic_registered_interval {
            interval_cache.lock().unwrap().remove(&declaration_indices);
            return;
        }

        let Some(mut values) = interval.values_to_retain else {
            return;
        };

        for (value, cyclic) in values.iter_mut().zip(interval.cyclic) {
            if !cyclic {
                continue;
            }
            // AD-HOC: Retain a cyclic conversion as unset so its inherited or initial value is resolved when applied.
            //         Caching the fallback's value would freeze it across later style changes.
            *value = RetainedStyleValueData::shared_keyword(crate::css::css_enums::keyword::UNSET).clone();
        }

        interval_cache
            .lock()
            .unwrap()
            .entry(declaration_indices)
            .or_insert(values);
    }
}

impl EffectDescription {
    /// The description of the effect `identity` names with the default timing function, no keyframes and no
    /// animation.
    pub(crate) fn new(identity: AnimationEffectId) -> Self {
        Self {
            identity,
            keyframe_set: None,
            timing_function: CompactEasing::Linear,
            animation: EffectAnimation::Unassociated,
            composite: FfiCompositeOperation::Replace,
        }
    }

    /// The keyframes a sample of the effect interpolates: none for an effect with no animation, which samples nothing.
    fn sampled_keyframes(&self) -> Option<&Arc<KeyframeSet>> {
        match self.animation {
            EffectAnimation::Unassociated => None,
            EffectAnimation::Animation(_) | EffectAnimation::Transition(_) | EffectAnimation::LaneTransition => {
                self.keyframe_set.as_ref()
            }
        }
    }

    /// Whether the effect belongs to a CSS transition, which the interpolation treats differently.
    #[must_use]
    pub(crate) fn is_transition(&self) -> bool {
        matches!(
            self.animation,
            EffectAnimation::Transition(_) | EffectAnimation::LaneTransition
        )
    }

    /// What a transition that reverses the one the effect belongs to starts from, where the host runs it.
    #[must_use]
    pub(crate) fn reversing(&self) -> Option<&TransitionReversing> {
        match &self.animation {
            EffectAnimation::Transition(reversing) => Some(reversing),
            EffectAnimation::Unassociated | EffectAnimation::Animation(_) | EffectAnimation::LaneTransition => None,
        }
    }

    /// The style sheet the effect's keyframes come from, which their URLs resolve against.
    #[must_use]
    pub(crate) fn resource_context(&self) -> Option<&ResourceContext> {
        self.sampled_keyframes()?.resource_context()
    }

    /// Calls `callback` with each longhand the effect's keyframes animate that can be animated, which may repeat: the
    /// longhands of a shorthand whose value is still to be substituted stand for it.
    pub(crate) fn for_each_animated_longhand(&self, mut callback: impl FnMut(u16)) {
        use crate::css::property_metadata::{longhands_for_shorthand, property_animation_type, property_is_shorthand};
        fn visit(property: u16, callback: &mut impl FnMut(u16)) {
            if property_is_shorthand(property) {
                for &longhand in longhands_for_shorthand(property) {
                    visit(longhand, callback);
                }
            } else if property_animation_type(property) != crate::css::animation::ANIMATION_TYPE_NONE {
                callback(property);
            }
        }
        for &property in self
            .sampled_keyframes()
            .into_iter()
            .flat_map(|keyframes| keyframes.target_properties())
        {
            visit(property, &mut callback);
        }
    }

    #[must_use]
    pub(crate) fn keyframes(&self) -> &[Keyframe] {
        self.sampled_keyframes().map_or(&[], |keyframes| keyframes.keyframes())
    }

    /// The custom properties the effect's keyframes interpolate.
    pub(crate) fn target_custom_properties(&self) -> &[crate::css::retained_fly_string::RetainedUtf16FlyString] {
        self.sampled_keyframes()
            .map_or(&[], |keyframes| keyframes.target_custom_properties())
    }

    /// What `keyframe`, one of the effect's, declares that samples. A keyframe generated to hold the element's own
    /// value of a shorthand stands for its longhands', which have keyframes of their own. A value that is invalid at
    /// computed-value time animates nothing. A longhand pending the substitution of its shorthand's value is
    /// substituted with it when the effect is sampled.
    /// https://drafts.csswg.org/css-values-5/#invalid-at-computed-value-time
    pub(crate) fn declarations_of(&self, keyframe: &Keyframe) -> impl Iterator<Item = &KeyframeDeclaration> + Clone {
        use crate::css::property_metadata::property_is_shorthand;
        self.sampled_keyframes()
            .into_iter()
            .flat_map(|keyframes| keyframes.declarations_of(keyframe))
            .filter(|declaration| match &declaration.value {
                KeyframeValue::ElementValue => !property_is_shorthand(declaration.property_id),
                KeyframeValue::Declared(value) => !matches!(value.data(), StyleValueData::GuaranteedInvalid),
            })
    }

    #[must_use]
    pub(crate) fn custom_declarations_of(&self, keyframe: &Keyframe) -> &[KeyframeCustomDeclaration] {
        self.sampled_keyframes()
            .map_or(&[], |keyframes| keyframes.custom_declarations_of(keyframe))
    }

    /// The custom property the effect's keyframes declare at `index`, counting over all of them in order.
    #[must_use]
    pub(crate) fn custom_declaration_at(&self, index: usize) -> &KeyframeCustomDeclaration {
        &self
            .sampled_keyframes()
            .expect("an effect that declares custom properties runs keyframes")
            .custom_declarations()[index]
    }

    /// The composite operation `keyframe`, one of the effect's, runs: its own, or the effect's where its own is `auto`.
    #[must_use]
    pub(crate) fn composite_of(&self, keyframe: &Keyframe) -> FfiCompositeOperation {
        match keyframe.composite {
            CompositeOperationOrAuto::Replace => FfiCompositeOperation::Replace,
            CompositeOperationOrAuto::Add => FfiCompositeOperation::Add,
            CompositeOperationOrAuto::Accumulate => FfiCompositeOperation::Accumulate,
            CompositeOperationOrAuto::Auto => self.composite,
        }
    }

    /// The easing a keyframe of the effect runs where it declares none, or where the one it declares still to be
    /// substituted substitutes to none: the animation's.
    #[must_use]
    pub(crate) fn default_easing(&self) -> Easing {
        match &self.animation {
            EffectAnimation::Animation(default_easing) => default_easing.easing(),
            EffectAnimation::Unassociated | EffectAnimation::Transition(_) | EffectAnimation::LaneTransition => {
                Easing::default()
            }
        }
    }

    /// The bytes the description holds beyond itself, without the keyframe set, which others share.
    fn heap_size(&self) -> usize {
        let default_easing = match &self.animation {
            EffectAnimation::Animation(default_easing) => default_easing.heap_size(),
            EffectAnimation::Unassociated | EffectAnimation::Transition(_) | EffectAnimation::LaneTransition => 0,
        };
        self.timing_function.heap_size() + default_easing
    }
}

/// The effects one of an element's animation lists holds, in composite order.
type AnimationEffectList = (AnimationSlot, Box<[PublishedEffect]>);

/// Per element, the animation effects the host holds for it and each of its pseudo-elements,
/// described for the style engine.
#[derive(Clone, Default)]
pub(crate) struct AnimationEffectDescriptions {
    /// Holding an animation is rare, so only the elements that do have a row, and a row holds only
    /// the lists that are not empty.
    rows: crate::cow_map::CowMap<StyleNodeID, Vec<AnimationEffectList>>,
}

impl AnimationEffectDescriptions {
    /// The bytes of the descriptions: the rows, the effects, the descriptions they share with the host's records of
    /// the effects, and the easings and interval caches those hold, each shared one once. The keyframe sets the effects
    /// run are the host's effects', and the values the keyframes declare their rules'.
    pub(super) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        use crate::heap_size::{map_bytes, vec_bytes};
        let effect_bytes =
            |effect: &PublishedEffect, sizer: &mut crate::heap_size::HeapSizer| {
                let description = match sizer.arc_shallow(effect.description()) {
                    0 => 0,
                    bytes => bytes + effect.description().heap_size(),
                };
                let interval_values = effect.custom_property_interval_values.as_ref().map_or(0, |values| {
                    match sizer.arc_shallow(values) {
                        0 => 0,
                        bytes => bytes + map_bytes(&values.lock().unwrap()),
                    }
                });
                description + interval_values
            };
        let mut bytes = self.rows.heap_size();
        for lists in self.rows.values() {
            bytes += vec_bytes(lists);
            for (_, effects) in lists {
                bytes += size_of_val::<[PublishedEffect]>(effects);
                for effect in effects {
                    bytes += effect_bytes(effect, sizer);
                }
            }
        }
        bytes
    }

    pub(super) fn registrations_changed(
        &mut self,
        previous: &CustomPropertyRegistry,
        registry: &CustomPropertyRegistry,
    ) {
        let nodes: Vec<StyleNodeID> = self.rows.keys().copied().collect();
        for node in nodes {
            let lists = self.rows.get_mut(&node).expect("the node was just listed");
            for (_, effects) in lists.iter_mut() {
                for effect in effects {
                    let Some(interval_values) = &effect.custom_property_interval_values else {
                        continue;
                    };
                    let mut retained_values = interval_values.lock().unwrap().clone();
                    retained_values.retain(|declaration_indices, _| {
                        let name = &effect.custom_declaration_at(declaration_indices[0]).name;
                        registry.registration_matches(previous, &name.to_utf16())
                    });
                    // Leave values held by older style snapshots intact.
                    effect.custom_property_interval_values = Some(Arc::new(Mutex::new(retained_values)));
                }
            }
        }
    }

    /// Whether an animation of `node` interpolates a custom property whose name `moved` holds for.
    pub(crate) fn animates_custom_property(&self, node: StyleNodeID, moved: impl Fn(&[u16]) -> bool) -> bool {
        self.rows.get(&node).is_some_and(|lists| {
            lists.iter().flat_map(|(_, effects)| effects.iter()).any(|effect| {
                effect
                    .target_custom_properties()
                    .iter()
                    .any(|name| moved(&name.to_utf16()))
            })
        })
    }

    /// Replace one list. An empty list drops it. An effect the list held already, by the identity of its description,
    /// keeps the interval values the engine retained for it, and the timing the host last sampled it with where the
    /// list gives it none: a description that changed in any way is another description.
    pub(crate) fn set(&mut self, node: StyleNodeID, slot: AnimationSlot, mut effects: Box<[PublishedEffect]>) {
        let lists = self.rows.entry(node).or_default();
        let existing = lists.iter().position(|(list_slot, _)| *list_slot == slot);
        if let Some(index) = existing {
            let previous_effects = &lists[index].1;
            for effect in &mut effects {
                if let Some(previous_effect) = previous_effects
                    .iter()
                    .find(|previous| Arc::ptr_eq(previous.description(), effect.description()))
                {
                    effect.custom_property_interval_values = previous_effect.custom_property_interval_values.clone();
                    effect.timing = effect.timing.or(previous_effect.timing);
                }
            }
        }
        match (existing, effects.is_empty()) {
            (Some(index), true) => {
                lists.swap_remove(index);
            }
            (Some(index), false) => lists[index].1 = effects,
            (None, true) => {}
            (None, false) => lists.push((slot, effects)),
        }
        if lists.is_empty() {
            self.rows.remove(&node);
        }
    }

    /// One of an element's lists, in composite order.
    #[must_use]
    pub(crate) fn effects(&self, node: StyleNodeID, slot: AnimationSlot) -> &[PublishedEffect] {
        self.rows
            .get(&node)
            .and_then(|lists| lists.iter().find(|(list_slot, _)| *list_slot == slot))
            .map_or(&[], |(_, effects)| effects)
    }

    /// One of an element's effects in one of its lists, by its description.
    fn effect_mut(
        &mut self,
        node: StyleNodeID,
        slot: AnimationSlot,
        mut matches: impl FnMut(&EffectDescription) -> bool,
    ) -> Option<&mut PublishedEffect> {
        self.rows
            .get_mut(&node)
            .and_then(|lists| lists.iter_mut().find(|(list_slot, _)| *list_slot == slot))
            .and_then(|(_, effects)| effects.iter_mut().find(|effect| matches(effect)))
    }

    /// Keeps `timing`, what the host samples one of an element's described effects with, and answers the key the
    /// effect samples its keyframes at as the host samples it: the one its timing gives at the time the host sampled
    /// its timeline, or none for an unresolved progress, which samples nothing. An effect the list does not describe
    /// keeps nothing, and samples nothing either.
    pub(crate) fn time_effect(
        &mut self,
        node: StyleNodeID,
        slot: AnimationSlot,
        description: &Arc<EffectDescription>,
        timing: CompactEffectTiming,
    ) -> Option<f64> {
        let effect = self.effect_mut(node, slot, |effect| std::ptr::eq(effect, &raw const **description))?;
        effect.timing = Some(timing);
        effect.key()
    }

    /// Keeps `timing`, what the host sampled one of an element's described effects with where it sampled
    /// without asking the engine. An effect the list does not describe keeps nothing.
    pub(crate) fn keep_timing(
        &mut self,
        node: StyleNodeID,
        slot: AnimationSlot,
        identity: AnimationEffectId,
        timing: CompactEffectTiming,
    ) {
        if let Some(effect) = self.effect_mut(node, slot, |effect| effect.identity == identity) {
            effect.timing = Some(timing);
        }
    }

    /// Has one of an element's described effects that the host sampled before take `timing`: the timing the host runs
    /// it on since, where it runs it without sampling it.
    pub(crate) fn refresh_timing(
        &mut self,
        node: StyleNodeID,
        identity: AnimationEffectId,
        timing: &EffectTimingSnapshot,
    ) {
        if let Some(kept) = self
            .effect_mut(node, 0, |effect| effect.identity == identity)
            .and_then(|effect| effect.timing.as_mut())
        {
            *kept = CompactEffectTiming::from(timing);
        }
    }

    /// Give up the lists of an identity that retires. An identity can be minted again for another
    /// element, so a list left behind would be read as that element's.
    pub(crate) fn retire(&mut self, node: StyleNodeID) {
        self.rows.remove(&node);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn effect(identity: u64) -> PublishedEffect {
        PublishedEffect::new(Arc::new(EffectDescription::new(AnimationEffectId(identity))))
    }

    #[test]
    fn an_empty_list_drops_the_row() {
        let node = StyleNodeID::from_raw(1).unwrap();
        let mut descriptions = AnimationEffectDescriptions::default();
        descriptions.set(node, 0, Box::new([effect(7), effect(9)]));
        assert_eq!(descriptions.effects(node, 0).len(), 2);
        assert!(descriptions.effects(node, 2).is_empty());
        descriptions.set(node, 0, Box::new([]));
        assert!(descriptions.rows.is_empty());
    }

    #[test]
    fn an_effect_the_list_held_already_keeps_its_timing() {
        let node = StyleNodeID::from_raw(1).unwrap();
        let mut descriptions = AnimationEffectDescriptions::default();
        let kept = effect(7);
        let kept_description = kept.description().clone();
        descriptions.set(node, 0, Box::new([kept]));
        let timing = CompactEffectTiming::from(&crate::animations::snapshot::EffectTimingSnapshot::default());
        descriptions.keep_timing(node, 0, AnimationEffectId(7), timing);
        descriptions.set(node, 0, Box::new([PublishedEffect::new(kept_description), effect(9)]));
        assert_eq!(descriptions.effects(node, 0)[0].timing, Some(timing));
        assert_eq!(descriptions.effects(node, 0)[1].timing, None);
    }

    #[test]
    fn a_retired_identity_holds_no_descriptions_when_reissued() {
        let node = StyleNodeID::from_raw(1).unwrap();
        let mut descriptions = AnimationEffectDescriptions::default();
        descriptions.set(node, 0, Box::new([effect(7)]));
        descriptions.set(node, 3, Box::new([effect(8)]));
        descriptions.retire(node);
        assert!(descriptions.effects(node, 0).is_empty());
        assert!(descriptions.effects(node, 3).is_empty());
        assert!(descriptions.rows.is_empty());
    }
}
