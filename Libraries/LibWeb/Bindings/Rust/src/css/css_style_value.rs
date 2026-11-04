/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSStyleValue, https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue

use ak::Utf16String;
use libjs_rust::api::{GcRefCell, MarkedVec, Trace, Vm};
use libweb_rust::typed_om::RetainedStyleValue;

use super::css_image_value::CSSImageValue;
use super::css_keyword_value::CSSKeywordValue;
use super::css_math_clamp::CSSMathClamp;
use super::css_math_invert::CSSMathInvert;
use super::css_math_max::CSSMathMax;
use super::css_math_min::CSSMathMin;
use super::css_math_negate::CSSMathNegate;
use super::css_math_product::CSSMathProduct;
use super::css_math_sum::CSSMathSum;
use super::css_math_value::CSSMathValue;
use super::css_numeric_value::{AnyCSSNumericValue, CSSNumericValue};
use super::css_transform_value::CSSTransformValue;
use super::css_unit_value::CSSUnitValue;
use super::css_unparsed_value::CSSUnparsedValue;
use super::typed_om;
use crate::bindings::conversions::{ToJs, UsvString};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, cell_into_abi, wrap};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use libjs_rust::api::{ThrowCompletionOr, Value};

/// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
#[derive(Clone, Copy, Eq, PartialEq)]
pub(crate) enum PerformTypeCheck {
    No,
    Yes,
}

/// An object of CSSStyleValue or of an interface that inherits from it.
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
#[expect(clippy::enum_variant_names, reason = "the variants are named after the interfaces")]
pub(crate) enum AnyCSSStyleValue {
    CSSStyleValue(Ref<CSSStyleValue>),
    CSSImageValue(Ref<CSSImageValue>),
    CSSKeywordValue(Ref<CSSKeywordValue>),
    CSSMathClamp(Ref<CSSMathClamp>),
    CSSMathInvert(Ref<CSSMathInvert>),
    CSSMathMax(Ref<CSSMathMax>),
    CSSMathMin(Ref<CSSMathMin>),
    CSSMathNegate(Ref<CSSMathNegate>),
    CSSMathProduct(Ref<CSSMathProduct>),
    CSSMathSum(Ref<CSSMathSum>),
    CSSMathValue(Ref<CSSMathValue>),
    CSSNumericValue(Ref<CSSNumericValue>),
    CSSTransformValue(Ref<CSSTransformValue>),
    CSSUnitValue(Ref<CSSUnitValue>),
    CSSUnparsedValue(Ref<CSSUnparsedValue>),
}

impl AnyCSSStyleValue {
    /// The object as a CSSNumericValue, if it is one.
    pub(crate) fn as_numeric(self) -> Option<AnyCSSNumericValue> {
        Some(match self {
            Self::CSSMathClamp(object) => AnyCSSNumericValue::CSSMathClamp(object),
            Self::CSSMathInvert(object) => AnyCSSNumericValue::CSSMathInvert(object),
            Self::CSSMathMax(object) => AnyCSSNumericValue::CSSMathMax(object),
            Self::CSSMathMin(object) => AnyCSSNumericValue::CSSMathMin(object),
            Self::CSSMathNegate(object) => AnyCSSNumericValue::CSSMathNegate(object),
            Self::CSSMathProduct(object) => AnyCSSNumericValue::CSSMathProduct(object),
            Self::CSSMathSum(object) => AnyCSSNumericValue::CSSMathSum(object),
            Self::CSSMathValue(object) => AnyCSSNumericValue::CSSMathValue(object),
            Self::CSSNumericValue(object) => AnyCSSNumericValue::CSSNumericValue(object),
            Self::CSSUnitValue(object) => AnyCSSNumericValue::CSSUnitValue(object),
            _ => return None,
        })
    }

    /// The part of the object that all CSSStyleValues have.
    pub(crate) fn style_value(&self) -> &CSSStyleValue {
        fn part_of_math_value(value: &CSSMathValue) -> &CSSStyleValue {
            <CSSMathValue as Inherits<CSSNumericValue>>::parent(value).parent()
        }

        match self {
            Self::CSSStyleValue(object) => object,
            Self::CSSImageValue(object) => object.style_value(),
            Self::CSSKeywordValue(object) => object.style_value(),
            Self::CSSMathClamp(object) => part_of_math_value(object.math_value()),
            Self::CSSMathInvert(object) => part_of_math_value(object.math_value()),
            Self::CSSMathMax(object) => part_of_math_value(object.math_value()),
            Self::CSSMathMin(object) => part_of_math_value(object.math_value()),
            Self::CSSMathNegate(object) => part_of_math_value(object.math_value()),
            Self::CSSMathProduct(object) => part_of_math_value(object.math_value()),
            Self::CSSMathSum(object) => part_of_math_value(object.math_value()),
            Self::CSSMathValue(object) => part_of_math_value(object),
            Self::CSSNumericValue(object) => object.parent(),
            Self::CSSTransformValue(object) => object.style_value(),
            Self::CSSUnitValue(object) => <CSSUnitValue as Inherits<CSSNumericValue>>::parent(object).parent(),
            Self::CSSUnparsedValue(object) => object.style_value(),
        }
    }

    /// The cell of the object, as LibWeb's C++ code holds it.
    pub(crate) fn cell_into_abi(self) -> *mut core::ffi::c_void {
        match self {
            Self::CSSStyleValue(object) => cell_into_abi(object),
            Self::CSSImageValue(object) => cell_into_abi(object),
            Self::CSSKeywordValue(object) => cell_into_abi(object),
            Self::CSSMathClamp(object) => cell_into_abi(object),
            Self::CSSMathInvert(object) => cell_into_abi(object),
            Self::CSSMathMax(object) => cell_into_abi(object),
            Self::CSSMathMin(object) => cell_into_abi(object),
            Self::CSSMathNegate(object) => cell_into_abi(object),
            Self::CSSMathProduct(object) => cell_into_abi(object),
            Self::CSSMathSum(object) => cell_into_abi(object),
            Self::CSSMathValue(object) => cell_into_abi(object),
            Self::CSSNumericValue(object) => cell_into_abi(object),
            Self::CSSTransformValue(object) => cell_into_abi(object),
            Self::CSSUnitValue(object) => cell_into_abi(object),
            Self::CSSUnparsedValue(object) => cell_into_abi(object),
        }
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#stylevalue-serialization
    pub(crate) fn serialize(&self, vm: &Vm) -> WebIdlResult<Utf16String> {
        match self {
            Self::CSSStyleValue(object) => Ok(object.serialize_source_value()),
            // AD-HOC: The spec doesn't say how to serialize this, as it's intentionally a black box.
            //         We just rely on CSSStyleValue serializing its held StyleValue.
            Self::CSSImageValue(object) => Ok(object.style_value().serialize_source_value()),
            Self::CSSKeywordValue(object) => Ok(object.serialize()),
            Self::CSSTransformValue(object) => object.serialize(vm),
            Self::CSSUnparsedValue(object) => Ok(object.serialize()),
            _ => Ok(self.as_numeric().expect("the object is numeric").to_css_string()),
        }
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
    pub(crate) fn create_an_internal_representation(
        &self,
        vm: &Vm,
        property_id: u16,
        property_name: &Utf16String,
        perform_type_check: PerformTypeCheck,
    ) -> WebIdlResult<RetainedStyleValue> {
        match self {
            Self::CSSStyleValue(object) => object.create_an_internal_representation(),
            Self::CSSImageValue(object) => {
                object.create_an_internal_representation(property_id, property_name, perform_type_check)
            }
            Self::CSSKeywordValue(object) => {
                object.create_an_internal_representation(property_id, property_name, perform_type_check)
            }
            Self::CSSTransformValue(object) => {
                object.create_an_internal_representation(vm, property_id, perform_type_check)
            }
            Self::CSSUnparsedValue(object) => object.create_an_internal_representation(),
            Self::CSSUnitValue(object) => object.create_an_internal_representation(property_id, perform_type_check),
            _ => self
                .as_numeric()
                .expect("the object is numeric")
                .math_value_internal_representation(property_id, perform_type_check),
        }
    }
}

impl ToJs for AnyCSSStyleValue {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(match self {
            Self::CSSStyleValue(object) => Value::from_object(wrap(cx, object)),
            Self::CSSImageValue(object) => Value::from_object(wrap(cx, object)),
            Self::CSSKeywordValue(object) => Value::from_object(wrap(cx, object)),
            Self::CSSTransformValue(object) => Value::from_object(wrap(cx, object)),
            Self::CSSUnparsedValue(object) => Value::from_object(wrap(cx, object)),
            _ => return self.as_numeric().expect("the object is numeric").to_js(cx),
        })
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSStyleValue {
    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssstylevalue-associatedproperty-slot
    #[gc(untraced)]
    associated_property: Option<Utf16String>,
    /// The value this object was reified from.
    #[gc(untraced)]
    source_value: Option<RetainedStyleValue>,
    #[gc(untraced)]
    serialization_mode: u8,
    /// The object that this is the CSSStyleValue part of, by which the steps of CSSStyleValue reach the steps of the
    /// interface of the object.
    object: GcRefCell<Option<AnyCSSStyleValue>>,
}

impl Wrappable for CSSStyleValue {}

impl CSSStyleValue {
    /// The part of an object of an interface that inherits from CSSStyleValue and has no source value.
    pub(crate) fn empty() -> Self {
        Self::with_source_value(None, None, 0)
    }

    /// The part of an object of an interface that inherits from CSSStyleValue and has a source value.
    pub(crate) fn with_source(source_value: RetainedStyleValue) -> Self {
        Self::with_source_value(None, Some(source_value), 0)
    }

    fn with_source_value(
        associated_property: Option<Utf16String>,
        source_value: Option<RetainedStyleValue>,
        serialization_mode: u8,
    ) -> Self {
        Self {
            associated_property,
            source_value,
            serialization_mode,
            object: GcRefCell::new(None),
        }
    }

    pub(crate) fn create(
        vm: &Vm,
        associated_property: Utf16String,
        source_value: RetainedStyleValue,
        serialization_mode: u8,
    ) -> Ref<Self> {
        let object = allocate(vm, |_| {
            Self::with_source_value(Some(associated_property), Some(source_value), serialization_mode)
        });
        object.set_object(vm, AnyCSSStyleValue::CSSStyleValue(object));
        object
    }

    /// Tells the part which object it belongs to, which an object does as soon as it exists.
    pub(crate) fn set_object(&self, vm: &Vm, object: AnyCSSStyleValue) {
        *self.object.borrow_mut(vm) = Some(object);
    }

    pub(crate) fn object(&self) -> AnyCSSStyleValue {
        self.object.borrow().expect("an object exists from when it is created")
    }

    pub(crate) fn associated_property(&self) -> Option<&Utf16String> {
        self.associated_property.as_ref()
    }

    pub(crate) fn source_value(&self) -> Option<&RetainedStyleValue> {
        self.source_value.as_ref()
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#stylevalue-serialization
    fn serialize_source_value(&self) -> Utf16String {
        // FIXME: if the value was constructed from a USVString
        // NB: Basically, if this was constructed with "parse a CSSStyleValue", regardless of what CSSStyleValue type it
        //     is now.
        //     the serialization is the USVString from which the value was constructed.
        // otherwise, if the value was constructed using an IDL constructor
        //     the serialization is specified in the sections below.
        // NB: This is handled by the interfaces that inherit from this one.
        // FIXME: otherwise, if the value was extracted from the CSSOM
        // NB: For CSSStyleValue itself, we use the source value we were created from.
        self.source_value
            .as_ref()
            .map(|source_value| source_value.serialize(self.serialization_mode))
            .unwrap_or_default()
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
    fn create_an_internal_representation(&self) -> WebIdlResult<RetainedStyleValue> {
        // If value is a direct CSSStyleValue,
        //     Return value’s associated value.
        self.source_value
            .clone()
            .ok_or_else(|| Exception::type_error("Missing create_an_internal_representation() overload"))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#stylevalue-serialization
    pub(crate) fn to_string(&self, cx: &mut CallContext) -> WebIdlResult<Utf16String> {
        self.object().serialize(cx.vm())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssstylevalue-parse
    pub(crate) fn parse(
        cx: &mut CallContext,
        property: UsvString,
        css_text: UsvString,
    ) -> WebIdlResult<AnyCSSStyleValue> {
        // The parse(property, cssText) method, when invoked, must parse a CSSStyleValue with property property, cssText
        // cssText, and parseMultiple set to false, and return the result.
        let values = typed_om::parse_a_css_style_value(cx, property, css_text)?;
        Ok(values.get(0).expect("parsing results in at least one value"))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssstylevalue-parseall
    pub(crate) fn parse_all<'vm>(
        cx: &mut CallContext<'vm>,
        property: UsvString,
        css_text: UsvString,
    ) -> WebIdlResult<MarkedVec<'vm, AnyCSSStyleValue>> {
        // The parseAll(property, cssText) method, when invoked, must parse a CSSStyleValue with property property,
        // cssText cssText, and parseMultiple set to true, and return the result.
        typed_om::parse_a_css_style_value(cx, property, css_text)
    }
}
