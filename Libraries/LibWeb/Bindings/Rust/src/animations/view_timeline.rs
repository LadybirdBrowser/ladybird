/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ViewTimeline, https://drafts.csswg.org/scroll-animations-1/#viewtimeline

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{MarkedVec, Trace, Visitor};
use libweb_rust::web_animations::{
    PROPERTY_ID_VIEW_TIMELINE_INSET, StyleValue, TimelineDescription, TimelineRecordRef, ViewTimelineInsetEdgeValue,
    ViewTimelineInsetValue, parse_property_value, style_value_as_host_ptr, style_value_from_host,
    view_timeline_inset_has_relative_length,
};

use super::animation_timeline::{
    AnimationTimeline, did_create_timeline, element_of_wrapper, element_wrapper, finalize_timeline,
};
use super::document_timeline::document_of_realm;
use super::scroll_timeline::{ScrollAxis, ScrollTimeline};
use crate::bindings::conversions::CppInterface;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::css::css_keyword_value::CSSKeywordValue;
use crate::css::css_numeric_value::AnyCSSNumericValue;
use crate::css::css_unit_value::CSSUnitValue;

unsafe extern "C" {
    fn web_animation_absolutize_style_value(document: *mut c_void, value: *const c_void) -> *const c_void;
}

/// The union (CSSNumericValue or CSSKeywordValue or DOMString) of the members of a sequence inset.
#[derive(Clone)]
pub(crate) enum CSSNumericValueOrCSSKeywordValueOrDOMString {
    CSSNumericValue(AnyCSSNumericValue),
    CSSKeywordValue(Ref<CSSKeywordValue>),
    DOMString(Utf16String),
}

// SAFETY: Visits the cells of the variant.
unsafe impl Trace for CSSNumericValueOrCSSKeywordValueOrDOMString {
    fn trace(&self, visitor: &mut Visitor) {
        match self {
            Self::CSSNumericValue(value) => value.trace(visitor),
            Self::CSSKeywordValue(value) => visitor.visit(*value),
            Self::DOMString(_) => {}
        }
    }
}

/// The union (DOMString or sequence<(CSSNumericValue or CSSKeywordValue or DOMString)>) of ViewTimelineOptions.inset.
pub(crate) enum DOMStringOrCSSNumericValueOrCSSKeywordValueOrDOMStringSequence<'vm> {
    DOMString(Utf16String),
    Sequence(MarkedVec<'vm, CSSNumericValueOrCSSKeywordValueOrDOMString>),
}

/// The dictionary ViewTimelineOptions of Animations/ViewTimeline.idl.
pub(crate) struct ViewTimelineOptions<'vm> {
    pub(crate) subject: CppInterface,
    pub(crate) axis: ScrollAxis,
    pub(crate) inset: DOMStringOrCSSNumericValueOrCSSKeywordValueOrDOMStringSequence<'vm>,
}

// https://drafts.csswg.org/scroll-animations-1/#viewtimeline
pub(crate) struct ViewTimeline {
    scroll_timeline: ScrollTimeline,
}

// SAFETY: The timeline visits what it reaches.
unsafe impl Trace for ViewTimeline {
    fn trace(&self, visitor: &mut Visitor) {
        self.scroll_timeline.trace(visitor);
    }
}

impl Inherits<ScrollTimeline> for ViewTimeline {
    fn parent(&self) -> &ScrollTimeline {
        &self.scroll_timeline
    }
}

impl Wrappable for ViewTimeline {
    const TRACED_ON_OWNER_THREAD_ONLY: bool = true;
    const FINALIZES: bool = true;

    fn finalize(object: Ref<Self>) {
        finalize_timeline(object.scroll_timeline.timeline());
    }
}

fn invalid_inset() -> Exception {
    Exception::type_error("Invalid view timeline inset")
}

/// The inset the text parses as for the document, as a <'view-timeline-inset'> value.
fn parse_inset(document: *mut c_void, text: &Utf16String) -> WebIdlResult<ViewTimelineInsetValue> {
    let value = parse_property_value(PROPERTY_ID_VIEW_TIMELINE_INSET, text).ok_or_else(invalid_inset)?;
    // NB: Arbitrary substitution functions and CSS-wide keywords have nothing to substitute or inherit from here.
    // AD-HOC: Relative lengths are not allowed, see https://github.com/w3c/csswg-drafts/issues/13852
    if value.is_css_wide_keyword()
        || value.is_unresolved()
        || value.is_pending_substitution()
        || view_timeline_inset_has_relative_length(&value)
    {
        return Err(invalid_inset());
    }
    // SAFETY: The document and the value are live, and the host hands over a reference to the absolutized value.
    let computed: StyleValue = unsafe {
        style_value_from_host(web_animation_absolutize_style_value(
            document,
            style_value_as_host_ptr(&value),
        ))
    };
    Ok(ViewTimelineInsetValue::from_computed_value(&computed))
}

fn parse_auto(keyword: &Utf16String) -> WebIdlResult<ViewTimelineInsetEdgeValue> {
    let is_auto = keyword
        .to_utf16()
        .iter()
        .map(|&code_unit| {
            if (u16::from(b'A')..=u16::from(b'Z')).contains(&code_unit) {
                code_unit + 0x20
            } else {
                code_unit
            }
        })
        .eq("auto".encode_utf16());
    if !is_auto {
        return Err(Exception::type_error("A view timeline inset keyword must be auto"));
    }
    Ok(ViewTimelineInsetEdgeValue::auto())
}

impl ViewTimeline {
    pub(crate) fn from_timeline(timeline: AnimationTimeline) -> Self {
        Self {
            scroll_timeline: ScrollTimeline::from_timeline(timeline),
        }
    }

    pub(crate) fn scroll_timeline(&self) -> &ScrollTimeline {
        &self.scroll_timeline
    }

    fn record(&self) -> &TimelineRecordRef {
        self.scroll_timeline.timeline().record()
    }

    // https://drafts.csswg.org/scroll-animations-1/#dom-viewtimeline-viewtimeline
    pub(crate) fn construct(cx: &mut CallContext, options: ViewTimelineOptions<'_>) -> WebIdlResult<Ref<Self>> {
        let document = document_of_realm(cx.realm());

        // 1. Let timeline be the new ViewTimeline object.
        // 2. Set the subject and axis properties of timeline to the corresponding values from options.
        let subject = element_of_wrapper(options.subject);

        // 3. Set the source of timeline to the subject’s nearest ancestor scroll container element that's scrollable
        //    in the specified axis.
        // NB: The source is found from the subject whenever it is asked for.

        // 4. If a DOMString value is provided as an inset, parse it as a <'view-timeline-inset'> value; if a sequence
        //    is provided, the first value represents the start inset and the second value represents the end inset. If
        //    the sequence has only one value, it is duplicated. If it has zero values or more than two values, or if it
        //    contains a CSSKeywordValue whose value is not "auto", throw a TypeError.
        let inset = match options.inset {
            DOMStringOrCSSNumericValueOrCSSKeywordValueOrDOMStringSequence::DOMString(string) => {
                parse_inset(document.as_ptr(), &string)?
            }
            DOMStringOrCSSNumericValueOrCSSKeywordValueOrDOMStringSequence::Sequence(values) => {
                if values.is_empty() || values.len() > 2 {
                    return Err(Exception::type_error("A view timeline inset takes one or two values"));
                }
                let mut edges = Vec::with_capacity(2);
                for value in values.values().iter() {
                    edges.push(match value {
                        CSSNumericValueOrCSSKeywordValueOrDOMString::CSSNumericValue(numeric) => {
                            parse_inset(document.as_ptr(), &numeric.to_css_string())?.into_start()
                        }
                        CSSNumericValueOrCSSKeywordValueOrDOMString::CSSKeywordValue(keyword) => {
                            parse_auto(&keyword.raw_value())?
                        }
                        // INTEROP: Blink takes "auto" as a string in the sequence too.
                        CSSNumericValueOrCSSKeywordValueOrDOMString::DOMString(keyword) => parse_auto(keyword)?,
                    });
                }
                let start = edges.remove(0);
                let end = edges.pop().unwrap_or_else(|| start.clone());
                ViewTimelineInsetValue::from_edges(start, end)
            }
        };

        // These insets define the ViewTimeline’s view progress visibility range.
        let record = TimelineRecordRef::new(
            document,
            TimelineDescription::View {
                subject,
                axis: options.axis as u8,
                inset,
            },
        );
        let object = allocate(cx.vm(), |_| Self::from_timeline(AnimationTimeline::new(record)));
        did_create_timeline(cell_into_abi(object));
        Ok(object)
    }

    // https://drafts.csswg.org/scroll-animations-1/#dom-viewtimeline-subject
    pub(crate) fn subject(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        let subject = self.record().subject().expect("a view timeline has a subject");
        Ok(element_wrapper(cx, subject))
    }

    // https://drafts.csswg.org/scroll-animations-1/#dom-viewtimeline-startoffset
    pub(crate) fn start_offset(&self, cx: &mut CallContext) -> WebIdlResult<AnyCSSNumericValue> {
        // Represents the starting (0% progress) scroll position of the view progress timeline as a length offset (in
        // px) from the scroll origin.
        // Null when the timeline is inactive.
        // AD-HOC: The attribute is not nullable in the IDL, so an inactive timeline gives 0px.
        let start = self.record().cover_range().map_or(0.0, |(start, _)| start);
        Ok(AnyCSSNumericValue::CSSUnitValue(CSSUnitValue::create(
            cx.vm(),
            start,
            "px",
        )))
    }

    // https://drafts.csswg.org/scroll-animations-1/#dom-viewtimeline-endoffset
    pub(crate) fn end_offset(&self, cx: &mut CallContext) -> WebIdlResult<AnyCSSNumericValue> {
        // Represents the ending (100% progress) scroll position of the view progress timeline as a length offset (in
        // px) from the scroll origin.
        // Null when the timeline is inactive.
        // AD-HOC: The attribute is not nullable in the IDL, so an inactive timeline gives 0px.
        let end = self.record().cover_range().map_or(0.0, |(_, end)| end);
        Ok(AnyCSSNumericValue::CSSUnitValue(CSSUnitValue::create(
            cx.vm(),
            end,
            "px",
        )))
    }
}
