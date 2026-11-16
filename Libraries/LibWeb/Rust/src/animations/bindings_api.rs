/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What libweb_bindings_rust, which implements the interfaces of Web Animations, uses of the records of animation
//! effects and of the parsing their attributes do.

use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

use super::animation::{AnimationCell, AnimationHandle, AnimationKind, AnimationRecordRef};
use super::composite_order::composite_order;
use super::easing::CompactEasing;
use super::effect::{AnimationEffectCell, EffectHandle};
use super::effect::{EffectTimingAtLocalTime, resolve_effect_timing};
use super::ffi::{FfiScrollTimelineSource, SCROLLER_NEAREST, retained_view_inset};
use super::ffi::{FfiTreeOrder, host_tree_order};
use super::ffi::{invalidate, run_procedure, run_procedure_on};
use super::keyframes::{KeyframeProperty, KeyframeSet, ProcessedKeyframe, key_of_offset};
use super::procedures::AnimationError;
use super::timeline::{TimelineHandle, TimelineKind, ViewTimelineInset, ViewTimelineInsetEdge};
use super::timing::{AnimationEffectTiming, FillMode, SpecifiedEffectTiming, TimeValue};
use crate::css::animation::FfiCompositeOperation;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style::effect_descriptions::easing_from_computed_timing_function;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData, retain_style_value};
use libcompositing_rust::easing::{Easing, FfiEasingKind};

pub use super::ffi::{
    FfiHostEffect as HostEffect, HostEffectPerformer, element_node_document,
    web_animation_compare_elements_in_tree_order,
};
pub use super::keyframes::CompositeOperationOrAuto;
pub use crate::css::animation::FfiCompositeOperation as CompositeOperation;
pub use libcompositing_rust::animation_timing::{AnimationPlayState, AnimationReplaceState};

/// An easing function parsed from script, with the serialization the effect reports it as.
#[derive(Clone)]
pub struct ParsedEasing {
    easing: Easing,
    serialization: ak::Utf16String,
}

impl ParsedEasing {
    #[must_use]
    pub fn serialization(&self) -> &ak::Utf16String {
        &self.serialization
    }

    /// The `linear` keyword.
    pub(crate) fn linear() -> Self {
        Self {
            easing: Easing::default(),
            serialization: ak::Utf16String::from_ascii(b"linear"),
        }
    }

    /// The `ease` keyword.
    pub(crate) fn ease() -> Self {
        Self {
            easing: super::easing::keyword_cubic_bezier(0),
            serialization: ak::Utf16String::from_ascii(b"ease"),
        }
    }

    /// The easing the computed value of a timing function `value` describes, serialized as a script reads it, or none
    /// where it describes none.
    pub(crate) fn of_timing_function(value: &RetainedStyleValueData) -> Option<Self> {
        Self::of_timing_function_data(value.data(), |mode| value.serialize(mode))
    }

    /// The easing the computed value of a timing function `value` describes, with `serialize` serializing the value in
    /// the mode a script reads it in, or none where it describes none.
    fn of_timing_function_data(value: &StyleValueData, serialize: impl FnOnce(u8) -> ak::Utf16String) -> Option<Self> {
        let easing = easing_from_computed_timing_function(value)?;
        let mode = if matches!(value, StyleValueData::Easing(_)) && easing.kind() == FfiEasingKind::CubicBezier {
            SERIALIZATION_MODE_NORMAL
        } else {
            SERIALIZATION_MODE_RESOLVED_VALUE
        };
        Some(Self {
            easing,
            serialization: serialize(mode),
        })
    }
}

// NB: The numeric values of the C++ enum SerializationMode.
const SERIALIZATION_MODE_NORMAL: u8 = 0;
const SERIALIZATION_MODE_RESOLVED_VALUE: u8 = 1;

/// The easing function the text parses as, as the easing member of EffectTiming is parsed using the
/// <easing-function> production [CSS-EASING-1].
pub fn parse_easing_function(text: &ak::Utf16String) -> Option<ParsedEasing> {
    use crate::css::property_metadata::property_id;
    let value = parse_property_value(property_id::ANIMATION_TIMING_FUNCTION, text)?;
    if value.is_unresolved() || value.is_css_wide_keyword() {
        return None;
    }
    let items = value.value_list_items()?;
    let [item] = items.as_slice() else {
        return None;
    };
    // FIXME: We should absolutize the style value to resolve relative lengths within calcs
    ParsedEasing::of_timing_function_data(item.data(), |mode| item.serialize(mode))
}

/// A pseudo-element selector parsed from script: the host's `CSS::PseudoElement`, and its serialization.
#[derive(Clone)]
pub struct ParsedPseudoElement {
    kind: u8,
    serialization: ak::Utf16String,
}

/// The pseudo-element the text parses as, as a <pseudo-element-selector>.
pub fn parse_pseudo_element_selector(text: &ak::Utf16String) -> Option<ParsedPseudoElement> {
    use crate::css::selector::PseudoElementType;
    let units = text.to_utf16();
    let (selector, pseudo_element) =
        crate::css::selector_parser::parse_pseudo_element_selector_from_utf16(&units).ok()?;
    // NB: The host's enum has KnownPseudoElementCount between the known pseudo-elements and UnknownWebKit.
    let kind = if pseudo_element == PseudoElementType::UnknownWebKit {
        pseudo_element as u8 + 1
    } else {
        pseudo_element as u8
    };
    Some(ParsedPseudoElement {
        kind,
        serialization: crate::css::selector_serialization::serialize_selector_ref(&selector),
    })
}

/// The id of the property the IDL attribute name names, ignoring ASCII case, as the C++ property_id_from_camel_case_string()
/// finds it, with legacy aliases mapping to the property they alias.
pub fn property_id_from_camel_case(name: &ak::Utf16String) -> Option<u16> {
    use crate::css::property_metadata::PROPERTY_CAMEL_CASE_LOOKUP;
    let units = name.to_utf16();
    let lowercase: Vec<u16> = units
        .iter()
        .map(|&unit| crate::css::ffi_support::ascii_lowercase(unit))
        .collect();
    PROPERTY_CAMEL_CASE_LOOKUP
        .binary_search_by(|(candidate, _)| candidate.bytes().map(u16::from).cmp(lowercase.iter().copied()))
        .ok()
        .map(|index| PROPERTY_CAMEL_CASE_LOOKUP[index].1)
}

pub use super::keyframes::is_animatable_property;
pub use crate::css::property_metadata::property_id::{
    ANIMATION_RANGE_END as PROPERTY_ID_ANIMATION_RANGE_END, ANIMATION_RANGE_START as PROPERTY_ID_ANIMATION_RANGE_START,
    CUSTOM as PROPERTY_ID_CUSTOM, FLOAT as PROPERTY_ID_FLOAT, OFFSET as PROPERTY_ID_OFFSET,
    VIEW_TIMELINE_INSET as PROPERTY_ID_VIEW_TIMELINE_INSET,
};
pub use crate::css::property_metadata::property_idl_name as camel_case_property_name;
pub use crate::css::typed_om::parse_value_for_property as parse_property_value;

/// A property a processed keyframe declares: a longhand or shorthand by its id, or a custom property by its name.
#[derive(Clone)]
pub enum KeyframePropertyName {
    Id(u16),
    Custom(ak::Utf16String),
}

/// A value of a property in a processed keyframe.
pub type StyleValue = RetainedStyleValueData;

/// Another reference to a value of a property.
#[must_use]
pub fn clone_property_value(value: &StyleValue) -> StyleValue {
    value.clone_retained()
}

/// The serialization of a value of a property, as script reads it.
pub fn serialize_property_value(value: &StyleValue) -> ak::Utf16String {
    value.serialize(SERIALIZATION_MODE_NORMAL)
}

/// One keyframe a keyframe set is built from: its computed keyframe offset, its composite operation, its easing and
/// the properties it declares.
pub struct KeyframeSetInput<'a> {
    pub computed_offset: f64,
    pub composite: CompositeOperationOrAuto,
    pub easing: &'a ParsedEasing,
    pub properties: &'a [(KeyframePropertyName, StyleValue)],
}

/// A keyframe of CSS as script reads it: its offset, easing and composite operation, and the properties it declares.
pub struct CssKeyframeValues {
    pub offset: f64,
    pub easing: ParsedEasing,
    pub composite: CompositeOperationOrAuto,
    pub properties: Vec<(KeyframePropertyName, StyleValue)>,
}

impl From<super::keyframe_reflection::CssKeyframe> for CssKeyframeValues {
    fn from(keyframe: super::keyframe_reflection::CssKeyframe) -> Self {
        Self {
            offset: keyframe.offset,
            easing: keyframe.easing,
            composite: keyframe.composite,
            properties: keyframe
                .properties
                .into_iter()
                .map(|(property, value)| {
                    let property = match property {
                        KeyframeProperty::Id(property_id) => KeyframePropertyName::Id(property_id),
                        KeyframeProperty::Custom(name) => {
                            KeyframePropertyName::Custom(ak::Utf16String::from_utf16(&name.to_utf16()))
                        }
                    };
                    (property, value)
                })
                .collect(),
        }
    }
}

/// The keyframe set the style engine runs of keyframes script gave a KeyframeEffect.
pub struct KeyframeSetRef(Arc<KeyframeSet>);

impl KeyframeSetRef {
    pub fn from_keyframes(keyframes: &[KeyframeSetInput<'_>]) -> Self {
        Self::from_keyframes_with_resource_context(keyframes, None)
    }

    /// The set of keyframes a copy of the effect `source` takes, whose URLs resolve against the style sheet the
    /// keyframes of `source` come from.
    pub fn from_keyframes_of_copy(keyframes: &[KeyframeSetInput<'_>], source: &EffectRecordRef) -> Self {
        Self::from_keyframes_with_resource_context(
            keyframes,
            super::keyframe_reflection::resource_context(&source.0.borrow()),
        )
    }

    fn from_keyframes_with_resource_context(
        keyframes: &[KeyframeSetInput<'_>],
        resource_context: Option<super::keyframes::ResourceContext>,
    ) -> Self {
        let keyframes = keyframes
            .iter()
            .map(|keyframe| ProcessedKeyframe {
                key: key_of_offset(keyframe.computed_offset),
                composite: keyframe.composite,
                easing: Some(keyframe.easing.easing.clone()),
                declarations: keyframe
                    .properties
                    .iter()
                    .map(|(property, value)| {
                        let property = match property {
                            KeyframePropertyName::Id(property_id) => KeyframeProperty::Id(*property_id),
                            KeyframePropertyName::Custom(name) => {
                                KeyframeProperty::Custom(RetainedUtf16FlyString::from_utf16(&name.to_utf16()))
                            }
                        };
                        (property, value.clone_retained())
                    })
                    .collect(),
            })
            .collect();
        Self(Arc::new(KeyframeSet::from_processed_keyframes(
            keyframes,
            resource_context,
        )))
    }

    /// Hands the reference to the host.
    #[must_use]
    pub fn into_host_ptr(self) -> *const c_void {
        Arc::into_raw(self.0).cast()
    }
}

/// A strong reference to an animation effect, as what libweb_bindings_rust holds of one.
#[derive(Clone)]
pub struct EffectRecordRef(EffectHandle);

impl Default for EffectRecordRef {
    fn default() -> Self {
        Self::new()
    }
}

impl EffectRecordRef {
    /// A new effect with the default timing, no keyframes and no target.
    #[must_use]
    pub fn new() -> Self {
        Self(EffectHandle::new())
    }

    /// Takes a reference of its own to an effect the host holds a reference to.
    ///
    /// # Safety
    /// `effect` must be an effect the host holds a reference to.
    pub unsafe fn retained_from_host(effect: *const c_void) -> Self {
        // SAFETY: Guaranteed by the caller.
        Self(unsafe { EffectHandle::retained_from_host(effect.cast()) })
    }

    /// The effect, as the host names it.
    #[must_use]
    pub fn as_host_ptr(&self) -> *const c_void {
        std::ptr::from_ref::<AnimationEffectCell>(&self.0).cast()
    }

    /// Invalidates the style the effect produces, forgetting what the host concluded of it.
    pub fn invalidate(&self) {
        self.0.borrow_mut().forget_what_the_host_concluded();
        super::compositor_update::set_needs_animated_style_update(&self.0);
    }

    /// Notes the cell of the object script sees of the effect, or that it has none, which the host's code reaches it
    /// through.
    pub fn set_object_cell(&self, object_cell: Option<NonNull<c_void>>) {
        self.0.set_object_cell(object_cell);
    }

    #[must_use]
    pub fn specified_timing(&self) -> SpecifiedEffectTiming {
        *self.0.borrow().specified_timing()
    }

    pub fn set_specified_timing(&self, specified: SpecifiedEffectTiming) {
        self.0.borrow_mut().set_specified_timing(specified);
    }

    // https://drafts.csswg.org/web-animations-2/#normalize-specified-timing
    pub fn normalize_specified_timing(&self) {
        let animation = self.0.borrow().associated_animation();
        let timeline = animation.and_then(|animation| animation.borrow().timeline_state());
        self.0.borrow_mut().normalize_specified_timing(timeline.as_ref());
    }

    /// The normalized timing the timing model reads of the effect.
    #[must_use]
    pub fn timing(&self) -> AnimationEffectTiming {
        self.0.borrow().timing()
    }

    /// The serialization of the value the timing function was made from.
    #[must_use]
    pub fn timing_function_serialization(&self) -> ak::Utf16String {
        ak::Utf16String::from_utf16(&self.0.borrow().timing_function_serialization().to_utf16())
    }

    pub fn set_timing_function(&self, timing_function: &ParsedEasing) {
        self.0.borrow_mut().set_timing_function(
            CompactEasing::of(timing_function.easing.clone()),
            RetainedUtf16FlyString::from_utf16(&timing_function.serialization.to_utf16()),
        );
    }

    /// Sets the specified timing and the timing function of the effect to those of `source`.
    pub fn copy_specified_timing_and_timing_function_from(&self, source: &Self) {
        let (specified, timing_function, serialization) = {
            let source = source.0.borrow();
            (
                *source.specified_timing(),
                source.timing_function().clone(),
                source.timing_function_serialization().clone(),
            )
        };
        let mut effect = self.0.borrow_mut();
        effect.set_specified_timing(specified);
        effect.set_timing_function(timing_function, serialization);
    }

    /// The animation whose associated effect this effect is.
    #[must_use]
    pub fn associated_animation(&self) -> Option<AnimationRecordRef> {
        self.0
            .borrow()
            .associated_animation()
            .map(AnimationRecordRef::from_handle)
    }

    /// The timing of the effect at `local_time` with `fill_mode`, on the timeline of its associated animation, if any,
    /// with its transformed progress where `with_transformed_progress`.
    #[must_use]
    pub fn resolve_timing(
        &self,
        local_time: Option<TimeValue>,
        fill_mode: FillMode,
        with_transformed_progress: bool,
    ) -> EffectTimingAtLocalTime {
        let animation = self.0.borrow().associated_animation();
        resolve_effect_timing(
            &self.0,
            local_time,
            fill_mode,
            animation.as_deref().map(|animation: &AnimationCell| animation),
            with_transformed_progress,
        )
    }

    /// The host's `DOM::Element` the effect targets.
    #[must_use]
    pub fn target_element(&self) -> Option<NonNull<c_void>> {
        self.0.borrow().target_element()
    }

    /// The serialization of the target pseudo-selector of the effect, if it has one.
    #[must_use]
    pub fn pseudo_element_serialization(&self) -> Option<ak::Utf16String> {
        self.0
            .borrow()
            .pseudo_element()
            .map(|pseudo_element| ak::Utf16String::from_utf16(&pseudo_element.serialization.to_utf16()))
    }

    /// Sets the target pseudo-selector of the effect.
    pub fn set_pseudo_element(&self, pseudo_element: Option<&ParsedPseudoElement>) {
        match pseudo_element {
            Some(pseudo_element) => self.0.borrow_mut().set_pseudo_element(
                pseudo_element.kind,
                RetainedUtf16FlyString::from_utf16(&pseudo_element.serialization.to_utf16()),
            ),
            None => self
                .0
                .borrow_mut()
                .set_pseudo_element(0, RetainedUtf16FlyString::none()),
        }
    }

    /// Sets the target pseudo-selector of the effect to that of `source`.
    pub fn copy_pseudo_element_from(&self, source: &Self) {
        let (kind, serialization) = {
            let source = source.0.borrow();
            match source.pseudo_element() {
                Some(pseudo_element) => (pseudo_element.kind, pseudo_element.serialization.clone()),
                None => (0, RetainedUtf16FlyString::none()),
            }
        };
        self.0.borrow_mut().set_pseudo_element(kind, serialization);
    }

    #[must_use]
    pub fn composite(&self) -> FfiCompositeOperation {
        self.0.borrow().composite()
    }

    pub fn set_composite(&self, composite: FfiCompositeOperation) {
        self.0.borrow_mut().set_composite(composite);
    }

    /// Whether the effect's keyframes come from CSS: from a `@keyframes` rule, or a CSS transition.
    #[must_use]
    pub fn keyframes_from_css(&self) -> bool {
        self.0.borrow().keyframes_from_css()
    }

    /// Notes that script is about to set the effect's keyframes, which keeps those it replaces of CSS from coming back.
    pub fn note_keyframes_set_by_script(&self) {
        self.0.borrow_mut().note_keyframes_set_by_script();
    }

    /// The keyframes of CSS the effect runs as `getKeyframes()` returns them.
    #[must_use]
    pub fn computed_css_keyframes(&self) -> Vec<CssKeyframeValues> {
        let effect = super::keyframe_reflection::ReflectedEffect::of(&self.0.borrow());
        super::keyframe_reflection::computed_css_keyframes(&effect)
            .into_iter()
            .map(Into::into)
            .collect()
    }

    /// The keyframes of CSS a copy of the effect takes.
    #[must_use]
    pub fn css_keyframes_of_copy(&self) -> Vec<CssKeyframeValues> {
        let effect = super::keyframe_reflection::ReflectedEffect::of(&self.0.borrow());
        super::keyframe_reflection::css_keyframes_of_copy(&effect)
            .into_iter()
            .map(Into::into)
            .collect()
    }
}

/// https://www.w3.org/TR/web-animations-1/#animation-composite-order
/// Sorts the animations in composite order, with `tree_order` comparing elements in tree order.
pub fn sort_in_composite_order(animations: &mut [AnimationRecordRef], tree_order: FfiTreeOrder) {
    let mut tree_order = host_tree_order(tree_order);
    animations.sort_by(|a, b| {
        if std::ptr::eq(a.cell(), b.cell()) {
            return std::cmp::Ordering::Equal;
        }
        composite_order(&a.cell().borrow(), &b.cell().borrow(), &mut tree_order)
    });
}

/// What script makes a timeline of.
pub enum TimelineDescription {
    /// https://www.w3.org/TR/web-animations-1/#document-timelines
    Document { origin_time: f64 },
    /// https://drafts.csswg.org/scroll-animations-1/#scrolltimeline
    /// The host's `DOM::Element` that is its source, if any, and its axis, the host's `ScrollAxis`.
    Scroll { source: Option<NonNull<c_void>>, axis: u8 },
    /// https://drafts.csswg.org/scroll-animations-1/#viewtimeline
    /// The host's `DOM::Element` that is its subject, its axis and its inset.
    View {
        subject: NonNull<c_void>,
        axis: u8,
        inset: ViewTimelineInsetValue,
    },
    /// The timeline of the Internals interface, whose time tests set.
    Internal,
}

/// The interface a timeline is an object of.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum TimelineInterfaceKind {
    Document,
    Scroll,
    View,
    Internal,
}

/// A strong reference to a timeline, as what libweb_bindings_rust holds of one.
#[derive(Clone)]
pub struct TimelineRecordRef(TimelineHandle);

impl TimelineRecordRef {
    /// A new timeline associated with the host's `DOM::Document` `document`, which keeps it alive.
    #[must_use]
    pub fn new(document: NonNull<c_void>, description: TimelineDescription) -> Self {
        let scroll = |source, axis| super::timeline::ScrollTimelineState {
            source,
            axis,
            last_max_scroll_offset: None,
            followed_scroller: None,
            view: None,
        };
        let kind = match description {
            TimelineDescription::Document { origin_time } => TimelineKind::Document { origin_time },
            TimelineDescription::Scroll { source, axis } => TimelineKind::Scroll(Box::new(scroll(
                super::timeline::ScrollTimelineSource::Element(source),
                axis,
            ))),
            TimelineDescription::View { subject, axis, inset } => {
                let mut state = scroll(
                    super::timeline::ScrollTimelineSource::Anonymous {
                        scroller: SCROLLER_NEAREST,
                        target: subject,
                        pseudo_element: None,
                    },
                    axis,
                );
                state.view = Some(super::timeline::ViewTimelineState {
                    inset: inset.0,
                    last_metrics: None,
                    subject_style_node: None,
                });
                TimelineKind::Scroll(Box::new(state))
            }
            TimelineDescription::Internal => TimelineKind::Internal {
                time_for_observation: None,
            },
        };
        Self(super::timeline_host::create_timeline(document, kind))
    }

    /// https://www.w3.org/TR/web-animations-1/#document-default-document-timeline
    /// The default document timeline of the host's `DOM::Document` `document`.
    #[must_use]
    pub fn default_of_document(document: NonNull<c_void>) -> Self {
        Self(super::timeline_host::default_document_timeline(document))
    }

    /// https://drafts.csswg.org/web-animations-1/#dom-animationtimeline-currenttime
    /// As script observes it.
    #[must_use]
    pub fn current_time_for_observation(&self) -> Option<TimeValue> {
        super::timeline_host::current_time_for_observation(&self.0)
    }

    /// https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-source
    /// The host's `DOM::Element` that is the source of a scroll timeline as script observes it, or none.
    #[must_use]
    pub fn source_element(&self) -> Option<NonNull<c_void>> {
        super::timeline_host::scroll_timeline_source(&self.0)
    }

    /// Sets the current time of the Internals interface's timeline to `time`, in milliseconds, or where
    /// `for_observation`, the time a read of it observes, keeping alive each animation it updates with `keep_alive`,
    /// which is handed the cells that keep it alive, and performing what they ask for through `perform`.
    pub fn set_internal_time(
        &self,
        time: Option<f64>,
        for_observation: bool,
        keep_alive: &dyn Fn(NonNull<c_void>),
        perform: HostEffectPerformer,
    ) {
        let keep_alive = |animation: &super::animation::AnimationCell| {
            animation.for_each_host_cell(|cell, _| keep_alive(cell));
        };
        super::timeline_host::set_internal_time(&self.0, time, for_observation, &keep_alive, perform);
    }

    /// The timeline, as the host names it.
    #[must_use]
    pub fn as_host_ptr(&self) -> *const c_void {
        std::ptr::from_ref::<super::timeline::TimelineCell>(&self.0).cast()
    }

    /// The cell of the object script sees of the timeline, while it is alive.
    #[must_use]
    pub fn object_cell(&self) -> Option<NonNull<c_void>> {
        self.0.object_cell()
    }

    /// Notes the cell of the object script sees of the timeline, or that it has none, which the host's code reaches it
    /// through.
    pub fn set_object_cell(&self, object_cell: Option<NonNull<c_void>>) {
        self.0.set_object_cell(object_cell);
    }

    /// Calls `visit` with each GC cell of the host the record names.
    pub fn for_each_host_cell(&self, mut visit: impl FnMut(NonNull<c_void>)) {
        self.0.for_each_host_cell(|cell, _| visit(cell));
    }

    /// The host's `DOM::Document` the timeline is associated with.
    #[must_use]
    pub fn document(&self) -> NonNull<c_void> {
        self.0.document()
    }

    /// Which interface the timeline is an object of.
    #[must_use]
    pub fn kind(&self) -> TimelineInterfaceKind {
        match &self.0.borrow().kind {
            TimelineKind::Document { .. } => TimelineInterfaceKind::Document,
            TimelineKind::Scroll(scroll) if scroll.view.is_some() => TimelineInterfaceKind::View,
            TimelineKind::Scroll(_) => TimelineInterfaceKind::Scroll,
            TimelineKind::Internal { .. } => TimelineInterfaceKind::Internal,
        }
    }

    /// https://drafts.csswg.org/web-animations-2/#timeline-duration
    #[must_use]
    pub fn duration(&self) -> Option<TimeValue> {
        self.0.borrow().duration()
    }

    /// The axis of a scroll timeline, the host's `ScrollAxis`.
    #[must_use]
    pub fn axis(&self) -> u8 {
        self.0.borrow().scroll().map_or(0, |scroll| scroll.axis)
    }

    /// The host's `DOM::Element` that is the subject of a view timeline.
    #[must_use]
    pub fn subject(&self) -> Option<NonNull<c_void>> {
        let record = self.0.borrow();
        let scroll = record.scroll()?;
        scroll.view.as_ref()?;
        let source: FfiScrollTimelineSource = scroll.source.into();
        NonNull::new(source.target)
    }

    /// The scroll offsets at the start and at the end of the cover range of a view timeline as its progress was last
    /// calculated, in CSS pixels: none while it was inactive.
    #[must_use]
    pub fn cover_range(&self) -> Option<(f64, f64)> {
        let record = self.0.borrow();
        let metrics = record.scroll()?.view.as_ref()?.last_metrics?;
        Some((metrics.cover_start(), metrics.cover_end()))
    }
}

/// One side of the view progress visibility range of a view timeline that script describes.
/// https://drafts.csswg.org/scroll-animations-1/#view-timeline-inset
#[derive(Clone)]
pub struct ViewTimelineInsetEdgeValue(ViewTimelineInsetEdge);

impl ViewTimelineInsetEdgeValue {
    /// The inset auto.
    #[must_use]
    pub fn auto() -> Self {
        Self(ViewTimelineInsetEdge {
            is_auto: true,
            px: 0.0,
            percentage: 0.0,
            calculated: RetainedStyleValueData::none(),
        })
    }
}

/// The start and the end inset of the view progress visibility range of a view timeline that script describes.
pub struct ViewTimelineInsetValue(ViewTimelineInset);

impl ViewTimelineInsetValue {
    #[must_use]
    pub fn from_edges(start: ViewTimelineInsetEdgeValue, end: ViewTimelineInsetEdgeValue) -> Self {
        Self(ViewTimelineInset {
            start: start.0,
            end: end.0,
        })
    }

    #[must_use]
    pub fn into_start(self) -> ViewTimelineInsetEdgeValue {
        ViewTimelineInsetEdgeValue(self.0.start)
    }

    /// The insets a computed `view-timeline-inset` value describes.
    #[must_use]
    pub fn from_computed_value(value: &StyleValue) -> Self {
        let inset = crate::css::style_compute::script_view_timeline_inset(value.data());
        // SAFETY: The calculations are null or style values that the value holds.
        Self(unsafe { retained_view_inset(&inset) })
    }
}

/// Whether a parsed `view-timeline-inset` value has a length in a unit that is not absolute.
#[must_use]
pub fn view_timeline_inset_has_relative_length(value: &StyleValue) -> bool {
    crate::css::style_compute::view_timeline_inset_has_relative_length(value.data())
}

/// Takes over one reference to a style value the host hands over.
///
/// # Safety
/// `value` must be a live style value whose reference the caller hands over.
#[must_use]
pub unsafe fn style_value_from_host(value: *const c_void) -> StyleValue {
    // SAFETY: Guaranteed by the caller.
    unsafe { RetainedStyleValueData::from_retained_pointer(value.cast()) }
}

/// The style value, as the host names it, which keeps its own reference.
#[must_use]
pub fn style_value_as_host_ptr(value: &StyleValue) -> *const c_void {
    value.pointer().cast()
}

/// An exception a procedure of an animation throws.
pub enum AnimationException {
    /// An "InvalidStateError" DOMException.
    InvalidState(&'static str),
    TypeError(&'static str),
}

impl From<AnimationError> for AnimationException {
    fn from(error: AnimationError) -> Self {
        match error {
            AnimationError::InvalidState(message) => Self::InvalidState(message),
            AnimationError::TypeError(message) => Self::TypeError(message),
        }
    }
}

/// A procedure that script runs on an animation through the Animation interface.
#[derive(Clone, Copy)]
pub enum AnimationProcedure {
    /// https://www.w3.org/TR/web-animations-1/#dom-animation-cancel
    Cancel,
    /// https://www.w3.org/TR/web-animations-1/#dom-animation-finish
    Finish,
    /// https://www.w3.org/TR/web-animations-1/#dom-animation-play
    Play,
    /// https://www.w3.org/TR/web-animations-1/#dom-animation-pause
    Pause,
    /// https://www.w3.org/TR/web-animations-1/#dom-animation-reverse
    Reverse,
    /// https://www.w3.org/TR/web-animations-1/#dom-animation-updateplaybackrate
    UpdatePlaybackRate(f64),
    /// https://www.w3.org/TR/web-animations-1/#dom-animation-playbackrate
    SetPlaybackRate(f64),
    /// https://www.w3.org/TR/web-animations-1/#dom-animation-persist
    Persist,
}

impl AnimationRecordRef {
    /// Runs `procedure` on the animation, and then has the host perform what it asked for through `perform`.
    pub fn run(&self, procedure: AnimationProcedure, perform: HostEffectPerformer) -> Result<(), AnimationException> {
        run_procedure_on(self.cell(), perform, |procedures| match procedure {
            AnimationProcedure::Cancel => {
                procedures.cancel(invalidate(true));
                Ok(())
            }
            AnimationProcedure::Finish => procedures.finish_an_animation(),
            AnimationProcedure::Play => procedures.play(invalidate(true)),
            AnimationProcedure::Pause => procedures.pause(),
            AnimationProcedure::Reverse => procedures.reverse(),
            AnimationProcedure::UpdatePlaybackRate(playback_rate) => procedures.update_playback_rate(playback_rate),
            AnimationProcedure::SetPlaybackRate(playback_rate) => procedures.set_playback_rate(playback_rate),
            AnimationProcedure::Persist => {
                // Sets this animation’s replace state to persisted.
                procedures.set_replace_state(AnimationReplaceState::Persisted);
                Ok(())
            }
        })
        .map_err(AnimationException::from)
    }

    /// Whether getAnimations() reports the animation: whether it is relevant, on its timeline, if it has one, with its
    /// associated effect, if any.
    /// https://www.w3.org/TR/web-animations-1/#animation-relevant
    #[must_use]
    pub fn is_exposed_relevant(&self) -> bool {
        let animation = self.cell().borrow();
        // AD-HOC: SVG animation elements drive their animations with their own timing model, which script must not
        //         be able to play, pause or cancel. Like other engines, do not expose them.
        if matches!(animation.kind(), AnimationKind::Smil { .. }) {
            return false;
        }
        let timeline = animation.timeline().map(|timeline| timeline.sample());
        let timeline = timeline.as_ref();
        let effect_timing = animation.effect().map(|effect| effect.borrow().timing());
        // NB: The local time of the associated effect is the current time of the animation.
        let local_time = effect_timing
            .is_some()
            .then(|| {
                super::timing::current_time_at(
                    animation.playback(),
                    timeline.and_then(|timeline| timeline.current_time.get()),
                )
            })
            .flatten();
        super::timing::is_relevant(animation.playback(), timeline, effect_timing.as_ref(), local_time)
    }

    /// Whether the animation is idle. A cancel of one that is not settles its promises and creates its cancel event.
    #[must_use]
    pub fn is_idle(&self) -> bool {
        self.cell().borrow().play_state() == AnimationPlayState::Idle
    }

    /// https://www.w3.org/TR/web-animations-1/#dom-animation-id
    #[must_use]
    pub fn dom_id(&self) -> ak::Utf16String {
        ak::Utf16String::from_utf16(&self.cell().borrow().dom_id.to_utf16())
    }

    pub fn set_dom_id(&self, id: &ak::Utf16String) {
        self.cell().borrow_mut().dom_id = RetainedUtf16FlyString::from_utf16(&id.to_utf16());
    }

    /// https://www.w3.org/TR/web-animations-1/#playback-rate
    #[must_use]
    pub fn playback_rate(&self) -> f64 {
        self.cell().borrow().playback().playback_rate
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-replace-state
    #[must_use]
    pub fn replace_state(&self) -> AnimationReplaceState {
        self.cell().borrow().playback().replace_state
    }

    /// https://www.w3.org/TR/web-animations-1/#play-states
    /// At `current_time`, the current time of the animation.
    #[must_use]
    pub fn play_state_at(&self, current_time: Option<TimeValue>) -> AnimationPlayState {
        self.cell().borrow().play_state_at(current_time)
    }

    /// https://drafts.csswg.org/css-animations-2/#dom-cssanimation-animationname
    #[must_use]
    pub fn css_animation_name(&self) -> ak::Utf16String {
        ak::Utf16String::from_utf16(&self.cell().borrow().css_animation().name.to_utf16())
    }

    /// https://drafts.csswg.org/css-transitions-2/#dom-csstransition-transitionproperty
    #[must_use]
    pub fn css_transition_property(&self) -> ak::Utf16String {
        ak::Utf16String::from_utf16(&self.cell().borrow().css_transition().property_name())
    }

    /// https://www.w3.org/TR/web-animations-1/#dom-animation-pending
    #[must_use]
    pub fn pending(&self) -> bool {
        let record = self.cell().borrow();
        let playback = record.playback();
        playback.has_pending_play_task || playback.has_pending_pause_task
    }
}

/// What kind of animation an animation is, which picks the interface of the object script sees of it.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum AnimationInterfaceKind {
    /// An animation script created, an Animation.
    Script,
    /// A CSSAnimation.
    CssAnimation,
    /// A CSSTransition.
    CssTransition,
    /// The animation of an SVG animation element, which script never sees.
    Smil,
}

/// The kind of a GC cell of the host an animation record names.
pub use super::animation::HostCellKind;

/// One edge of an attachment range: the host's `TimelineRangeName` it is measured from, a length plus a percentage of
/// the range, and a calculation only the size of the range resolves, if any.
/// https://drafts.csswg.org/scroll-animations-1/#animation-range-start
#[derive(Clone)]
pub struct AttachmentRangeEdgeValue {
    pub name: u8,
    pub px: f64,
    pub percentage: f64,
    pub calculated: Option<StyleValue>,
}

impl AttachmentRangeEdgeValue {
    /// The edge `normal`, which is the start of the entire timeline, or its end where `is_end`.
    #[must_use]
    pub fn normal(is_end: bool) -> Self {
        Self {
            name: 0,
            px: 0.0,
            percentage: if is_end { 100.0 } else { 0.0 },
            calculated: None,
        }
    }

    /// Whether the edge is `normal`, the start of the entire timeline, or its end where `is_end`.
    #[must_use]
    pub fn is_normal(&self, is_end: bool) -> bool {
        self.name == 0
            && self.px == 0.0
            && self.percentage == if is_end { 100.0 } else { 0.0 }
            && self.calculated.is_none()
    }
}

/// The edge of the computed value of `animation-range-start`, or of `animation-range-end` where `is_end`, that `value`
/// is, absolutized, if it is one.
#[must_use]
pub fn attachment_range_edge_from_computed_value(value: &StyleValue, is_end: bool) -> Option<AttachmentRangeEdgeValue> {
    let mut edge = crate::css::style_compute::FfiAnimationRangeEdge {
        name: 0,
        offset_px: 0.0,
        offset_percentage: 0.0,
        offset_calculated: std::ptr::null(),
    };
    if !crate::css::style_compute::rust_animation_range_edge(value.data(), is_end, &mut edge) {
        return None;
    }
    // SAFETY: The calculation is null or one `value` holds, which this takes a reference of its own to.
    let calculated = unsafe { retain_style_value(edge.offset_calculated.cast()) };
    Some(AttachmentRangeEdgeValue {
        name: edge.name,
        px: edge.offset_px,
        percentage: edge.offset_percentage,
        calculated: (!calculated.is_null())
            // SAFETY: The reference was just taken.
            .then(|| unsafe { RetainedStyleValueData::from_retained_pointer(calculated) }),
    })
}

impl AnimationRecordRef {
    /// A new idle animation script creates, with no associated effect and no timeline.
    #[must_use]
    pub fn new_script_animation() -> Self {
        Self::from_handle(AnimationHandle::new(AnimationKind::Script))
    }

    /// Which interface the object script sees of the animation is an object of.
    #[must_use]
    pub fn kind(&self) -> AnimationInterfaceKind {
        match self.cell().borrow().kind() {
            AnimationKind::Script => AnimationInterfaceKind::Script,
            AnimationKind::CssAnimation(_) => AnimationInterfaceKind::CssAnimation,
            AnimationKind::CssTransition(_) => AnimationInterfaceKind::CssTransition,
            AnimationKind::Smil { .. } => AnimationInterfaceKind::Smil,
        }
    }

    /// The cell of the object script sees of the animation, while it has one.
    #[must_use]
    pub fn object_cell(&self) -> Option<NonNull<c_void>> {
        self.cell().object_cell()
    }

    /// Notes the cell of the object script sees of the animation, or that it has none.
    pub fn set_object_cell(&self, object_cell: Option<NonNull<c_void>>) {
        self.cell().set_object_cell(object_cell);
    }

    /// Notes that the host finalized the object script sees of the animation. Whatever holds the record and is alive
    /// to the host keeps that object alive, so the animation is dead to the host now, though the record may outlive
    /// the object until what holds it is swept, after the elements it names may be gone. It no longer names its owning
    /// element then, so that no set of animations finds it alive.
    pub fn did_finalize_object(&self) {
        self.cell().set_object_cell(None);
        self.cell().borrow_mut().set_owning_element(None);
    }

    /// Calls `visit` with each GC cell of the host that keeps alive the elements the record names.
    pub fn for_each_host_cell(&self, visit: impl FnMut(NonNull<c_void>, HostCellKind)) {
        self.cell().for_each_host_cell(visit);
    }

    /// Calls `visit` with each GC cell of the host the record names, which the object of the animation keeps alive.
    pub fn for_each_named_cell(&self, visit: impl FnMut(NonNull<c_void>, HostCellKind)) {
        self.cell().for_each_named_cell(visit);
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-associated-effect
    #[must_use]
    pub fn effect(&self) -> Option<EffectRecordRef> {
        self.cell().borrow().effect().cloned().map(EffectRecordRef)
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-timeline
    #[must_use]
    pub fn timeline(&self) -> Option<TimelineRecordRef> {
        self.cell().borrow().timeline().cloned().map(TimelineRecordRef)
    }

    /// https://www.w3.org/TR/css-animations-2/#owning-element-section
    /// The host's `DOM::Element`, and its `PseudoElement` where the owner is one of its pseudo-elements.
    #[must_use]
    pub fn owning_element(&self) -> Option<(NonNull<c_void>, Option<u8>)> {
        self.cell()
            .borrow()
            .owning_element()
            .map(|owner| (owner.element, owner.pseudo_element))
    }

    /// The host's `SVGAnimationElement` of the animation of an SVG animation element.
    #[must_use]
    pub fn smil_animation_element(&self) -> Option<NonNull<c_void>> {
        match self.cell().borrow().kind() {
            AnimationKind::Smil { animation_element, .. } => Some(*animation_element),
            _ => None,
        }
    }

    /// The start time, hold time, playback rate, pending playback rate, pending tasks, whether the animation was last
    /// found finished, and the replace state.
    #[must_use]
    pub fn playback(&self) -> super::timing::AnimationPlayback {
        *self.cell().borrow().playback()
    }

    /// Whether the timeline of the animation is a progress-based timeline.
    #[must_use]
    pub fn has_progress_based_timeline(&self) -> bool {
        self.cell()
            .borrow()
            .timeline()
            .is_some_and(|timeline| timeline.borrow().is_progress_based())
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-current-time
    /// With its timeline at `timeline_time`, which is unresolved where the animation has no timeline.
    #[must_use]
    pub fn current_time_at(&self, timeline_time: Option<TimeValue>) -> Option<TimeValue> {
        let record = self.cell().borrow();
        let timeline_time = record.timeline().and(timeline_time);
        super::timing::current_time_at(record.playback(), timeline_time)
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-set-the-associated-effect-of-an-animation
    pub fn set_effect(&self, effect: Option<&EffectRecordRef>, perform: HostEffectPerformer) {
        let effect = effect.map(|effect| effect.0.clone());
        run_procedure(self.cell(), perform, |procedures| {
            procedures.set_the_associated_effect(effect.as_ref(), invalidate(true));
        });
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-set-the-timeline-of-an-animation
    pub fn set_timeline(&self, timeline: Option<&TimelineRecordRef>, perform: HostEffectPerformer) {
        let timeline = timeline.map(|timeline| timeline.0.clone());
        run_procedure(self.cell(), perform, |procedures| procedures.set_timeline(timeline));
    }

    /// https://www.w3.org/TR/web-animations-1/#set-the-start-time
    /// To the validated `start_time`.
    pub fn set_start_time(&self, start_time: Option<TimeValue>, perform: HostEffectPerformer) {
        run_procedure(self.cell(), perform, |procedures| {
            procedures.set_start_time_for_bindings(start_time);
        });
    }

    /// https://www.w3.org/TR/web-animations-1/#animation-set-the-current-time
    /// To the validated `seek_time`.
    pub fn set_current_time(
        &self,
        seek_time: Option<TimeValue>,
        perform: HostEffectPerformer,
    ) -> Result<(), AnimationException> {
        run_procedure_on(self.cell(), perform, |procedures| {
            procedures.set_current_time_for_bindings(seek_time)
        })
        .map_err(AnimationException::from)
    }

    /// https://www.w3.org/TR/web-animations-1/#play-an-animation
    /// With the auto-rewind flag set to true.
    pub fn play_an_animation(&self, perform: HostEffectPerformer) -> Result<(), AnimationException> {
        run_procedure_on(self.cell(), perform, |procedures| {
            procedures.play_an_animation(super::procedures::AutoRewind::Yes, invalidate(true))
        })
        .map_err(AnimationException::from)
    }

    /// https://www.w3.org/TR/web-animations-1/#update-an-animations-finished-state
    /// With the did seek flag false and the synchronously notify flag true, where `observed_current_time`, if resolved,
    /// is the unconstrained current time.
    pub fn update_finished_state(
        &self,
        should_invalidate: bool,
        observed_current_time: Option<TimeValue>,
        perform: HostEffectPerformer,
    ) {
        run_procedure(self.cell(), perform, |procedures| {
            procedures.update_finished_state(
                super::procedures::DidSeek::No,
                super::procedures::SynchronouslyNotify::Yes,
                invalidate(should_invalidate),
                observed_current_time,
            );
        });
    }

    /// Notes `task`, the host's identity of the task it queued to run the finish notification steps of the animation,
    /// or to dispatch its finish event.
    pub fn set_finish_notification_task(&self, task: u64) {
        super::ffi::animations_animation_set_finish_notification_task(self.cell(), task);
    }

    /// https://drafts.csswg.org/scroll-animations-1/#animation-attachment-range
    /// The start and the end edge.
    #[must_use]
    pub fn range(&self) -> [AttachmentRangeEdgeValue; 2] {
        self.cell().borrow().range().map(|edge| AttachmentRangeEdgeValue {
            name: edge.name,
            px: edge.offset.px,
            percentage: edge.offset.percentage,
            calculated: edge
                .offset
                .calculated
                .optional_data()
                .is_some()
                .then(|| edge.offset.calculated.clone_retained()),
        })
    }

    /// Sets the start edge of the attachment range, or the end edge where `is_end`, taking in what that changes.
    pub fn set_range_edge(&self, is_end: bool, edge: AttachmentRangeEdgeValue, perform: HostEffectPerformer) {
        let mut range = self.cell().borrow().range();
        range[usize::from(is_end)] = super::animation::AttachmentRangeEdge {
            name: edge.name,
            offset: super::animation::AttachmentRangeEdgeOffset {
                px: edge.px,
                percentage: edge.percentage,
                calculated: edge.calculated.unwrap_or_else(RetainedStyleValueData::none),
            },
        };
        if !self.cell().borrow_mut().set_range(range) {
            return;
        }
        run_procedure(self.cell(), perform, |procedures| procedures.attachment_range_changed());
    }

    /// Notes that script overrode what `flag` names of the CSS animation.
    pub fn set_css_animation_flag(&self, flag: super::animation::CssAnimationFlag) {
        self.cell().borrow_mut().css_animation_mut().set_flag(flag);
    }

    /// The bytes of the allocation every handle shares, and those the record holds beyond it.
    #[must_use]
    pub fn memory_size(&self) -> usize {
        self.cell().memory_size()
    }
}

impl EffectRecordRef {
    /// The cell of the object script sees of the effect, while it has one.
    #[must_use]
    pub fn object_cell(&self) -> Option<NonNull<c_void>> {
        self.0.object_cell()
    }
}

impl TimelineRecordRef {
    /// The current time of the timeline, or none where it is inactive.
    #[must_use]
    pub fn current_time(&self) -> Option<TimeValue> {
        self.0.sample().current_time.get()
    }

    /// The current time style sampling reads in place of the timeline's own, if any.
    #[must_use]
    pub fn style_sampling_override(&self) -> Option<Option<TimeValue>> {
        self.0.borrow().style_sampling_override()
    }

    pub fn set_style_sampling_override(&self, current_time: Option<Option<TimeValue>>) {
        self.0.borrow_mut().set_style_sampling_override(current_time);
    }

    /// https://www.w3.org/TR/web-animations-1/#timeline-time-to-origin-relative-time
    /// Where the timeline can convert one, which only a document timeline can.
    #[must_use]
    pub fn convert_a_timeline_time_to_an_origin_relative_time(&self, timeline_time: TimeValue) -> Option<f64> {
        if !matches!(self.0.borrow().kind, TimelineKind::Document { .. }) {
            return None;
        }
        self.0
            .convert_a_timeline_time_to_an_origin_relative_time(Some(timeline_time))
    }
}

pub use super::animation::CssAnimationFlag;
pub use super::ffi::FfiHostEffectKind as HostEffectKind;
pub use super::host_effects::PlaybackEventType;

/// The time a calculation of the numeric type gives: milliseconds of a number or a time, or a percentage, where it gives
/// one of those.
#[must_use]
pub fn time_value_of_calculation(
    calculation: Arc<crate::css::typed_om::CalcNode>,
    numeric_type: crate::css::typed_om::NumericType,
) -> Option<TimeValue> {
    use super::timing::TimeValueType;
    use crate::css::calc::{CanonicalNumericType, resolve_calculated_canonically};
    let calculated = crate::css::typed_om::calculated_value(
        crate::css::property_metadata::property_id::CUSTOM,
        calculation,
        numeric_type,
    );
    let (canonical_type, value) = resolve_calculated_canonically(calculated.data(), None)?;
    Some(match canonical_type {
        CanonicalNumericType::Number => TimeValue {
            r#type: TimeValueType::Milliseconds,
            value,
        },
        // NB: The canonical unit of a time is the second.
        CanonicalNumericType::Time => TimeValue {
            r#type: TimeValueType::Milliseconds,
            value: value * 1000.0,
        },
        CanonicalNumericType::Percentage => TimeValue {
            r#type: TimeValueType::Percentage,
            value,
        },
        _ => return None,
    })
}

/// Whether the numeric type matches a time.
#[must_use]
pub fn numeric_type_matches_time(numeric_type: &crate::css::typed_om::NumericType) -> bool {
    numeric_type.matches_dimension(crate::css::typed_om::BASE_TYPE_TIME, None)
}

/// https://drafts.csswg.org/scroll-animations-1/#typedef-timeline-range-name
/// The name of the host's `TimelineRangeName`, or none for an edge measured along the entire timeline.
#[must_use]
pub fn timeline_range_name_to_string(name: u8) -> Option<&'static str> {
    Some(match name {
        0 => return None,
        1 => "cover",
        2 => "contain",
        3 => "entry",
        4 => "exit",
        5 => "entry-crossing",
        6 => "exit-crossing",
        7 => "scroll",
        _ => unreachable!("the host names a timeline range it knows"),
    })
}

pub use super::css_events::FfiCssEventType as CssEventType;
pub use super::pending_animation_events::{
    FfiPendingAnimationEvent as PendingAnimationEvent, PendingAnimationEventKind,
};

/// https://www.w3.org/TR/web-animations-1/#update-animations-and-send-events
/// Takes the events of the pending animation event queue of a document, whose animations the host's
/// `DocumentAnimations` `animations` are, in the order to dispatch them in, comparing elements in tree order with
/// `tree_order`, each with the animation it is of.
///
/// # Safety
/// `animations` must be what a live document knows of animations, which nothing else reaches for the call.
pub unsafe fn take_pending_animation_events_to_dispatch(
    animations: NonNull<c_void>,
    tree_order: FfiTreeOrder,
) -> Vec<(AnimationRecordRef, PendingAnimationEvent)> {
    // SAFETY: Guaranteed by the caller.
    let animations = unsafe {
        animations
            .cast::<super::document_animations::DocumentAnimations>()
            .as_mut()
    };
    // NB: Steps 4 to 6 take and sort the events.
    animations
        .pending_events
        .take_events_to_dispatch(&mut host_tree_order(tree_order))
        .into_iter()
        .map(|event| (AnimationRecordRef::from_handle(event.animation), event.event))
        .collect()
}

/// The name of the host's `CSS::PseudoElement` `kind`, without its colons.
pub fn pseudo_element_name(kind: u8) -> &'static str {
    crate::css::selector::pseudo_element_from_ffi(kind).name()
}
