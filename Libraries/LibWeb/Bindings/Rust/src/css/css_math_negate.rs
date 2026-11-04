/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSMathNegate, https://drafts.css-houdini.org/css-typed-om-1/#cssmathnegate

use std::sync::Arc;

use libjs_rust::api::{Trace, Vm};
use libweb_rust::typed_om::{CalcNode, SumValue};

use super::css_math_value::*;
use super::css_numeric_value::{AnyCSSNumericValue, CSSNumberish, rectify_a_numberish_value};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSMathNegate {
    math_value: CSSMathValue,
    value: AnyCSSNumericValue,
}

impl Wrappable for CSSMathNegate {}

impl Inherits<CSSMathValue> for CSSMathNegate {
    fn parent(&self) -> &CSSMathValue {
        &self.math_value
    }
}

impl CSSMathNegate {
    pub(crate) fn create(vm: &Vm, value: AnyCSSNumericValue) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            math_value: CSSMathValue::new(CSSMathOperator::Negate, value.numeric_type()),
            value,
        });
        AnyCSSNumericValue::CSSMathNegate(object).finish(vm);
        object
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathnegate-cssmathnegate
    pub(crate) fn construct(cx: &mut CallContext, arg: CSSNumberish) -> WebIdlResult<Ref<Self>> {
        // The CSSMathNegate(arg) constructor must, when called, perform the following steps:
        // 1. Replace arg with the result of rectifying a numberish value for arg.
        let vm = cx.vm();
        let converted_value = rectify_a_numberish_value(vm, &arg, None);

        // 2. Return a new CSSMathNegate whose value internal slot is set to arg.
        Ok(Self::create(vm, converted_value))
    }

    pub(crate) fn math_value(&self) -> &CSSMathValue {
        &self.math_value
    }

    pub(crate) fn inner_value(&self) -> AnyCSSNumericValue {
        self.value
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathnegate-value
    pub(crate) fn value(&self, _: &mut CallContext) -> WebIdlResult<AnyCSSNumericValue> {
        Ok(self.value)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmathvalue
    pub(crate) fn serialize_math_value(&self, s: &mut String, nested: bool, parenless: bool) {
        // NB: Only steps 1 and 4 apply here.
        // 1. Let s initially be the empty string.

        // 4. Otherwise, if this is a CSSMathNegate:
        // 1. If paren-less is true, continue to the next step; otherwise, if nested is true, append "(" to s;
        //    otherwise, append "calc(" to s.
        serialize_opening(s, nested, parenless);

        // 2. Append "-" to s.
        s.push('-');

        // 3. Serialize this’s value internal slot with nested set to true, and append the result to s.
        self.value.serialize(s, NESTED);

        // 4. If paren-less is false, append ")" to s,
        if !parenless {
            s.push(')');
        }

        // 5. Return s.
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-a-sum-value
    pub(crate) fn create_a_sum_value(&self) -> Option<SumValue> {
        // 1. Let values be the result of creating a sum value from this’s value internal slot.
        // 2. If values is failure, return failure.
        let mut values = self.value.create_a_sum_value()?;

        // 3. Negate the value of each item of values.
        for value in &mut values {
            value.value = -value.value;
        }

        // 4. Return values.
        Some(values)
    }

    pub(crate) fn create_calculation_node(&self) -> WebIdlResult<Arc<CalcNode>> {
        Ok(Arc::new(CalcNode::Negate(self.value.create_calculation_node()?)))
    }
}
