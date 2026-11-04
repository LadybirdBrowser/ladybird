/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSImageValue, https://drafts.css-houdini.org/css-typed-om-1/#cssimagevalue

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::typed_om::RetainedStyleValue;

use super::css_style_value::{AnyCSSStyleValue, CSSStyleValue, PerformTypeCheck};
use super::typed_om::{self, TypedOmValueType};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{Exception, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSImageValue {
    style_value: CSSStyleValue,
}

impl Wrappable for CSSImageValue {}

impl Inherits<CSSStyleValue> for CSSImageValue {
    fn parent(&self) -> &CSSStyleValue {
        &self.style_value
    }
}

impl CSSImageValue {
    pub(crate) fn create(vm: &Vm, source_value: RetainedStyleValue) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            style_value: CSSStyleValue::with_source(source_value),
        });
        object
            .style_value
            .set_object(vm, AnyCSSStyleValue::CSSImageValue(object));
        object
    }

    pub(crate) fn style_value(&self) -> &CSSStyleValue {
        &self.style_value
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
        // NB: https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue-match-a-grammar doesn't list CSSImageValue,
        //     but we should match <image>.
        if perform_type_check == PerformTypeCheck::Yes
            && !typed_om::property_accepts_type(property_id, TypedOmValueType::Image)
        {
            return Err(Exception::TypeError(Utf16String::from_utf8(&format!(
                "Property '{}' does not accept <image>",
                typed_om::to_rust_string(property_name)
            ))));
        }

        //     FIXME: If any component of property’s CSS grammar has a limited numeric range, and the corresponding part
        //            of value is a CSSUnitValue that is outside of that range, replace that value with the result of
        //            wrapping it in a fresh CSSMathSum whose values internal slot contains only that part of value.

        //     Return the value.
        Ok(self
            .style_value
            .source_value()
            .expect("a CSSImageValue has a source value")
            .clone())
    }
}
