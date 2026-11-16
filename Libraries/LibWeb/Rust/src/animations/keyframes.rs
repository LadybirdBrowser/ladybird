/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Keyframe sets: the keyframes an effect runs, resolved from a `@keyframes` rule, from the keyframes script gave a
//! `KeyframeEffect`, or for a CSS transition.
//!
//! A set is immutable once built and shared by everything that runs it: every effect of one `@keyframes` rule holds
//! the rule's set. What depends on the effect running it, its composite operation and the easing a keyframe that
//! declares none runs, stays with the effect.

use crate::css::easing::Easing;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
use std::ops::Range;
use std::sync::Arc;

/// What keyframe offsets, as percentages, are scaled by to key a keyframe, which keys an offset of 100% at 100000.
pub(crate) const KEY_SCALE_FACTOR: f64 = 1000.0;

/// The key of a keyframe at an offset of 0%.
pub(crate) const INITIAL_KEY: u64 = 0;

/// The key of a keyframe at an offset of 100%.
pub(crate) const FINAL_KEY: u64 = 100 * KEY_SCALE_FACTOR as u64;

/// The offset `offset`, a fraction of an iteration, scaled as a key, before it is truncated to one.
pub(crate) fn scaled_offset(offset: f64) -> f64 {
    offset * 100.0 * KEY_SCALE_FACTOR
}

/// The key of a keyframe at the offset `offset`, a fraction of an iteration.
pub(crate) fn key_of_offset(offset: f64) -> u64 {
    #[expect(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let key = scaled_offset(offset) as u64;
    key
}

/// The offset, a fraction of an iteration, that `key` scales.
pub(crate) fn offset_of_key(key: f64) -> f64 {
    #[expect(clippy::cast_precision_loss)]
    let final_key = FINAL_KEY as f64;
    key / final_key
}

/// https://drafts.csswg.org/web-animations-1/#enumdef-compositeoperationorauto
/// In the order of the host's `Bindings::CompositeOperationOrAuto`.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CompositeOperationOrAuto {
    Replace,
    Add,
    Accumulate,
    Auto,
}

/// The easing a keyframe runs to the next one.
#[derive(Clone)]
pub(crate) enum KeyframeEasing {
    /// The keyframe declares none, and runs its effect's.
    None,
    Easing(Easing),
    /// An easing still to be substituted against the element the effect is sampled on.
    Value(RetainedStyleValueData),
}

/// What a keyframe declares for a property.
#[derive(Clone, PartialEq)]
pub(crate) enum KeyframeValue {
    /// The element's own value, held by a keyframe the initial and final keyframes were generated as, which is not
    /// known until the element is sampled.
    ElementValue,
    Declared(RetainedStyleValueData),
}

impl KeyframeValue {
    /// The declared value, or `None` for the element's own value.
    #[must_use]
    pub(crate) fn declared(&self) -> Option<&RetainedStyleValueData> {
        match self {
            Self::ElementValue => None,
            Self::Declared(value) => Some(value),
        }
    }
}

/// A property a keyframe declares, which may be a shorthand.
pub(crate) struct KeyframeDeclaration {
    pub(crate) property_id: u16,
    pub(crate) value: KeyframeValue,
}

/// A custom property a keyframe declares.
pub(crate) struct KeyframeCustomDeclaration {
    pub(crate) name: RetainedUtf16FlyString,
    pub(crate) value: KeyframeValue,
}

/// https://drafts.csswg.org/web-animations-1/#keyframe
pub(crate) struct Keyframe {
    /// The keyframe's offset, scaled by [`KEY_SCALE_FACTOR`].
    pub(crate) key: u64,
    pub(crate) composite: CompositeOperationOrAuto,
    pub(crate) easing: KeyframeEasing,
    declarations: Range<u32>,
    custom_declarations: Range<u32>,
}

/// The style sheet the keyframes of a `@keyframes` rule come from, which their URLs resolve against.
#[derive(Clone)]
pub(crate) struct ResourceContext {
    /// Shared with every resolution of the keyframes' declarations that points into it.
    pub(crate) base_url: Arc<[u8]>,
    pub(crate) origin_clean: bool,
}

/// A property an effect animates: a longhand or shorthand by its id, or a custom property by its name.
#[derive(Clone)]
pub(crate) enum KeyframeProperty {
    Id(u16),
    Custom(RetainedUtf16FlyString),
}

impl PartialEq for KeyframeProperty {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::Id(first), Self::Id(second)) => first == second,
            // NB: Fly strings are interned, so equal names share their raw representation.
            (Self::Custom(first), Self::Custom(second)) => first.raw() == second.raw(),
            _ => false,
        }
    }
}

/// What the keyframes of a set were made from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum KeyframeOrigin {
    /// Keyframes script gave a `KeyframeEffect`, or anything else that is not CSS.
    Script,
    /// A `@keyframes` rule.
    KeyframesRule,
    /// A CSS transition.
    Transition,
}

/// https://drafts.csswg.org/css-animations-2/#keyframes
/// A keyframe block of a `@keyframes` rule as the rule declares it, once for each of the block's keys.
pub(crate) struct DeclaredKeyframe {
    /// The keyframe's offset, scaled by [`KEY_SCALE_FACTOR`].
    pub(crate) key: u64,
    pub(crate) composite: CompositeOperationOrAuto,
    /// The `animation-timing-function` the block declares where it names an easing without substituting anything;
    /// a block that declares none, or one that substitutes, runs its animation's.
    pub(crate) easing: Option<RetainedStyleValueData>,
    declarations: Range<u32>,
}

/// The keyframe blocks of a `@keyframes` rule in the order the rule declares them, before the keyframes of one key are
/// cascaded together, which script reads of the keyframes of a CSS animation.
pub(crate) struct DeclaredKeyframes {
    keyframes: Box<[DeclaredKeyframe]>,
    declarations: Box<[(KeyframeProperty, RetainedStyleValueData)]>,
}

impl DeclaredKeyframes {
    #[must_use]
    pub(crate) fn keyframes(&self) -> &[DeclaredKeyframe] {
        &self.keyframes
    }

    /// The properties the keyframe block declares, longhands and custom properties, in the order it declares them.
    #[must_use]
    pub(crate) fn declarations_of(&self, keyframe: &DeclaredKeyframe) -> &[(KeyframeProperty, RetainedStyleValueData)] {
        &self.declarations[keyframe.declarations.start as usize..keyframe.declarations.end as usize]
    }

    fn heap_size(&self) -> usize {
        size_of_val::<[DeclaredKeyframe]>(&self.keyframes)
            + size_of_val::<[(KeyframeProperty, RetainedStyleValueData)]>(&self.declarations)
    }
}

/// Where in a style sheet the keyframes of a set come from, which few sets have and none of a CSS transition does.
#[derive(Default)]
struct StyleSheetSource {
    resource_context: Option<ResourceContext>,
    declared_keyframes: Option<DeclaredKeyframes>,
}

impl StyleSheetSource {
    fn heap_size(&self) -> usize {
        size_of::<Self>()
            + self
                .resource_context
                .as_ref()
                .map_or(0, |context| crate::heap_size::ARC_HEADER_BYTES + context.base_url.len())
            + self.declared_keyframes.as_ref().map_or(0, DeclaredKeyframes::heap_size)
    }
}

/// The keyframes an effect runs, in the order they are used, with what follows from them alone.
pub struct KeyframeSet {
    keyframes: Box<[Keyframe]>,
    declarations: Box<[KeyframeDeclaration]>,
    custom_declarations: Box<[KeyframeCustomDeclaration]>,
    /// The style sheet the keyframes come from, which their URLs resolve against, and the keyframe blocks of the
    /// `@keyframes` rule they were made from, if they were.
    style_sheet_source: Option<Box<StyleSheetSource>>,
    /// The longhands the keyframes animate, sorted: what any shorthand they declare expands to.
    target_properties: Box<[u16]>,
    /// The custom properties the keyframes animate.
    target_custom_properties: Box<[RetainedUtf16FlyString]>,
    /// Whether a keyframe declares a custom property, or one that changes the layout tree's shape or what the main
    /// thread observes of an element's box, which a clock tick cannot sample.
    animates_what_a_tick_cannot: bool,
    /// Whether a keyframe declares a custom property or one the children of the target inherit.
    animates_an_inherited_property: bool,
    /// Whether a keyframe adds to or accumulates onto the value beneath it, whatever its effect does.
    has_additive_keyframe: bool,
    origin: KeyframeOrigin,
    /// The effective zoom the values of a CSS transition's keyframes were computed at, which script reads them
    /// unscaled by.
    computed_value_zoom: f32,
}

/// A keyframe a unit test gives a set: its key, composite operation, easing and declarations.
#[cfg(test)]
pub(crate) type KeyframeAsGiven = (
    u64,
    CompositeOperationOrAuto,
    KeyframeEasing,
    Vec<(KeyframeProperty, KeyframeValue)>,
);

/// A keyframe as a builder collects it, before the set lays its declarations out.
struct PendingKeyframe {
    key: u64,
    composite: CompositeOperationOrAuto,
    easing: KeyframeEasing,
    declarations: Vec<(KeyframeProperty, KeyframeValue)>,
}

impl PendingKeyframe {
    fn new(key: u64) -> Self {
        Self {
            key,
            composite: CompositeOperationOrAuto::Auto,
            easing: KeyframeEasing::None,
            declarations: Vec::new(),
        }
    }

    /// Sets what the keyframe declares for `property`, in place of what it declared before.
    fn set(&mut self, property: KeyframeProperty, value: KeyframeValue) {
        match self.declarations.iter_mut().find(|(declared, _)| *declared == property) {
            Some((_, declared_value)) => *declared_value = value,
            None => self.declarations.push((property, value)),
        }
    }

    /// Whether the keyframe declares `property`, or a shorthand of it.
    fn declares(&self, property: &KeyframeProperty) -> bool {
        use crate::css::property_metadata::{expanded_longhands_for_shorthand, property_is_shorthand};
        self.declarations
            .iter()
            .any(|(declared, _)| match (declared, property) {
                (KeyframeProperty::Id(shorthand), KeyframeProperty::Id(longhand))
                    if property_is_shorthand(*shorthand) =>
                {
                    expanded_longhands_for_shorthand(*shorthand).contains(longhand)
                }
                (declared, property) => declared == property,
            })
    }
}

/// The scaled key of a keyframe offset given as a percentage.
fn key_of_percentage(percentage: f64) -> u64 {
    #[expect(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let key = (percentage * KEY_SCALE_FACTOR) as u64;
    key
}

/// Whether a property, by its id, can be animated: a longhand whose animation type is not `none`, or a shorthand
/// with such a longhand.
pub fn is_animatable_property(property_id: u16) -> bool {
    use crate::css::property_metadata::{longhands_for_shorthand, property_animation_type, property_is_shorthand};
    if property_is_shorthand(property_id) {
        return longhands_for_shorthand(property_id)
            .iter()
            .any(|&longhand| is_animatable_property(longhand));
    }
    property_animation_type(property_id) != crate::css::animation::ANIMATION_TYPE_NONE
}

/// The animated properties of a declaration of `property_id` with `value`: the longhands it expands to.
fn expanded_longhands(
    property_id: u16,
    value: &RetainedStyleValueData,
    mut sink: impl FnMut(u16, &RetainedStyleValueData),
) {
    crate::css::style_compute::expand_shorthands_with(property_id, value, false, &mut |longhand, longhand_value, _| {
        sink(longhand, longhand_value);
    });
}

/// The animated properties a builder collected, without duplicates, in the order it found them.
#[derive(Default)]
struct AnimatedProperties(Vec<KeyframeProperty>);

impl AnimatedProperties {
    fn insert(&mut self, property: KeyframeProperty) {
        if !self.0.contains(&property) {
            self.0.push(property);
        }
    }
}

impl KeyframeSet {
    /// The keyframes of a `@keyframes` rule, given as each keyframe block's keys, which are percentages, and its
    /// declarations, from a style sheet with `resource_context`.
    ///
    /// Loosely based on https://drafts.csswg.org/css-animations-2/#keyframe-processing
    pub(crate) fn from_keyframes_rule<'a>(
        blocks: impl IntoIterator<Item = (&'a [f64], &'a crate::css::declaration_block::DeclarationBlockHeader)>,
        resource_context: Option<ResourceContext>,
    ) -> Self {
        use crate::css::css_enums::keyword;
        use crate::css::property_metadata::property_id;

        let mut keyframes: Vec<PendingKeyframe> = Vec::new();
        let mut animated_properties = AnimatedProperties::default();
        let mut declared_keyframes = Vec::new();
        let mut declared_declarations = Vec::new();

        // NB: Forwards pass, resolve all the user-specified keyframe properties.
        for (keys, block) in blocks {
            let mut resolved_keyframe = PendingKeyframe::new(0);
            let mut declared_easing = None;
            for declaration in block.properties() {
                let value = RetainedStyleValueData::from_arc(declaration.value.clone());
                match declaration.property_id {
                    property_id::ANIMATION_TIMING_FUNCTION => {
                        // NB: animation-timing-function is a list property, but inside @keyframes only a single value
                        //     is meaningful.
                        let value = match value.data() {
                            StyleValueData::ValueList { values, .. } => match values.as_slice().first() {
                                Some(first) => first.clone_retained(),
                                None => continue,
                            },
                            _ => value,
                        };
                        if crate::css::style_compute::value_is_css_wide_keyword(value.data()) {
                            continue;
                        }
                        resolved_keyframe.easing = match value.data() {
                            StyleValueData::Easing(_) | StyleValueData::Keyword { .. } => {
                                declared_easing = Some(value.clone_retained());
                                KeyframeEasing::Easing(
                                    crate::css::style::effect_descriptions::easing_from_computed_timing_function(
                                        value.data(),
                                    )
                                    .expect("an easing or keyword timing function describes an easing"),
                                )
                            }
                            _ => {
                                declared_easing = None;
                                KeyframeEasing::Value(value)
                            }
                        };
                    }
                    property_id::ANIMATION_COMPOSITION => {
                        let value = match value.data() {
                            StyleValueData::ValueList { values, .. } if values.len() == 1 => {
                                values.as_slice()[0].clone_retained()
                            }
                            _ => value,
                        };
                        resolved_keyframe.composite = match value.data() {
                            StyleValueData::Keyword { keyword: keyword::ADD } => CompositeOperationOrAuto::Add,
                            StyleValueData::Keyword {
                                keyword: keyword::ACCUMULATE,
                            } => CompositeOperationOrAuto::Accumulate,
                            _ => CompositeOperationOrAuto::Replace,
                        };
                    }
                    property if !is_animatable_property(property) => {}
                    property => {
                        // NB: Unresolved properties are resolved when the effect is sampled.
                        expanded_longhands(property, &value, |longhand, longhand_value| {
                            animated_properties.insert(KeyframeProperty::Id(longhand));
                            resolved_keyframe.set(
                                KeyframeProperty::Id(longhand),
                                KeyframeValue::Declared(longhand_value.clone_retained()),
                            );
                        });
                    }
                }
            }
            for custom_property in block.custom_properties().iter() {
                let name = RetainedUtf16FlyString::from_utf16(custom_property.name.units());
                animated_properties.insert(KeyframeProperty::Custom(name.clone()));
                resolved_keyframe.set(
                    KeyframeProperty::Custom(name),
                    KeyframeValue::Declared(RetainedStyleValueData::from_arc(
                        custom_property.declaration.value.clone(),
                    )),
                );
            }

            let first_declaration = declaration_index(declared_declarations.len());
            declared_declarations.extend(
                resolved_keyframe
                    .declarations
                    .iter()
                    .filter_map(|(property, value)| Some((property.clone(), value.declared()?.clone_retained()))),
            );
            let declarations = first_declaration..declaration_index(declared_declarations.len());
            for &key in keys {
                let key = key_of_percentage(key);
                declared_keyframes.push(DeclaredKeyframe {
                    key,
                    composite: resolved_keyframe.composite,
                    easing: declared_easing.as_ref().map(RetainedStyleValueData::clone_retained),
                    declarations: declarations.clone(),
                });
                match keyframes.iter_mut().find(|keyframe| keyframe.key == key) {
                    Some(existing_keyframe) => {
                        for (property, value) in &resolved_keyframe.declarations {
                            existing_keyframe.set(property.clone(), value.clone());
                        }
                        if resolved_keyframe.composite != CompositeOperationOrAuto::Auto {
                            existing_keyframe.composite = resolved_keyframe.composite;
                        }
                        if !matches!(resolved_keyframe.easing, KeyframeEasing::None) {
                            existing_keyframe.easing = resolved_keyframe.easing.clone();
                        }
                    }
                    None => keyframes.push(PendingKeyframe {
                        key,
                        composite: resolved_keyframe.composite,
                        easing: resolved_keyframe.easing.clone(),
                        declarations: resolved_keyframe.declarations.clone(),
                    }),
                }
            }
        }

        let mut set = Self::with_initial_and_final_frames(keyframes, &animated_properties, resource_context);
        set.origin = KeyframeOrigin::KeyframesRule;
        set.style_sheet_source.get_or_insert_default().declared_keyframes = Some(DeclaredKeyframes {
            keyframes: declared_keyframes.into(),
            declarations: declared_declarations.into(),
        });
        set
    }

    /// The keyframes script gave a `KeyframeEffect`, each with its computed offset already scaled to a key, in the
    /// order they were given, from a style sheet with `resource_context` where they were copied from keyframes of CSS.
    pub(crate) fn from_processed_keyframes(
        keyframes: Vec<ProcessedKeyframe>,
        resource_context: Option<ResourceContext>,
    ) -> Self {
        let mut animated_properties = AnimatedProperties::default();
        let keyframes = keyframes
            .into_iter()
            .map(|keyframe| {
                let mut resolved_keyframe = PendingKeyframe::new(keyframe.key);
                resolved_keyframe.composite = keyframe.composite;
                resolved_keyframe.easing = keyframe.easing.map_or(KeyframeEasing::None, KeyframeEasing::Easing);
                for (property, value) in keyframe.declarations {
                    match &property {
                        KeyframeProperty::Custom(_) => animated_properties.insert(property.clone()),
                        &KeyframeProperty::Id(property_id) => expanded_longhands(property_id, &value, |longhand, _| {
                            animated_properties.insert(KeyframeProperty::Id(longhand));
                        }),
                    }
                    resolved_keyframe.set(property, KeyframeValue::Declared(value));
                }
                resolved_keyframe
            })
            .collect();
        Self::with_initial_and_final_frames(keyframes, &animated_properties, resource_context)
    }

    /// The keyframes of a computation of the values `frames` declare over an element, each property by its id, keyed
    /// by their position, replacing the value beneath them, from a style sheet with `resource_context`. No initial or
    /// final keyframe is made of what they leave out.
    pub(crate) fn for_value_computation(
        frames: Vec<Vec<(u16, RetainedStyleValueData)>>,
        resource_context: Option<ResourceContext>,
    ) -> Self {
        let mut animated_properties = AnimatedProperties::default();
        let keyframes = frames
            .into_iter()
            .enumerate()
            .map(|(index, declarations)| {
                let mut keyframe = PendingKeyframe::new(index as u64);
                keyframe.composite = CompositeOperationOrAuto::Replace;
                for (property_id, value) in declarations {
                    animated_properties.insert(KeyframeProperty::Id(property_id));
                    keyframe.set(KeyframeProperty::Id(property_id), KeyframeValue::Declared(value));
                }
                keyframe
            })
            .collect();
        Self::laid_out(keyframes, &animated_properties, resource_context)
    }

    /// The keyframes of a CSS transition of `property` from `start` to `end`, values computed at the effective zoom
    /// `computed_value_zoom`.
    pub(crate) fn for_transition(
        property: KeyframeProperty,
        start: RetainedStyleValueData,
        end: RetainedStyleValueData,
        computed_value_zoom: f32,
    ) -> Self {
        let keyframe = |key, value| {
            let mut keyframe = PendingKeyframe::new(key);
            keyframe.set(property.clone(), KeyframeValue::Declared(value));
            keyframe
        };
        let mut animated_properties = AnimatedProperties::default();
        animated_properties.insert(property.clone());
        let mut set = Self::laid_out(
            vec![keyframe(INITIAL_KEY, start), keyframe(FINAL_KEY, end)],
            &animated_properties,
            None,
        );
        set.origin = KeyframeOrigin::Transition;
        set.computed_value_zoom = computed_value_zoom;
        set
    }

    /// The set of `keyframes`, each given as its key, composite operation, easing and declarations, exactly as given,
    /// for a unit test.
    #[cfg(test)]
    pub(crate) fn from_keyframes_as_given(keyframes: Vec<KeyframeAsGiven>) -> Self {
        let mut animated_properties = AnimatedProperties::default();
        let keyframes = keyframes
            .into_iter()
            .map(|(key, composite, easing, declarations)| {
                for (property, _) in &declarations {
                    animated_properties.insert(property.clone());
                }
                PendingKeyframe {
                    key,
                    composite,
                    easing,
                    declarations,
                }
            })
            .collect();
        Self::laid_out(keyframes, &animated_properties, None)
    }

    /// The set of `keyframes`, once the initial and final keyframes are generated for `animated_properties`.
    fn with_initial_and_final_frames(
        mut keyframes: Vec<PendingKeyframe>,
        animated_properties: &AnimatedProperties,
        resource_context: Option<ResourceContext>,
    ) -> Self {
        // NB: The keyframes are used in the order of their keys, and those of one key in the order they were given.
        keyframes.sort_by_key(|keyframe| keyframe.key);
        generate_initial_and_final_frames(&mut keyframes, animated_properties);
        Self::laid_out(keyframes, animated_properties, resource_context)
    }

    fn laid_out(
        keyframes: Vec<PendingKeyframe>,
        animated_properties: &AnimatedProperties,
        resource_context: Option<ResourceContext>,
    ) -> Self {
        use crate::css::property_metadata::{property_id, property_is_inherited};

        let mut declarations = Vec::new();
        let mut custom_declarations = Vec::new();
        let mut animates_what_a_tick_cannot = false;
        let mut animates_an_inherited_property = false;
        let mut has_additive_keyframe = false;
        let index = declaration_index;
        let keyframes = keyframes
            .into_iter()
            .map(|keyframe| {
                has_additive_keyframe |= matches!(
                    keyframe.composite,
                    CompositeOperationOrAuto::Add | CompositeOperationOrAuto::Accumulate
                );
                let first_declaration = index(declarations.len());
                let first_custom_declaration = index(custom_declarations.len());
                for (property, value) in keyframe.declarations {
                    match property {
                        KeyframeProperty::Id(property_id) => {
                            animates_what_a_tick_cannot |= matches!(
                                property_id,
                                property_id::DISPLAY | property_id::VISIBILITY | property_id::CONTENT_VISIBILITY
                            );
                            animates_an_inherited_property |= property_is_inherited(property_id);
                            declarations.push(KeyframeDeclaration { property_id, value });
                        }
                        KeyframeProperty::Custom(name) => {
                            animates_what_a_tick_cannot = true;
                            animates_an_inherited_property = true;
                            custom_declarations.push(KeyframeCustomDeclaration { name, value });
                        }
                    }
                }
                Keyframe {
                    key: keyframe.key,
                    composite: keyframe.composite,
                    easing: keyframe.easing,
                    declarations: first_declaration..index(declarations.len()),
                    custom_declarations: first_custom_declaration..index(custom_declarations.len()),
                }
            })
            .collect();

        let mut target_properties: Vec<u16> = Vec::new();
        let mut target_custom_properties = Vec::new();
        for property in &animated_properties.0 {
            match property {
                KeyframeProperty::Id(property_id) => target_properties.push(*property_id),
                KeyframeProperty::Custom(name) => target_custom_properties.push(name.clone()),
            }
        }
        target_properties.sort_unstable();
        target_properties.dedup();

        Self {
            keyframes,
            declarations: declarations.into(),
            custom_declarations: custom_declarations.into(),
            style_sheet_source: resource_context.map(|resource_context| {
                Box::new(StyleSheetSource {
                    resource_context: Some(resource_context),
                    declared_keyframes: None,
                })
            }),
            target_properties: target_properties.into(),
            target_custom_properties: target_custom_properties.into(),
            animates_what_a_tick_cannot,
            animates_an_inherited_property,
            has_additive_keyframe,
            origin: KeyframeOrigin::Script,
            computed_value_zoom: 1.0,
        }
    }

    /// Another reference to the set `set` names.
    ///
    /// # Safety
    /// `set` must be a keyframe set the host holds a reference to.
    #[must_use]
    pub(crate) unsafe fn retained_from_host(set: *const Self) -> Arc<Self> {
        // SAFETY: Guaranteed by the caller.
        unsafe {
            Arc::increment_strong_count(set);
            Arc::from_raw(set)
        }
    }

    /// The keyframes, in the order they are used.
    #[must_use]
    pub(crate) fn keyframes(&self) -> &[Keyframe] {
        &self.keyframes
    }

    #[must_use]
    pub(crate) fn declarations_of(&self, keyframe: &Keyframe) -> &[KeyframeDeclaration] {
        &self.declarations[keyframe.declarations.start as usize..keyframe.declarations.end as usize]
    }

    #[must_use]
    pub(crate) fn custom_declarations_of(&self, keyframe: &Keyframe) -> &[KeyframeCustomDeclaration] {
        &self.custom_declarations
            [keyframe.custom_declarations.start as usize..keyframe.custom_declarations.end as usize]
    }

    /// The custom properties all the keyframes declare, in the order they are used.
    #[must_use]
    pub(crate) fn custom_declarations(&self) -> &[KeyframeCustomDeclaration] {
        &self.custom_declarations
    }

    /// The style sheet the keyframes come from, which their URLs resolve against.
    #[must_use]
    pub(crate) fn resource_context(&self) -> Option<&ResourceContext> {
        self.style_sheet_source.as_ref()?.resource_context.as_ref()
    }

    #[must_use]
    pub(crate) fn origin(&self) -> KeyframeOrigin {
        self.origin
    }

    /// The keyframe blocks of the `@keyframes` rule the keyframes were made from, if they were.
    #[must_use]
    pub(crate) fn declared_keyframes(&self) -> Option<&DeclaredKeyframes> {
        self.style_sheet_source.as_ref()?.declared_keyframes.as_ref()
    }

    /// The effective zoom the values of a CSS transition's keyframes were computed at.
    #[must_use]
    pub(crate) fn computed_value_zoom(&self) -> f32 {
        self.computed_value_zoom
    }

    /// The longhands the keyframes animate, sorted.
    #[must_use]
    pub(crate) fn target_properties(&self) -> &[u16] {
        &self.target_properties
    }

    /// The custom properties the keyframes animate.
    #[must_use]
    pub(crate) fn target_custom_properties(&self) -> &[RetainedUtf16FlyString] {
        &self.target_custom_properties
    }

    /// Whether the compositor may run all the keyframes animate.
    #[must_use]
    pub(crate) fn may_run_on_the_compositor(&self) -> bool {
        use crate::css::property_metadata::property_id;
        self.target_custom_properties.is_empty()
            && self.target_properties.iter().all(|property| {
                matches!(
                    *property,
                    property_id::OPACITY
                        | property_id::BACKGROUND_COLOR
                        | property_id::FILTER
                        | property_id::TRANSLATE
                        | property_id::ROTATE
                        | property_id::SCALE
                        | property_id::TRANSFORM
                )
            })
    }

    #[must_use]
    pub(crate) fn animates_what_a_tick_cannot(&self) -> bool {
        self.animates_what_a_tick_cannot
    }

    #[must_use]
    pub(crate) fn animates_an_inherited_property(&self) -> bool {
        self.animates_an_inherited_property
    }

    #[must_use]
    pub(crate) fn has_additive_keyframe(&self) -> bool {
        self.has_additive_keyframe
    }

    /// Every value the keyframes declare or run their easing by.
    pub(crate) fn declared_values(&self) -> impl Iterator<Item = &StyleValueData> {
        let declared = self
            .declarations
            .iter()
            .map(|declaration| &declaration.value)
            .chain(self.custom_declarations.iter().map(|declaration| &declaration.value))
            .filter_map(KeyframeValue::declared);
        let easings = self.keyframes.iter().filter_map(|keyframe| match &keyframe.easing {
            KeyframeEasing::Value(value) => Some(value),
            KeyframeEasing::None | KeyframeEasing::Easing(_) => None,
        });
        declared
            .chain(easings)
            .filter_map(RetainedStyleValueData::optional_data)
    }

    /// The bytes the set holds on the heap, beside itself. The values it declares are their rules'.
    #[must_use]
    pub(crate) fn heap_size(&self) -> usize {
        self.style_sheet_source.as_ref().map_or(0, |source| source.heap_size())
            + size_of_val::<[Keyframe]>(&self.keyframes)
            + self
                .keyframes
                .iter()
                .map(|keyframe| match &keyframe.easing {
                    KeyframeEasing::Easing(Easing::Linear(points)) => crate::heap_size::vec_bytes(points),
                    _ => 0,
                })
                .sum::<usize>()
            + size_of_val::<[KeyframeDeclaration]>(&self.declarations)
            + size_of_val::<[KeyframeCustomDeclaration]>(&self.custom_declarations)
            + size_of_val::<[u16]>(&self.target_properties)
            + size_of_val::<[RetainedUtf16FlyString]>(&self.target_custom_properties)
    }
}

/// The index of the next of a set's declarations, where it holds `length` of them.
fn declaration_index(length: usize) -> u32 {
    u32::try_from(length).expect("a keyframe set holds fewer than 2^32 declarations")
}

/// A keyframe script gave a `KeyframeEffect`, as processing the keyframes argument made it.
pub(crate) struct ProcessedKeyframe {
    /// The computed keyframe offset, scaled by [`KEY_SCALE_FACTOR`] as a percentage.
    pub(crate) key: u64,
    pub(crate) composite: CompositeOperationOrAuto,
    pub(crate) easing: Option<Easing>,
    pub(crate) declarations: Vec<(KeyframeProperty, RetainedStyleValueData)>,
}

// https://drafts.csswg.org/css-animations-2/#keyframe-processing
fn generate_initial_and_final_frames(keyframes: &mut Vec<PendingKeyframe>, animated_properties: &AnimatedProperties) {
    // 1. Find or create the initial keyframe, a keyframe with a keyframe offset of 0%, default timing function
    //    as its keyframe timing function, and default composite as its keyframe composite.
    if keyframes.first().is_none_or(|keyframe| keyframe.key != INITIAL_KEY) {
        keyframes.insert(0, PendingKeyframe::new(INITIAL_KEY));
    }

    // 2. For any property in animated properties that is not otherwise present in a keyframe with an offset of
    //    0% or one that would be positioned earlier in the used keyframe order, add the computed value of that
    //    property on element to initial keyframe’s keyframe values.
    let initial_keyframes = keyframes
        .iter()
        .take_while(|keyframe| keyframe.key == INITIAL_KEY)
        .count();
    let missing: Vec<_> = animated_properties
        .0
        .iter()
        .filter(|property| {
            !keyframes[..initial_keyframes]
                .iter()
                .any(|keyframe| keyframe.declares(property))
        })
        .cloned()
        .collect();
    for property in missing {
        keyframes[0].set(property, KeyframeValue::ElementValue);
    }

    // 3. If initial keyframe’s keyframe values is not empty, prepend initial keyframe to keyframes.

    // 4. Repeat for final keyframe, using an offset of 100%, considering keyframes positioned later in the used
    //    keyframe order, and appending to keyframes.
    if keyframes.last().is_none_or(|keyframe| keyframe.key != FINAL_KEY) {
        keyframes.push(PendingKeyframe::new(FINAL_KEY));
    }
    let final_keyframes = keyframes
        .iter()
        .rev()
        .take_while(|keyframe| keyframe.key == FINAL_KEY)
        .count();
    let first_final_keyframe = keyframes.len() - final_keyframes;
    let missing: Vec<_> = animated_properties
        .0
        .iter()
        .filter(|property| {
            !keyframes[first_final_keyframe..]
                .iter()
                .any(|keyframe| keyframe.declares(property))
        })
        .cloned()
        .collect();
    let final_keyframe = keyframes.last_mut().expect("the final keyframe was found or created");
    for property in missing {
        final_keyframe.set(property, KeyframeValue::ElementValue);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn value(keyword: u16) -> RetainedStyleValueData {
        RetainedStyleValueData::from_owned(StyleValueData::Keyword { keyword })
    }

    fn declared_properties(set: &KeyframeSet, keyframe: &Keyframe) -> Vec<(u16, bool)> {
        set.declarations_of(keyframe)
            .iter()
            .map(|declaration| (declaration.property_id, declaration.value.declared().is_some()))
            .collect()
    }

    #[test]
    fn script_keyframes_gain_initial_and_final_keyframes_for_what_they_leave_out() {
        use crate::css::css_enums::keyword;
        use crate::css::property_metadata::property_id;
        let keyframe = |key, property| ProcessedKeyframe {
            key,
            composite: CompositeOperationOrAuto::Auto,
            easing: None,
            declarations: vec![(KeyframeProperty::Id(property), value(keyword::AUTO))],
        };
        let set = KeyframeSet::from_processed_keyframes(
            vec![
                keyframe(50_000, property_id::OPACITY),
                keyframe(FINAL_KEY, property_id::COLOR),
            ],
            None,
        );
        let keys: Vec<_> = set.keyframes().iter().map(|keyframe| keyframe.key).collect();
        assert_eq!(keys, [INITIAL_KEY, 50_000, FINAL_KEY]);
        let mut initial = declared_properties(&set, &set.keyframes()[0]);
        initial.sort_unstable();
        let mut expected = vec![(property_id::OPACITY, false), (property_id::COLOR, false)];
        expected.sort_unstable();
        assert_eq!(initial, expected);
        let mut last = declared_properties(&set, &set.keyframes()[2]);
        last.sort_unstable();
        let mut expected = vec![(property_id::COLOR, true), (property_id::OPACITY, false)];
        expected.sort_unstable();
        assert_eq!(last, expected);
        let mut target_properties = vec![property_id::OPACITY, property_id::COLOR];
        target_properties.sort_unstable();
        assert_eq!(set.target_properties(), target_properties);
    }

    #[test]
    fn keyframes_of_one_offset_keep_their_order() {
        use crate::css::css_enums::keyword;
        use crate::css::property_metadata::property_id;
        let keyframe = |key, composite| ProcessedKeyframe {
            key,
            composite,
            easing: None,
            declarations: vec![(KeyframeProperty::Id(property_id::OPACITY), value(keyword::AUTO))],
        };
        let set = KeyframeSet::from_processed_keyframes(
            vec![
                keyframe(FINAL_KEY, CompositeOperationOrAuto::Add),
                keyframe(INITIAL_KEY, CompositeOperationOrAuto::Replace),
                keyframe(FINAL_KEY, CompositeOperationOrAuto::Accumulate),
            ],
            None,
        );
        let composites: Vec<_> = set.keyframes().iter().map(|keyframe| keyframe.composite).collect();
        assert_eq!(
            composites,
            [
                CompositeOperationOrAuto::Replace,
                CompositeOperationOrAuto::Add,
                CompositeOperationOrAuto::Accumulate
            ]
        );
        assert!(set.has_additive_keyframe());
    }
}
