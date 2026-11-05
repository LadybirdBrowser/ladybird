/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSVariableReferenceValue, https://drafts.css-houdini.org/css-typed-om-1/#cssvariablereferencevalue

use core::cell::RefCell;

use ak::Utf16String;
use libjs_rust::api::{GcRefCell, Trace, Vm};
use libweb_rust::typed_om::is_a_custom_property_name_string;

use super::css_rule::RuleText;
use super::css_unparsed_value::CSSUnparsedValue;
use super::typed_om;
use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSVariableReferenceValue {
    #[gc(untraced)]
    variable: RefCell<Utf16String>,
    fallback: GcRefCell<Option<Ref<CSSUnparsedValue>>>,
}

impl Wrappable for CSSVariableReferenceValue {}

fn not_a_custom_property_name(variable: &Utf16String) -> Exception {
    Exception::TypeError(Utf16String::from_utf8(&format!(
        "'{}' is not a valid CSS custom property name",
        typed_om::to_rust_string(variable)
    )))
}

impl CSSVariableReferenceValue {
    pub(crate) fn create(vm: &Vm, variable: Utf16String, fallback: Option<Ref<CSSUnparsedValue>>) -> Ref<Self> {
        allocate(vm, |_| Self {
            variable: RefCell::new(variable),
            fallback: GcRefCell::new(fallback),
        })
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssvariablereferencevalue-cssvariablereferencevalue
    pub(crate) fn construct(
        cx: &mut CallContext,
        variable: UsvString,
        fallback: Option<Ref<CSSUnparsedValue>>,
    ) -> WebIdlResult<Ref<Self>> {
        // The CSSVariableReferenceValue(variable, fallback) constructor must, when called, perform the following steps:
        // 1. If variable is not a custom property name string, throw a TypeError.
        let variable = variable.into_utf16_string();
        if !is_a_custom_property_name_string(&variable) {
            return Err(not_a_custom_property_name(&variable));
        }

        // 2. Return a new CSSVariableReferenceValue with its variable internal slot set to variable and its fallback
        //    internal slot set to fallback.
        Ok(Self::create(cx.vm(), variable, fallback))
    }

    pub(crate) fn raw_fallback(&self) -> Option<Ref<CSSUnparsedValue>> {
        *self.fallback.borrow()
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssvariablereferencevalue-variable
    pub(crate) fn variable(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The getter for the variable attribute of a CSSVariableReferenceValue this must return its variable internal
        // slot.
        Ok(self.variable.borrow().clone())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssvariablereferencevalue-variable
    pub(crate) fn set_variable(&self, _: &mut CallContext, variable: UsvString) -> WebIdlResult<()> {
        // The variable attribute of a CSSVariableReferenceValue this must, on setting a variable variable, perform the
        // following steps:
        // 1. If variable is not a custom property name string, throw a TypeError.
        let variable = variable.into_utf16_string();
        if !is_a_custom_property_name_string(&variable) {
            return Err(not_a_custom_property_name(&variable));
        }

        // 2. Otherwise, set this’s variable internal slot to variable.
        *self.variable.borrow_mut() = variable;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssvariablereferencevalue-fallback
    pub(crate) fn fallback(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<CSSUnparsedValue>>> {
        // AD-HOC: No spec algorithm, see https://github.com/w3c/css-houdini-drafts/issues/1146#issuecomment-3188550133
        Ok(self.raw_fallback())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssvariablereferencevalue
    pub(crate) fn serialize(&self, s: &mut RuleText) {
        // To serialize a CSSVariableReferenceValue this:
        // 1. Let s initially be "var(".
        s.push_str("var(");

        // 2. Append this’s variable internal slot to s.
        s.push(&self.variable.borrow());

        // 3. If this’s fallback internal slot is not null, append ", " to s, then serialize the fallback internal slot
        //    and append it to s.
        if let Some(fallback) = self.raw_fallback() {
            // AD-HOC: Tested behaviour requires we append "," without the space.
            //         https://github.com/w3c/css-houdini-drafts/issues/1148
            s.push_str(",");
            s.push(&fallback.serialize());
        }

        // 4. Append ")" to s and return s.
        s.push_str(")");
    }
}
