/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The animation effects an element holds, described for the style engine.
//!
//! Sampling an element's animations interpolates what its effects' keyframes declare. Everything a
//! keyframe declares that does not depend on the element being sampled - its offset, its easing,
//! its composite operation and the values it declares - is settled when the host describes the
//! effect, which it does again whenever the effect changes in a way that moves any of it. What does
//! depend on the element, a value or an easing still to be substituted against it, travels as
//! written.

use super::animations::{AnimationSlot, EffectTiming};
use super::bridge::{
    FfiAnimationEffectVersion, FfiPublishedAnimationCustomDeclaration, FfiPublishedAnimationDeclaration,
    FfiPublishedAnimationEffect, FfiPublishedAnimationKeyframe, FfiPublishedEasingKind, FfiPublishedLinearEasingPoint,
};
use super::tree::StyleNodeID;
use super::weak_pool::WeakPool;
use crate::css::animation::FfiCompositeOperation;
use crate::css::custom_properties::CustomPropertyRegistry;
use crate::css::easing::{Easing, FfiEasingDescriptor, FfiEasingKind, FfiLinearEasingPoint};
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style_compute::FfiEffectTiming;
use crate::css::style_value::EasingPayload;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
use std::collections::HashMap;
use std::ffi::c_void;
use std::ops::Range;
use std::sync::{Arc, Mutex};

/// What a published keyframe declares for a property.
#[derive(Clone, PartialEq)]
pub(crate) enum PublishedValue {
    /// The element's own value, held by a keyframe the host synthesized, and not known until the
    /// element is sampled.
    ElementValue,
    Declared(RetainedStyleValueData),
}

impl PublishedValue {
    /// The value's identity: zero for the element's own value.
    fn identity(&self) -> usize {
        match self {
            Self::ElementValue => 0,
            Self::Declared(value) => value.pointer() as usize,
        }
    }

    /// Retains the value a host pointer names, or stands for the element's own value where it is
    /// null.
    ///
    /// # Safety
    /// `value` must be null or a live style value.
    unsafe fn from_host(value: *const c_void) -> Self {
        match value.is_null() {
            true => Self::ElementValue,
            false => Self::Declared(unsafe { retained(value) }),
        }
    }
}

impl PublishedEffect {
    /// The values the effect's keyframes declare, and those they run their easing by.
    pub(crate) fn declared_values(&self) -> impl Iterator<Item = &StyleValueData> {
        let keyframes = &self.keyframes;
        let declared = keyframes
            .declarations
            .iter()
            .map(|declaration| &declaration.value)
            .chain(
                keyframes
                    .custom_declarations
                    .iter()
                    .map(|declaration| &declaration.value),
            )
            .filter_map(|value| match value {
                PublishedValue::ElementValue => None,
                PublishedValue::Declared(value) => value.optional_data(),
            });
        let easings = keyframes
            .keyframes
            .iter()
            .filter_map(|keyframe| keyframe.easing_value.as_ref())
            .filter_map(RetainedStyleValueData::optional_data);
        declared.chain(easings)
    }
}

/// # Safety
/// `value` must be a live style value.
unsafe fn retained(value: *const c_void) -> RetainedStyleValueData {
    unsafe {
        RetainedStyleValueData::from_retained_pointer(crate::css::style_value::retain_style_value(
            value.cast::<StyleValueData>(),
        ))
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
        return match *keyword {
            keyword::LINEAR => Some(Easing::default()),
            keyword::EASE => Some(cubic_bezier(0.25, 0.1, 0.25, 1.0)),
            keyword::EASE_IN => Some(cubic_bezier(0.42, 0.0, 1.0, 1.0)),
            keyword::EASE_OUT => Some(cubic_bezier(0.0, 0.0, 0.58, 1.0)),
            keyword::EASE_IN_OUT => Some(cubic_bezier(0.42, 0.0, 0.58, 1.0)),
            _ => None,
        };
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

#[derive(Clone)]

pub(crate) struct PublishedDeclaration {
    pub(crate) property_id: u16,
    pub(crate) value: PublishedValue,
}

/// A custom property a keyframe declares. The name is retained: a description outlives the call
/// that published it, and a fly string's raw representation is only an identity while the string
/// is alive.
#[derive(Clone)]
pub(crate) struct PublishedCustomDeclaration {
    pub(crate) name: RetainedUtf16FlyString,
    pub(crate) value: PublishedValue,
}

#[derive(Clone, PartialEq)]
pub(crate) struct PublishedKeyframe {
    pub(crate) key: i64,
    /// The easing the keyframe runs, which is the one its animation runs where the keyframe has
    /// none of its own, and what it runs where `easing_value` substitutes to nothing.
    pub(crate) easing: Easing,
    pub(crate) easing_value: Option<RetainedStyleValueData>,
    /// The composite operation, with a keyframe's `auto` already the effect's own.
    pub(crate) composite: FfiCompositeOperation,
    declarations: Range<usize>,
    custom_declarations: Range<usize>,
}

/// The style sheet an effect's keyframes come from, which their URLs resolve against.
#[derive(Clone)]
pub(crate) struct PublishedResourceContext {
    /// Shared with every resolution of the effect's declarations that points into it.
    pub(crate) base_url: Arc<[u8]>,
    pub(crate) origin_clean: bool,
}

/// Maps an interval's declaration indices to its substituted values.
type CustomPropertyIntervalValues = HashMap<[usize; 2], [RetainedStyleValueData; 2]>;

/// What an effect's keyframes declare, which effects describing equal keyframes share: the elements running one
/// `@keyframes` rule hold one of these between them.
pub(crate) struct PublishedKeyframes {
    /// The style sheet the keyframes come from, which their URLs resolve against.
    resource_context: Option<PublishedResourceContext>,
    target_properties: Box<[u16]>,
    keyframes: Box<[PublishedKeyframe]>,
    declarations: Box<[PublishedDeclaration]>,
    custom_declarations: Box<[PublishedCustomDeclaration]>,
}

impl PublishedKeyframes {
    /// The hash the document's pool finds equal keyframes under: what each declares, values by identity.
    fn pool_hash(&self) -> u64 {
        use std::hash::{Hash, Hasher};
        let mut hasher = super::fast_hash::fast_hasher();
        self.resource_context
            .as_ref()
            .map(|context| (&context.base_url, context.origin_clean))
            .hash(&mut hasher);
        self.target_properties.hash(&mut hasher);
        for keyframe in &self.keyframes {
            keyframe.key.hash(&mut hasher);
            (keyframe.composite as u8).hash(&mut hasher);
        }
        for declaration in &self.declarations {
            declaration.property_id.hash(&mut hasher);
            declaration.value.identity().hash(&mut hasher);
        }
        for declaration in &self.custom_declarations {
            declaration.name.raw().hash(&mut hasher);
            declaration.value.identity().hash(&mut hasher);
        }
        hasher.finish()
    }

    /// Whether the two declare the same, values by identity.
    fn declares_the_same_as(&self, other: &Self) -> bool {
        self.resource_context
            .as_ref()
            .map(|context| (&context.base_url, context.origin_clean))
            == other
                .resource_context
                .as_ref()
                .map(|context| (&context.base_url, context.origin_clean))
            && self.target_properties == other.target_properties
            && self.keyframes.len() == other.keyframes.len()
            && self.keyframes.iter().zip(&other.keyframes).all(|(first, second)| {
                first.key == second.key
                    && first.easing == second.easing
                    && first.easing_value.as_ref().map(RetainedStyleValueData::pointer)
                        == second.easing_value.as_ref().map(RetainedStyleValueData::pointer)
                    && first.composite == second.composite
                    && first.declarations == second.declarations
                    && first.custom_declarations == second.custom_declarations
            })
            && self.declarations.len() == other.declarations.len()
            && self
                .declarations
                .iter()
                .zip(&other.declarations)
                .all(|(first, second)| {
                    first.property_id == second.property_id && first.value.identity() == second.value.identity()
                })
            && self.custom_declarations.len() == other.custom_declarations.len()
            && self
                .custom_declarations
                .iter()
                .zip(&other.custom_declarations)
                .all(|(first, second)| {
                    first.name.raw() == second.name.raw() && first.value.identity() == second.value.identity()
                })
    }

    fn heap_size(&self) -> usize {
        self.resource_context
            .as_ref()
            .map_or(0, |context| crate::heap_size::ARC_HEADER_BYTES + context.base_url.len())
            + size_of_val::<[u16]>(&self.target_properties)
            + size_of_val::<[PublishedKeyframe]>(&self.keyframes)
            + self
                .keyframes
                .iter()
                .map(|keyframe| match &keyframe.easing {
                    Easing::Linear(points) => crate::heap_size::vec_bytes(points),
                    _ => 0,
                })
                .sum::<usize>()
            + size_of_val::<[PublishedDeclaration]>(&self.declarations)
            + size_of_val::<[PublishedCustomDeclaration]>(&self.custom_declarations)
    }
}

/// One of an element's animation effects, described for the style engine.
#[derive(Clone)]
pub(crate) struct PublishedEffect {
    pub(crate) identity: u64,
    pub(crate) generation: u64,
    /// The effect belongs to a CSS transition, which the interpolation treats differently.
    pub(crate) is_transition: bool,
    keyframes: Arc<PublishedKeyframes>,
    /// Substituted interval values retained for this effect version, for an effect whose keyframes declare custom
    /// properties.
    custom_property_interval_values: Option<Arc<Mutex<CustomPropertyIntervalValues>>>,
    /// The timing the host last sampled the effect with, which moves without the description.
    pub(crate) timing: Option<EffectTiming>,
    /// What a transition reverses to, and how much shorter a reversing transition runs, where the effect is one.
    pub(crate) reversing: Option<TransitionReversing>,
}

const _: () = assert!(size_of::<PublishedEffect>() == 224);

/// What a transition that reverses the one an effect belongs to starts from.
/// https://drafts.csswg.org/css-transitions/#reversing-adjusted-start-value
#[derive(Clone)]
pub(crate) struct TransitionReversing {
    pub(crate) adjusted_start_value: RetainedStyleValueData,
    pub(crate) shortening_factor: f64,
}

fn target_properties(properties: impl IntoIterator<Item = u16>) -> Box<[u16]> {
    use crate::css::property_metadata::{longhands_for_shorthand, property_animation_type, property_is_shorthand};

    let mut pending: Vec<_> = properties.into_iter().collect();
    let mut longhands = Vec::new();
    while let Some(property) = pending.pop() {
        if property_is_shorthand(property) {
            pending.extend_from_slice(longhands_for_shorthand(property));
        } else if property_animation_type(property) != crate::css::animation::ANIMATION_TYPE_NONE {
            longhands.push(property);
        }
    }
    longhands.sort_unstable();
    longhands.dedup();
    longhands.into_boxed_slice()
}

impl PublishedEffect {
    /// The effect of a CSS transition of `property_id` from `start` to `end`, as the host describes one:
    /// two keyframes, each running the linear easing, that replace the value beneath them.
    pub(crate) fn transition(
        identity: u64,
        property_id: u16,
        start: RetainedStyleValueData,
        end: RetainedStyleValueData,
        timing: Option<EffectTiming>,
    ) -> Self {
        let keyframe = |key, index| PublishedKeyframe {
            key,
            easing: Easing::default(),
            easing_value: None,
            composite: FfiCompositeOperation::Replace,
            declarations: index..index + 1,
            custom_declarations: 0..0,
        };
        Self {
            identity,
            generation: 0,
            is_transition: true,
            keyframes: Arc::new(PublishedKeyframes {
                resource_context: None,
                target_properties: target_properties([property_id]),
                // `KeyframeEffect::AnimationKeyFrameKeyScaleFactor` keys the end at 100%.
                keyframes: Box::new([keyframe(0, 0), keyframe(100 * 1000, 1)]),
                declarations: Box::new([start, end].map(|value| PublishedDeclaration {
                    property_id,
                    value: PublishedValue::Declared(value),
                })),
                custom_declarations: Box::new([]),
            }),
            custom_property_interval_values: None,
            timing,
            reversing: None,
        }
    }

    /// The style sheet the effect's keyframes come from, which their URLs resolve against.
    #[must_use]
    pub(crate) fn resource_context(&self) -> Option<&PublishedResourceContext> {
        self.keyframes.resource_context.as_ref()
    }

    /// The longhands the effect's keyframes animate, sorted.
    #[must_use]
    pub(crate) fn target_properties(&self) -> &[u16] {
        &self.keyframes.target_properties
    }

    #[must_use]
    pub(crate) fn keyframes(&self) -> &[PublishedKeyframe] {
        &self.keyframes.keyframes
    }

    #[must_use]
    pub(crate) fn declarations_of(&self, keyframe: &PublishedKeyframe) -> &[PublishedDeclaration] {
        &self.keyframes.declarations[keyframe.declarations.clone()]
    }

    #[must_use]
    pub(crate) fn custom_declarations_of(&self, keyframe: &PublishedKeyframe) -> &[PublishedCustomDeclaration] {
        &self.keyframes.custom_declarations[keyframe.custom_declarations.clone()]
    }

    #[must_use]
    pub(crate) fn custom_declaration_at(&self, index: usize) -> &PublishedCustomDeclaration {
        &self.keyframes.custom_declarations[index]
    }

    /// Resolve interval values afresh for interpolable syntaxes or neutral keyframes; retain them otherwise.
    /// Also report whether the values are retained across samples.
    pub(crate) fn resolve_custom_property_interval_values(
        &self,
        declaration_indices: [usize; 2],
        has_interpolable_syntax: bool,
        resolve_values: impl FnOnce() -> [RetainedStyleValueData; 2],
    ) -> ([RetainedStyleValueData; 2], bool) {
        let has_neutral_endpoint = declaration_indices
            .iter()
            .any(|&index| matches!(self.custom_declaration_at(index).value, PublishedValue::ElementValue));

        if has_interpolable_syntax || has_neutral_endpoint {
            return (resolve_values(), false);
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
            .entry(declaration_indices)
            .or_insert_with(resolve_values)
            .clone();
        (values, true)
    }
}

/// The flat buffers one animation list's descriptions travel in.
#[derive(Clone, Copy)]
pub(crate) struct PublishedEffectBuffers<'a> {
    pub(crate) effects: &'a [FfiPublishedAnimationEffect],
    pub(crate) keyframes: &'a [FfiPublishedAnimationKeyframe],
    pub(crate) declarations: &'a [FfiPublishedAnimationDeclaration],
    pub(crate) custom_declarations: &'a [FfiPublishedAnimationCustomDeclaration],
    pub(crate) linear_points: &'a [FfiPublishedLinearEasingPoint],
    pub(crate) base_url_bytes: &'a [u8],
}

fn range(first: u32, count: u32) -> Range<usize> {
    first as usize..first as usize + count as usize
}

impl PublishedEffectBuffers<'_> {
    /// The effects the buffers describe, each value they name retained.
    ///
    /// # Safety
    /// Every value and custom-property name the buffers name must be live.
    pub(crate) unsafe fn effects(self) -> Box<[PublishedEffect]> {
        self.effects
            .iter()
            .map(|effect| {
                let mut declarations = Vec::new();
                let mut custom_declarations = Vec::new();
                let keyframes = self.keyframes[range(effect.first_keyframe, effect.keyframe_count)]
                    .iter()
                    .map(|keyframe| {
                        let first_declaration = declarations.len();
                        declarations.extend(
                            self.declarations[range(keyframe.first_declaration, keyframe.declaration_count)]
                                .iter()
                                .map(|declaration| PublishedDeclaration {
                                    property_id: declaration.property_id,
                                    value: unsafe { PublishedValue::from_host(declaration.value) },
                                }),
                        );
                        let first_custom_declaration = custom_declarations.len();
                        custom_declarations.extend(
                            self.custom_declarations
                                [range(keyframe.first_custom_declaration, keyframe.custom_declaration_count)]
                            .iter()
                            .map(|declaration| PublishedCustomDeclaration {
                                name: unsafe { RetainedUtf16FlyString::from_borrowed_raw(declaration.name) },
                                value: unsafe { PublishedValue::from_host(declaration.value) },
                            }),
                        );
                        PublishedKeyframe {
                            key: keyframe.key,
                            easing: self.easing(keyframe),
                            easing_value: (!keyframe.easing_value.is_null())
                                .then(|| unsafe { retained(keyframe.easing_value) }),
                            composite: keyframe.composite,
                            declarations: first_declaration..declarations.len(),
                            custom_declarations: first_custom_declaration..custom_declarations.len(),
                        }
                    })
                    .collect();
                PublishedEffect {
                    identity: effect.identity,
                    generation: effect.generation,
                    is_transition: effect.is_transition,
                    custom_property_interval_values: (!custom_declarations.is_empty()).then(Default::default),
                    keyframes: Arc::new(PublishedKeyframes {
                        resource_context: effect.has_resource_context.then(|| PublishedResourceContext {
                            base_url: self.base_url_bytes[range(effect.base_url_offset, effect.base_url_length)].into(),
                            origin_clean: effect.resource_context_is_origin_clean,
                        }),
                        target_properties: target_properties(
                            declarations.iter().map(|declaration| declaration.property_id),
                        ),
                        keyframes,
                        declarations: declarations.into(),
                        custom_declarations: custom_declarations.into(),
                    }),
                    timing: None,
                    reversing: (!effect.reversing_adjusted_start_value.is_null()).then(|| TransitionReversing {
                        adjusted_start_value: unsafe { retained(effect.reversing_adjusted_start_value) },
                        shortening_factor: effect.reversing_shortening_factor,
                    }),
                }
            })
            .collect()
    }

    fn easing(self, keyframe: &FfiPublishedAnimationKeyframe) -> Easing {
        match keyframe.easing_kind {
            FfiPublishedEasingKind::Linear => Easing::Linear(
                self.linear_points[range(keyframe.first_linear_point, keyframe.linear_point_count)]
                    .iter()
                    .map(|point| FfiLinearEasingPoint {
                        input: point.input,
                        output: point.output,
                    })
                    .collect(),
            ),
            FfiPublishedEasingKind::CubicBezier => Easing::CubicBezier {
                x1: keyframe.x1,
                y1: keyframe.y1,
                x2: keyframe.x2,
                y2: keyframe.y2,
            },
            FfiPublishedEasingKind::Steps => Easing::Steps {
                interval_count: keyframe.interval_count,
                position: keyframe.step_position,
            },
        }
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
    rows: HashMap<StyleNodeID, Vec<AnimationEffectList>>,
    /// The keyframes the effects describe, by content, which an effect describing equal ones shares.
    keyframes: WeakPool<PublishedKeyframes>,
}

impl AnimationEffectDescriptions {
    /// The bytes of the descriptions: the rows, the effects, and the keyframes and interval caches they hold, each
    /// shared one once. The values the keyframes declare are their rules'.
    pub(super) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        use crate::heap_size::{map_bytes, vec_bytes};
        let effect_bytes =
            |effect: &PublishedEffect, sizer: &mut crate::heap_size::HeapSizer| {
                let keyframes = match sizer.arc_shallow(&effect.keyframes) {
                    0 => 0,
                    bytes => bytes + effect.keyframes.heap_size(),
                };
                let interval_values = effect.custom_property_interval_values.as_ref().map_or(0, |values| {
                    match sizer.arc_shallow(values) {
                        0 => 0,
                        bytes => bytes + map_bytes(&values.lock().unwrap()),
                    }
                });
                keyframes + interval_values
            };
        let mut bytes = map_bytes(&self.rows);
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
        for (_, effects) in self.rows.values_mut().flatten() {
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

    /// Whether an animation of `node` interpolates a custom property whose name `moved` holds for.
    pub(crate) fn animates_custom_property(&self, node: StyleNodeID, moved: impl Fn(&[u16]) -> bool) -> bool {
        self.rows.get(&node).is_some_and(|lists| {
            lists.iter().flat_map(|(_, effects)| effects.iter()).any(|effect| {
                effect
                    .keyframes
                    .custom_declarations
                    .iter()
                    .any(|declaration| moved(&declaration.name.to_utf16()))
            })
        })
    }

    /// Replace one list. An empty list drops it.
    pub(crate) fn set(&mut self, node: StyleNodeID, slot: AnimationSlot, mut effects: Box<[PublishedEffect]>) {
        for effect in &mut effects {
            let hash = effect.keyframes.pool_hash();
            match self
                .keyframes
                .find(hash, |pooled| pooled.declares_the_same_as(&effect.keyframes))
            {
                Some(pooled) => effect.keyframes = pooled,
                None => self.keyframes.insert(hash, &effect.keyframes),
            }
        }
        let lists = self.rows.entry(node).or_default();
        let existing = lists.iter().position(|(list_slot, _)| *list_slot == slot);
        if let Some(index) = existing {
            let previous_effects = &lists[index].1;
            for effect in &mut effects {
                if let Some(previous_effect) = previous_effects
                    .iter()
                    .find(|previous| previous.identity == effect.identity && previous.generation == effect.generation)
                {
                    effect.custom_property_interval_values = previous_effect.custom_property_interval_values.clone();
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

    /// Whether one of an element's lists describes exactly these versions of the effects, in this
    /// order. Every change that moves what a description says moves its effect's generation.
    #[cfg(test)]
    #[must_use]
    pub(crate) fn describe(
        &self,
        node: StyleNodeID,
        slot: AnimationSlot,
        versions: &[FfiAnimationEffectVersion],
    ) -> bool {
        let effects = self.effects(node, slot);
        effects.len() == versions.len()
            && effects
                .iter()
                .zip(versions)
                .all(|(effect, version)| effect.identity == version.identity && effect.generation == version.generation)
    }

    /// Keeps `timing` and `easing`, what the host samples one of an element's described effects with, and
    /// answers the key the effect samples its keyframes at as the host samples it: the one its timing
    /// gives at the time the host sampled its timeline, or `host_key` where the engine cannot decide the
    /// timing, or none for an unresolved progress, which samples nothing. An effect the list does not
    /// describe keeps nothing, and samples nothing either.
    ///
    /// # Safety
    /// The linear points `easing` names must be live.
    pub(crate) unsafe fn time_effect(
        &mut self,
        node: StyleNodeID,
        slot: AnimationSlot,
        identity: u64,
        timing: &FfiEffectTiming,
        easing: &FfiEasingDescriptor,
        host_key: Option<f64>,
    ) -> Option<f64> {
        let effect = self
            .rows
            .get_mut(&node)
            .and_then(|lists| lists.iter_mut().find(|(list_slot, _)| *list_slot == slot))
            .and_then(|(_, effects)| effects.iter_mut().find(|effect| effect.identity == identity));
        let effect = effect?;
        // A timing that has not moved keeps the easing it holds, so a sample allocates nothing.
        let unchanged = effect
            .timing
            .as_ref()
            .is_some_and(|kept| kept.timing == *timing && unsafe { easing_is(&kept.easing, easing) });
        if !unchanged {
            effect.timing = Some(EffectTiming {
                timing: *timing,
                easing: unsafe { Easing::from_descriptor(easing) },
            });
        }
        effect.timing.as_ref().expect("the timing was kept above").key(host_key)
    }

    /// Keeps `timing`, what the host sampled one of an element's described effects with where it sampled
    /// without asking the engine. An effect the list does not describe keeps nothing.
    pub(crate) fn keep_timing(&mut self, node: StyleNodeID, slot: AnimationSlot, identity: u64, timing: EffectTiming) {
        if let Some(effect) = self
            .rows
            .get_mut(&node)
            .and_then(|lists| lists.iter_mut().find(|(list_slot, _)| *list_slot == slot))
            .and_then(|(_, effects)| effects.iter_mut().find(|effect| effect.identity == identity))
        {
            effect.timing = Some(timing);
        }
    }

    /// Has one of an element's described effects that the host sampled before take `timing`, keeping its easing: the
    /// timing the host runs it on since, where it runs it without sampling it.
    pub(crate) fn refresh_timing(&mut self, node: StyleNodeID, identity: u64, timing: &FfiEffectTiming) {
        if let Some(kept) = self
            .rows
            .get_mut(&node)
            .and_then(|lists| lists.iter_mut().find(|(list_slot, _)| *list_slot == 0))
            .and_then(|(_, effects)| effects.iter_mut().find(|effect| effect.identity == identity))
            .and_then(|effect| effect.timing.as_mut())
        {
            kept.timing = *timing;
        }
    }

    /// Give up the lists of an identity that retires. An identity can be minted again for another
    /// element, so a list left behind would be read as that element's.
    pub(crate) fn retire(&mut self, node: StyleNodeID) {
        self.rows.remove(&node);
    }
}

/// The versions of the effects each of an element's lists holds as the host described them to the engine, which the
/// host follows over every description it writes and every identity a transaction releases, as the engine retires the
/// node's lists then, so it knows without asking whether the engine describes a list already.
#[derive(Default)]
pub(crate) struct DescribedVersions {
    rows: HashMap<StyleNodeID, Vec<DescribedList>>,
}

/// The identity and generation of each effect one of an element's lists holds, in composite order.
type DescribedList = (AnimationSlot, Box<[(u64, u64)]>);

impl DescribedVersions {
    /// The bytes the versions hold on the heap.
    pub(crate) fn heap_size(&self) -> usize {
        use crate::heap_size::{map_bytes, vec_bytes};
        map_bytes(&self.rows)
            + self
                .rows
                .values()
                .map(|lists| {
                    vec_bytes(lists)
                        + lists
                            .iter()
                            .map(|(_, versions)| size_of_val::<[(u64, u64)]>(versions))
                            .sum::<usize>()
                })
                .sum::<usize>()
    }

    /// Follows the host describing `effects` as one of `node`'s lists.
    pub(crate) fn follow(&mut self, node: StyleNodeID, slot: AnimationSlot, effects: &[PublishedEffect]) {
        let lists = self.rows.entry(node).or_default();
        lists.retain(|(list_slot, _)| *list_slot != slot);
        if !effects.is_empty() {
            lists.push((
                slot,
                effects
                    .iter()
                    .map(|effect| (effect.identity, effect.generation))
                    .collect(),
            ));
        }
        if lists.is_empty() {
            self.rows.remove(&node);
        }
    }

    /// Whether the engine describes one of `node`'s lists as exactly these versions: every change that moves what a
    /// description says moves its effect's generation.
    pub(crate) fn describe(
        &self,
        node: StyleNodeID,
        slot: AnimationSlot,
        versions: &[FfiAnimationEffectVersion],
    ) -> bool {
        let described = self
            .rows
            .get(&node)
            .and_then(|lists| lists.iter().find(|(list_slot, _)| *list_slot == slot))
            .map_or(&[][..], |(_, described)| described);
        described.len() == versions.len()
            && described
                .iter()
                .zip(versions)
                .all(|(&(identity, generation), version)| {
                    identity == version.identity && generation == version.generation
                })
    }

    /// Forgets the lists of the nodes a transaction retired, whose identities it `released`.
    pub(crate) fn forget(&mut self, released: &[u32]) {
        if self.rows.is_empty() {
            return;
        }
        for node in released.iter().filter_map(|&node| StyleNodeID::from_raw(node)) {
            self.rows.remove(&node);
        }
    }
}

/// Whether `easing` is the function `descriptor` describes.
///
/// # Safety
/// The linear points `descriptor` names must be live.
unsafe fn easing_is(easing: &Easing, descriptor: &FfiEasingDescriptor) -> bool {
    match (easing, descriptor.kind) {
        (Easing::Linear(points), FfiEasingKind::Linear) => {
            points.len() == descriptor.linear_point_count
                && (points.is_empty()
                    || points.as_slice() == unsafe { ak::slice_from_raw_parts(descriptor.linear_points, points.len()) })
        }
        (Easing::CubicBezier { x1, y1, x2, y2 }, FfiEasingKind::CubicBezier) => {
            [*x1, *y1, *x2, *y2] == [descriptor.x1, descriptor.y1, descriptor.x2, descriptor.y2]
        }
        (
            Easing::Steps {
                interval_count,
                position,
            },
            FfiEasingKind::Steps,
        ) => *interval_count == descriptor.interval_count && *position == descriptor.step_position,
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn effect(identity: u64, generation: u64) -> FfiPublishedAnimationEffect {
        FfiPublishedAnimationEffect {
            identity,
            generation,
            is_transition: false,
            has_resource_context: false,
            resource_context_is_origin_clean: false,
            first_keyframe: 0,
            keyframe_count: 0,
            base_url_offset: 0,
            base_url_length: 0,
            reversing_adjusted_start_value: std::ptr::null(),
            reversing_shortening_factor: 1.0,
        }
    }

    /// The engine's descriptions, and the host's record of what it described, which must answer alike.
    #[derive(Default)]
    struct Described {
        rows: AnimationEffectDescriptions,
        host: DescribedVersions,
    }

    impl Described {
        fn describe(&self, node: StyleNodeID, slot: AnimationSlot, versions: &[FfiAnimationEffectVersion]) -> bool {
            let described = self.rows.describe(node, slot, versions);
            assert_eq!(described, self.host.describe(node, slot, versions));
            described
        }

        fn effects(&self, node: StyleNodeID, slot: AnimationSlot) -> &[PublishedEffect] {
            self.rows.effects(node, slot)
        }

        fn retire(&mut self, node: StyleNodeID) {
            self.rows.retire(node);
            self.host.forget(&[node.raw()]);
        }
    }

    fn set(
        descriptions: &mut Described,
        node: StyleNodeID,
        slot: AnimationSlot,
        effects: &[FfiPublishedAnimationEffect],
    ) {
        let buffers = PublishedEffectBuffers {
            effects,
            keyframes: &[],
            declarations: &[],
            custom_declarations: &[],
            linear_points: &[],
            base_url_bytes: &[],
        };
        let effects = unsafe { buffers.effects() };
        descriptions.host.follow(node, slot, &effects);
        descriptions.rows.set(node, slot, effects);
    }

    fn version(identity: u64, generation: u64) -> FfiAnimationEffectVersion {
        FfiAnimationEffectVersion { identity, generation }
    }

    #[test]
    fn a_list_describes_the_versions_it_was_published_for() {
        let node = StyleNodeID::from_raw(1).unwrap();
        let mut descriptions = Described::default();
        set(&mut descriptions, node, 0, &[effect(7, 1), effect(9, 3)]);
        assert!(descriptions.describe(node, 0, &[version(7, 1), version(9, 3)]));
        assert!(!descriptions.describe(node, 0, &[version(7, 2), version(9, 3)]));
        assert!(!descriptions.describe(node, 0, &[version(9, 3), version(7, 1)]));
        assert!(!descriptions.describe(node, 0, &[version(7, 1)]));
        assert!(!descriptions.describe(node, 2, &[version(7, 1), version(9, 3)]));
        assert!(descriptions.describe(node, 2, &[]));

        set(&mut descriptions, node, 0, &[]);
        assert!(descriptions.rows.rows.is_empty());
        assert!(descriptions.host.rows.is_empty());
    }

    #[test]
    fn a_retired_identity_holds_no_descriptions_when_reissued() {
        let node = StyleNodeID::from_raw(1).unwrap();
        let mut descriptions = Described::default();
        set(&mut descriptions, node, 0, &[effect(7, 1)]);
        set(&mut descriptions, node, 3, &[effect(8, 1)]);
        descriptions.retire(node);
        assert!(descriptions.effects(node, 0).is_empty());
        assert!(descriptions.effects(node, 3).is_empty());
        assert!(descriptions.rows.rows.is_empty());
        assert!(descriptions.host.rows.is_empty());
        assert!(descriptions.describe(node, 0, &[]));
    }
}
