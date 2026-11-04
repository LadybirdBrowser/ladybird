/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSMathSum, https://drafts.css-houdini.org/css-typed-om-1/#cssmathsum

use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{MarkedVec, Trace, Vm};
use libweb_rust::typed_om::{CalcNode, NumericType, SumValue, create_a_type_from_a_unit_map};

use super::css_math_value::*;
use super::css_numeric_array::CSSNumericArray;
use super::css_numeric_value::{AnyCSSNumericValue, CSSNumberish};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSMathSum {
    math_value: CSSMathValue,
    values: Ref<CSSNumericArray>,
}

impl Wrappable for CSSMathSum {}

impl Inherits<CSSMathValue> for CSSMathSum {
    fn parent(&self) -> &CSSMathValue {
        &self.math_value
    }
}

impl CSSMathSum {
    pub(crate) fn create(vm: &Vm, numeric_type: NumericType, values: Ref<CSSNumericArray>) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            math_value: CSSMathValue::new(CSSMathOperator::Sum, numeric_type),
            values,
        });
        AnyCSSNumericValue::CSSMathSum(object).finish(vm);
        object
    }

    pub(crate) fn add_all_types_into_math_sum(
        vm: &Vm,
        values: &MarkedVec<'_, AnyCSSNumericValue>,
    ) -> WebIdlResult<Ref<Self>> {
        let Some(numeric_type) = add_all_types(&values.to_vec()) else {
            return Err(Exception::type_error(
                "Cannot create a CSSMathSum with values of incompatible types",
            ));
        };
        let values = CSSNumericArray::create(vm, values);
        Ok(Self::create(vm, numeric_type, values))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathsum-cssmathsum
    pub(crate) fn construct(cx: &mut CallContext, args: MarkedVec<'_, CSSNumberish>) -> WebIdlResult<Ref<Self>> {
        // The CSSMathSum(...args) constructor must, when called, perform the following steps:

        // 1. Replace each item of args with the result of rectifying a numberish value for the item.
        let vm = cx.vm();
        let values = rectify_numberish_values(vm, &args);

        // 2. If args is empty, throw a SyntaxError.
        if values.is_empty() {
            return Err(Exception::syntax_error(Utf16String::from_utf8(
                "Cannot create an empty CSSMathSum",
            )));
        }

        // 3. Let type be the result of adding the types of all the items of args. If type is failure, throw a TypeError.
        // 4. Return a new CSSMathSum whose values internal slot is set to args.
        Self::add_all_types_into_math_sum(vm, &values)
    }

    pub(crate) fn math_value(&self) -> &CSSMathValue {
        &self.math_value
    }

    pub(crate) fn numeric_array(&self) -> Ref<CSSNumericArray> {
        self.values
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathsum-values
    pub(crate) fn values(&self, _: &mut CallContext) -> WebIdlResult<Ref<CSSNumericArray>> {
        Ok(self.values)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmathvalue
    pub(crate) fn serialize_math_value(&self, s: &mut String, nested: bool, parenless: bool) {
        // NB: Only steps 1 and 3 apply here.
        // 1. Let s initially be the empty string.

        // 3. Otherwise, if this is a CSSMathSum:
        // 1. If paren-less is true, continue to the next step; otherwise, if nested is true, append "(" to s;
        //    otherwise, append "calc(" to s.
        serialize_opening(s, nested, parenless);

        // 2. Serialize the first item in this’s values internal slot with nested set to true, and append the result
        //    to s.
        let values = self.values.values();
        values[0].serialize(s, NESTED);

        // 3. For each arg in this's values internal slot beyond the first:
        for arg in &values[1..] {
            // 1. If arg is a CSSMathNegate, append " - " to s, then serialize arg’s value internal slot with nested
            //    set to true, and append the result to s.
            if let AnyCSSNumericValue::CSSMathNegate(negate) = arg {
                s.push_str(" - ");
                negate.inner_value().serialize(s, NESTED);
            }
            // 2. Otherwise, append " + " to s, then serialize arg with nested set to true, and append the result to s.
            else {
                s.push_str(" + ");
                arg.serialize(s, NESTED);
            }
        }

        // 4. If paren-less is false, append ")" to s,
        if !parenless {
            s.push(')');
        }

        // 5. Return s.
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-a-sum-value
    pub(crate) fn create_a_sum_value(&self) -> Option<SumValue> {
        // 1. Let values initially be an empty list.
        let mut values = SumValue::new();

        // 2. For each item in this’s values internal slot:
        for item in self.values.values() {
            // 1. Let value be the result of creating a sum value from item. If value is failure, return failure.
            let value = item.create_a_sum_value()?;

            // 2. For each subvalue of value:
            for subvalue in value {
                // 1. If values already contains an item with the same unit map as subvalue, increment that item’s
                //    value by the value of subvalue.
                if let Some(existing_item) = values.iter_mut().find(|other| other.unit_map == subvalue.unit_map) {
                    existing_item.value += subvalue.value;
                }
                // 2. Otherwise, append subvalue to values.
                else {
                    values.push(subvalue);
                }
            }
        }

        // 3. Create a type from the unit map of each item of values, and add all the types together.
        //    If the result is failure, return failure.
        let mut added_type = create_a_type_from_a_unit_map(&values.first()?.unit_map)?;
        for item in &values[1..] {
            added_type = added_type.added_to(&create_a_type_from_a_unit_map(&item.unit_map)?)?;
        }

        // 4. Return values.
        Some(values)
    }

    pub(crate) fn create_calculation_node(&self) -> WebIdlResult<Arc<CalcNode>> {
        let child_nodes = self
            .values
            .values()
            .iter()
            .map(AnyCSSNumericValue::create_calculation_node)
            .collect::<WebIdlResult<Vec<_>>>()?;
        Ok(Arc::new(CalcNode::Sum(child_nodes)))
    }
}
