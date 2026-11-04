/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSKeywordValue, https://drafts.css-houdini.org/css-typed-om-1/#csskeywordvalue

use core::cell::RefCell;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::typed_om::{RetainedStyleValue, serialize_an_identifier};

use super::css_style_value::{AnyCSSStyleValue, CSSStyleValue, PerformTypeCheck};
use super::typed_om;
use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

/// The union (DOMString or CSSKeywordValue) of CSS/CSSKeywordValue.idl.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum CSSKeywordish {
    DOMString(#[gc(untraced)] Utf16String),
    CSSKeywordValue(Ref<CSSKeywordValue>),
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSKeywordValue {
    style_value: CSSStyleValue,
    #[gc(untraced)]
    value: RefCell<Utf16String>,
}

impl Wrappable for CSSKeywordValue {}

impl Inherits<CSSStyleValue> for CSSKeywordValue {
    fn parent(&self) -> &CSSStyleValue {
        &self.style_value
    }
}

impl CSSKeywordValue {
    pub(crate) fn create(vm: &Vm, value: Utf16String) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            style_value: CSSStyleValue::empty(),
            value: RefCell::new(value),
        });
        object
            .style_value
            .set_object(vm, AnyCSSStyleValue::CSSKeywordValue(object));
        object
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-csskeywordvalue-csskeywordvalue
    pub(crate) fn construct(cx: &mut CallContext, value: UsvString) -> WebIdlResult<Ref<Self>> {
        // 1. If value is an empty string, throw a TypeError.
        let value = value.into_utf16_string();
        if value.is_empty() {
            return Err(Exception::type_error(
                "Cannot create a CSSKeywordValue with an empty string as the value",
            ));
        }

        // 2. Otherwise, return a new CSSKeywordValue with its value internal slot set to value.
        Ok(Self::create(cx.vm(), value))
    }

    pub(crate) fn style_value(&self) -> &CSSStyleValue {
        &self.style_value
    }

    pub(crate) fn raw_value(&self) -> Utf16String {
        self.value.borrow().clone()
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-csskeywordvalue-value
    pub(crate) fn value(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.raw_value())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-csskeywordvalue-value
    pub(crate) fn set_value(&self, _: &mut CallContext, value: UsvString) -> WebIdlResult<()> {
        // 1. If value is an empty string, throw a TypeError.
        let value = value.into_utf16_string();
        if value.is_empty() {
            return Err(Exception::type_error(
                "Cannot set CSSKeywordValue.value to an empty string",
            ));
        }

        // 2. Otherwise, set this’s value internal slot, to value.
        *self.value.borrow_mut() = value;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#keywordvalue-serialization
    pub(crate) fn serialize(&self) -> Utf16String {
        // To serialize a CSSKeywordValue this:
        // 1. Return this’s value internal slot.
        // AD-HOC: Serialize it as an identifier. Spec issue: https://github.com/w3c/csswg-drafts/issues/12545
        serialize_an_identifier(&self.value.borrow())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
    pub(crate) fn create_an_internal_representation(
        &self,
        property_id: u16,
        property_name: &Utf16String,
        perform_type_check: PerformTypeCheck,
    ) -> WebIdlResult<RetainedStyleValue> {
        // If value is a CSSStyleValue subclass,
        //     If value does not match the grammar of a list-valued property iteration of property, throw a TypeError.
        // https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue-match-a-grammar
        // A CSSKeywordValue matches an <ident> specified in a grammar if its value internal slot matches the
        // identifier.
        // If case-folding rules are in effect normally for that <ident> (such as Auto matching the keyword auto
        // specified in the grammar for width), they apply to this comparison as well.
        //
        //     If any component of property’s CSS grammar has a limited numeric range, and the corresponding part of
        //     value is a CSSUnitValue that is outside of that range, replace that value with the result of wrapping it
        //     in a fresh CSSMathSum whose values internal slot contains only that part of value.
        // NB: Non-applicable.
        //
        //     Return the value.
        let value = self.value.borrow();
        typed_om::keyword_style_value_for_property(property_id, &value, perform_type_check).ok_or_else(|| {
            Exception::TypeError(Utf16String::from_utf8(&format!(
                "Property '{}' does not accept the keyword '{}'",
                typed_om::to_rust_string(property_name),
                typed_om::to_rust_string(&value)
            )))
        })
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#rectify-a-keywordish-value
pub(crate) fn rectify_a_keywordish_value(vm: &Vm, keywordish: &CSSKeywordish) -> Ref<CSSKeywordValue> {
    // To rectify a keywordish value val, perform the following steps:
    match keywordish {
        // 1. If val is a CSSKeywordValue, return val.
        CSSKeywordish::CSSKeywordValue(value) => *value,
        // 2. If val is a DOMString, return a new CSSKeywordValue with its value internal slot set to val.
        CSSKeywordish::DOMString(value) => CSSKeywordValue::create(vm, value.clone()),
    }
}
