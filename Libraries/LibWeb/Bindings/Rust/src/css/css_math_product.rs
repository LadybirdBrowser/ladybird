/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSMathProduct, https://drafts.css-houdini.org/css-typed-om-1/#cssmathproduct

use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{MarkedVec, Trace, Vm};
use libweb_rust::typed_om::{CalcNode, NumericType, SumValue, SumValueItem, UnitMap, product_of_two_unit_maps};

use super::css_math_value::*;
use super::css_numeric_array::CSSNumericArray;
use super::css_numeric_value::{AnyCSSNumericValue, CSSNumberish};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSMathProduct {
    math_value: CSSMathValue,
    values: Ref<CSSNumericArray>,
}

impl Wrappable for CSSMathProduct {}

impl Inherits<CSSMathValue> for CSSMathProduct {
    fn parent(&self) -> &CSSMathValue {
        &self.math_value
    }
}

impl CSSMathProduct {
    pub(crate) fn create(vm: &Vm, numeric_type: NumericType, values: Ref<CSSNumericArray>) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            math_value: CSSMathValue::new(CSSMathOperator::Product, numeric_type),
            values,
        });
        AnyCSSNumericValue::CSSMathProduct(object).finish(vm);
        object
    }

    pub(crate) fn multiply_all_types_into_math_product(
        vm: &Vm,
        values: &MarkedVec<'_, AnyCSSNumericValue>,
    ) -> WebIdlResult<Ref<Self>> {
        let Some(numeric_type) = multiply_all_types(&values.to_vec()) else {
            return Err(Exception::type_error(
                "Cannot create a CSSMathProduct with values of incompatible types",
            ));
        };
        let values = CSSNumericArray::create(vm, values);
        Ok(Self::create(vm, numeric_type, values))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathproduct-cssmathproduct
    pub(crate) fn construct(cx: &mut CallContext, args: MarkedVec<'_, CSSNumberish>) -> WebIdlResult<Ref<Self>> {
        // The CSSMathProduct(...args) constructor is defined identically to the above, except that in step 3 it
        // multiplies the types instead of adding, and in the last step it returns a CSSMathProduct.

        // 1. Replace each item of args with the result of rectifying a numberish value for the item.
        let vm = cx.vm();
        let values = rectify_numberish_values(vm, &args);

        // 2. If args is empty, throw a SyntaxError.
        if values.is_empty() {
            return Err(Exception::syntax_error(Utf16String::from_utf8(
                "Cannot create an empty CSSMathProduct",
            )));
        }

        // 3. Let type be the result of multiplying the types of all the items of args. If type is failure, throw a TypeError.
        // 4. Return a new CSSMathProduct whose values internal slot is set to args.
        Self::multiply_all_types_into_math_product(vm, &values)
    }

    pub(crate) fn math_value(&self) -> &CSSMathValue {
        &self.math_value
    }

    pub(crate) fn numeric_array(&self) -> Ref<CSSNumericArray> {
        self.values
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathproduct-values
    pub(crate) fn values(&self, _: &mut CallContext) -> WebIdlResult<Ref<CSSNumericArray>> {
        Ok(self.values)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmathvalue
    pub(crate) fn serialize_math_value(&self, s: &mut String, nested: bool, parenless: bool) {
        // NB: Only steps 1 and 5 apply here.
        // 1. Let s initially be the empty string.

        // 5. Otherwise, if this is a CSSMathProduct:
        // 1. If paren-less is true, continue to the next step; otherwise, if nested is true, append "(" to s;
        //    otherwise, append "calc(" to s.
        serialize_opening(s, nested, parenless);

        // 2. Serialize the first item in this’s values internal slot with nested set to true, and append the result
        //    to s.
        let values = self.values.values();
        values[0].serialize(s, NESTED);

        // 3. For each arg in this’s values internal slot beyond the first:
        for arg in &values[1..] {
            // 1. If arg is a CSSMathInvert, append " / " to s, then serialize arg’s value internal slot with nested
            //    set to true, and append the result to s.
            if let AnyCSSNumericValue::CSSMathInvert(invert) = arg {
                s.push_str(" / ");
                invert.inner_value().serialize(s, NESTED);
            }
            // 2. Otherwise, append " * " to s, then serialize arg with nested set to true, and append the result to s.
            else {
                s.push_str(" * ");
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
        // 1. Let values initially be the sum value «(1, «[ ]»)». (I.e. what you’d get from 1.)
        let mut values = vec![SumValueItem {
            value: 1.0,
            unit_map: UnitMap::new(),
        }];

        // 2. For each item in this’s values internal slot:
        for item in self.values.values() {
            // 1. Let new values be the result of creating a sum value from item.
            //    Let temp initially be an empty list.
            // 2. If new values is failure, return failure.
            let new_values = item.create_a_sum_value()?;
            let mut temp = SumValue::new();

            // 3. For each item1 in values:
            for item1 in &values {
                // 1. For each item2 in new values:
                for item2 in &new_values {
                    // 1. Let item be a tuple with its value set to the product of the values of item1 and item2, and
                    //    its unit map set to the product of the unit maps of item1 and item2, with all entries with a
                    //    zero value removed.
                    let mut unit_map = product_of_two_unit_maps(&item1.unit_map, &item2.unit_map);
                    unit_map.retain(|_, power| *power != 0);

                    // 2. Append item to temp.
                    temp.push(SumValueItem {
                        value: item1.value * item2.value,
                        unit_map,
                    });
                }
            }

            // 4. Set values to temp.
            values = temp;
        }

        // 3. Return values.
        Some(values)
    }

    pub(crate) fn create_calculation_node(&self) -> WebIdlResult<Arc<CalcNode>> {
        let child_nodes = self
            .values
            .values()
            .iter()
            .map(AnyCSSNumericValue::create_calculation_node)
            .collect::<WebIdlResult<Vec<_>>>()?;
        Ok(Arc::new(CalcNode::Product(child_nodes)))
    }
}
