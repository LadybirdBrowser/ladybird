/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSMathMin, https://drafts.css-houdini.org/css-typed-om-1/#cssmathmin

use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{MarkedVec, Trace, Vm};
use libweb_rust::typed_om::{CalcNode, NumericType, SumValue};

use super::css_math_value::*;
use super::css_numeric_array::CSSNumericArray;
use super::css_numeric_value::{AnyCSSNumericValue, CSSNumberish};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSMathMin {
    math_value: CSSMathValue,
    values: Ref<CSSNumericArray>,
}

impl Wrappable for CSSMathMin {}

impl Inherits<CSSMathValue> for CSSMathMin {
    fn parent(&self) -> &CSSMathValue {
        &self.math_value
    }
}

impl CSSMathMin {
    pub(crate) fn create(vm: &Vm, numeric_type: NumericType, values: Ref<CSSNumericArray>) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            math_value: CSSMathValue::new(CSSMathOperator::Min, numeric_type),
            values,
        });
        AnyCSSNumericValue::CSSMathMin(object).finish(vm);
        object
    }

    pub(crate) fn add_all_types_into_math_min(
        vm: &Vm,
        values: &MarkedVec<'_, AnyCSSNumericValue>,
    ) -> WebIdlResult<Ref<Self>> {
        let Some(numeric_type) = add_all_types(&values.to_vec()) else {
            return Err(Exception::type_error(
                "Cannot create a CSSMathMin with values of incompatible types",
            ));
        };
        let values = CSSNumericArray::create(vm, values);
        Ok(Self::create(vm, numeric_type, values))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathmin-cssmathmin
    pub(crate) fn construct(cx: &mut CallContext, args: MarkedVec<'_, CSSNumberish>) -> WebIdlResult<Ref<Self>> {
        // The CSSMathMin(...args) and CSSMathMax(...args) constructors are defined identically to the above, except
        // that in the last step they return a new CSSMathMin or CSSMathMax object, respectively.

        // 1. Replace each item of args with the result of rectifying a numberish value for the item.
        let vm = cx.vm();
        let values = rectify_numberish_values(vm, &args);

        // 2. If args is empty, throw a SyntaxError.
        if values.is_empty() {
            return Err(Exception::syntax_error(Utf16String::from_utf8(
                "Cannot create an empty CSSMathMin",
            )));
        }

        // 3. Let type be the result of adding the types of all the items of args. If type is failure, throw a TypeError.
        // 4. Return a new CSSMathMin whose values internal slot is set to args.
        Self::add_all_types_into_math_min(vm, &values)
    }

    pub(crate) fn math_value(&self) -> &CSSMathValue {
        &self.math_value
    }

    pub(crate) fn numeric_array(&self) -> Ref<CSSNumericArray> {
        self.values
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathmin-values
    pub(crate) fn values(&self, _: &mut CallContext) -> WebIdlResult<Ref<CSSNumericArray>> {
        Ok(self.values)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmathvalue
    pub(crate) fn serialize_math_value(&self, s: &mut String) {
        // NB: Only steps 1 and 2 apply here.
        // 1. Let s initially be the empty string.
        serialize_min_or_max(s, "min", self.values.values());
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-a-sum-value
    pub(crate) fn create_a_sum_value(&self) -> Option<SumValue> {
        min_or_max_sum_value(self.values.values(), |first, second| first < second)
    }

    pub(crate) fn create_calculation_node(&self) -> WebIdlResult<Arc<CalcNode>> {
        let child_nodes = self
            .values
            .values()
            .iter()
            .map(AnyCSSNumericValue::create_calculation_node)
            .collect::<WebIdlResult<Vec<_>>>()?;
        Ok(Arc::new(CalcNode::Min(child_nodes)))
    }
}
