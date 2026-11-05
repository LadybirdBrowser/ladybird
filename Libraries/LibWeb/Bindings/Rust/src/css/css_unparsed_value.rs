/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSUnparsedValue, https://drafts.css-houdini.org/css-typed-om-1/#cssunparsedvalue

use std::collections::HashSet;

use ak::Utf16String;
use libjs_rust::api::{GcRefCell, MarkedVec, Trace, Vm};
use libweb_rust::typed_om::{RetainedStyleValue, unresolved_style_value};

use super::css_rule::RuleText;
use super::css_style_value::{AnyCSSStyleValue, CSSStyleValue};
use super::css_variable_reference_value::CSSVariableReferenceValue;
use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

/// The union (USVString or CSSVariableReferenceValue) of CSS/CSSUnparsedValue.idl.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum CSSUnparsedSegment {
    USVString(#[gc(untraced)] UsvString),
    CSSVariableReferenceValue(Ref<CSSVariableReferenceValue>),
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSUnparsedValue {
    style_value: CSSStyleValue,
    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunparsedvalue-tokens-slot
    tokens: GcRefCell<Vec<CSSUnparsedSegment>>,
}

impl Wrappable for CSSUnparsedValue {}

impl Inherits<CSSStyleValue> for CSSUnparsedValue {
    fn parent(&self) -> &CSSStyleValue {
        &self.style_value
    }
}

impl CSSUnparsedValue {
    pub(crate) fn create(vm: &Vm, members: &MarkedVec<'_, CSSUnparsedSegment>) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            style_value: CSSStyleValue::empty(),
            tokens: GcRefCell::new(members.to_vec()),
        });
        object
            .style_value
            .set_object(vm, AnyCSSStyleValue::CSSUnparsedValue(object));
        object
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunparsedvalue-cssunparsedvalue
    pub(crate) fn construct(
        cx: &mut CallContext,
        members: MarkedVec<'_, CSSUnparsedSegment>,
    ) -> WebIdlResult<Ref<Self>> {
        // AD-HOC: There is no spec for this, see https://github.com/w3c/css-houdini-drafts/issues/1146
        Ok(Self::create(cx.vm(), &members))
    }

    pub(crate) fn style_value(&self) -> &CSSStyleValue {
        &self.style_value
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunparsedvalue-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        // The length attribute returns the size of the [[tokens]] internal slot.
        Ok(u32::try_from(self.tokens.borrow().len()).expect("the value has fewer tokens than an index can name"))
    }

    pub(crate) fn item(&self, _: &mut CallContext, index: u32) -> WebIdlResult<CSSUnparsedSegment> {
        Ok(self.tokens.borrow()[index as usize].clone())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#ref-for-dfn-set-the-value-of-an-existing-indexed-property
    pub(crate) fn set_value_of_existing_indexed_property(
        &self,
        cx: &mut CallContext,
        n: u32,
        new_value: CSSUnparsedSegment,
    ) -> WebIdlResult<()> {
        // To set the value of an existing indexed property of a CSSUnparsedValue this, an index n, and a value new
        // value, let tokens be this’s [[tokens]] internal slot, and set tokens[n] to new value.
        self.tokens.borrow_mut(cx.vm())[n as usize] = new_value;
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#ref-for-dfn-set-the-value-of-a-new-indexed-property
    pub(crate) fn set_value_of_new_indexed_property(
        &self,
        cx: &mut CallContext,
        n: u32,
        new_value: CSSUnparsedSegment,
    ) -> WebIdlResult<()> {
        // To set the value of a new indexed property of a CSSUnparsedValue this, an index n, and a value new value, let
        // tokens be this’s [[tokens]] internal slot. If n is not equal to the size of tokens, throw a RangeError.
        // Otherwise, append new value to tokens.
        let mut tokens = self.tokens.borrow_mut(cx.vm());
        if n as usize != tokens.len() {
            return Err(Exception::range_error("Index out of range"));
        }
        tokens.push(new_value);
        Ok(())
    }

    fn contains_unparsed_value(&self, needle: &Self, visited: &mut HashSet<*const Self>) -> bool {
        // NB: A value can contain itself without containing the needle, so each value is only scanned once.
        if !visited.insert(core::ptr::from_ref(self)) {
            return false;
        }
        self.tokens.borrow().iter().any(|segment| {
            let CSSUnparsedSegment::CSSVariableReferenceValue(variable_reference) = segment else {
                return false;
            };
            let Some(fallback) = variable_reference.raw_fallback() else {
                return false;
            };
            let fallback: &Self = &fallback;
            core::ptr::eq(fallback, needle) || fallback.contains_unparsed_value(needle, visited)
        })
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssunparsedvalue
    pub(crate) fn serialize(&self) -> Utf16String {
        // AD-HOC: It's possible for one of the tokens to contain this in its fallback slot, or a similar situation with
        //         more levels of nesting. To avoid crashing, do a scan for that first and return the empty string.
        // Spec issue: https://github.com/w3c/css-houdini-drafts/issues/1158
        if self.contains_unparsed_value(self, &mut HashSet::new()) {
            return Utf16String::default();
        }

        // To serialize a CSSUnparsedValue this:
        // 1. Let s initially be the empty string.
        let mut s = RuleText::default();

        // 2. For each item in this’s [[tokens]] internal slot:
        for item in self.tokens.borrow().iter() {
            // FIXME: In order to match the expected test behaviour, this should insert comments, with the same rules as
            //        serialize_a_series_of_component_values(). See https://github.com/w3c/css-houdini-drafts/issues/1148
            match item {
                // 1. If item is a USVString, append it to s.
                CSSUnparsedSegment::USVString(string) => s.push_units(&string.to_utf16()),
                // 2. Otherwise, item is a CSSVariableReferenceValue. Serialize it, then append the result to s.
                CSSUnparsedSegment::CSSVariableReferenceValue(variable) => variable.serialize(&mut s),
            }
        }

        // 3. Return s.
        s.finish()
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
    pub(crate) fn create_an_internal_representation(&self) -> WebIdlResult<RetainedStyleValue> {
        // If value is a CSSStyleValue subclass,
        //     If value does not match the grammar of a list-valued property iteration of property, throw a TypeError.
        //
        //     If any component of property’s CSS grammar has a limited numeric range, and the corresponding part of
        //     value is a CSSUnitValue that is outside of that range, replace that value with the result of wrapping it
        //     in a fresh CSSMathSum whose values internal slot contains only that part of value.
        //
        //     Return the value.

        // https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue-match-a-grammar
        // A CSSUnparsedValue matches any grammar.

        // NB: CSSUnparsedValue stores a list of strings, each of which may contain any number of tokens. So the
        //     simplest way to convert it to ComponentValues is to serialize and then parse it.
        unresolved_style_value(&self.serialize())
            .ok_or_else(|| Exception::type_error("Invalid arbitrary substitution function syntax"))
    }
}
