/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSMathClamp, https://drafts.css-houdini.org/css-typed-om-1/#cssmathclamp

use std::sync::Arc;

use libjs_rust::api::{Trace, Vm};
use libweb_rust::typed_om::{CalcNode, NumericType, SumValue, SumValueItem};

use super::css_math_value::*;
use super::css_numeric_value::{AnyCSSNumericValue, CSSNumberish, rectify_a_numberish_value};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSMathClamp {
    math_value: CSSMathValue,
    lower: AnyCSSNumericValue,
    value: AnyCSSNumericValue,
    upper: AnyCSSNumericValue,
}

impl Wrappable for CSSMathClamp {}

impl Inherits<CSSMathValue> for CSSMathClamp {
    fn parent(&self) -> &CSSMathValue {
        &self.math_value
    }
}

impl CSSMathClamp {
    pub(crate) fn create(
        vm: &Vm,
        numeric_type: NumericType,
        lower: AnyCSSNumericValue,
        value: AnyCSSNumericValue,
        upper: AnyCSSNumericValue,
    ) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            math_value: CSSMathValue::new(CSSMathOperator::Clamp, numeric_type),
            lower,
            value,
            upper,
        });
        AnyCSSNumericValue::CSSMathClamp(object).finish(vm);
        object
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathclamp-cssmathclamp
    pub(crate) fn construct(
        cx: &mut CallContext,
        lower: CSSNumberish,
        value: CSSNumberish,
        upper: CSSNumberish,
    ) -> WebIdlResult<Ref<Self>> {
        // The CSSMathClamp(lower, value, upper) constructor must, when called, perform the following steps:
        // 1. Replace lower, value, and upper with the result of rectifying a numberish value for each.
        let vm = cx.vm();
        let lower = rectify_a_numberish_value(vm, &lower, None);
        let value = rectify_a_numberish_value(vm, &value, None);
        let upper = rectify_a_numberish_value(vm, &upper, None);

        // 2. Let type be the result of adding the types of lower, value, and upper. If type is failure, throw a
        //    TypeError.
        let Some(numeric_type) = add_all_types(&[lower, value, upper]) else {
            return Err(Exception::type_error(
                "Cannot create a CSSMathClamp with values of incompatible types",
            ));
        };

        // 3. Return a new CSSMathClamp whose lower, value, and upper internal slots are set to lower, value, and upper,
        //    respectively.
        Ok(Self::create(vm, numeric_type, lower, value, upper))
    }

    pub(crate) fn math_value(&self) -> &CSSMathValue {
        &self.math_value
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathclamp-lower
    pub(crate) fn lower(&self, _: &mut CallContext) -> WebIdlResult<AnyCSSNumericValue> {
        // AD-HOC: No spec definition.
        Ok(self.lower)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathclamp-value
    pub(crate) fn value(&self, _: &mut CallContext) -> WebIdlResult<AnyCSSNumericValue> {
        // AD-HOC: No spec definition.
        Ok(self.value)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathclamp-upper
    pub(crate) fn upper(&self, _: &mut CallContext) -> WebIdlResult<AnyCSSNumericValue> {
        // AD-HOC: No spec definition.
        Ok(self.upper)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmathvalue
    pub(crate) fn serialize_math_value(&self, s: &mut String) {
        // AD-HOC: The spec is missing serialization rules for CSSMathClamp:
        //         https://github.com/w3c/css-houdini-drafts/issues/1152
        s.push_str("clamp(");
        self.lower.serialize(s, NESTED_AND_PARENLESS);
        s.push_str(", ");
        self.value.serialize(s, NESTED_AND_PARENLESS);
        s.push_str(", ");
        self.upper.serialize(s, NESTED_AND_PARENLESS);
        s.push(')');
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#equal-numeric-value
    pub(crate) fn is_equal_clamp(&self, other: &Self) -> bool {
        // AD-HOC: Spec doesn't handle clamp(). https://github.com/w3c/css-houdini-drafts/issues/1152
        self.lower.is_equal_numeric_value(&other.lower)
            && self.value.is_equal_numeric_value(&other.value)
            && self.upper.is_equal_numeric_value(&other.upper)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-a-sum-value
    pub(crate) fn create_a_sum_value(&self) -> Option<SumValue> {
        // AD-HOC: There is no spec for this. https://github.com/w3c/css-houdini-drafts/issues/1152
        //         So, basing it on the spec for CSSMathMin.
        // Create sum values from lower, value, and upper.
        let lower = self.lower.create_a_sum_value()?;
        let value = self.value.create_a_sum_value()?;
        let upper = self.upper.create_a_sum_value()?;

        // If any of those are failure, or has a length greater than one, return failure.
        if lower.len() > 1 || value.len() > 1 || upper.len() > 1 {
            return None;
        }

        // If not all their unit maps are identical, return failure.
        if lower[0].unit_map != value[0].unit_map || value[0].unit_map != upper[0].unit_map {
            return None;
        }

        // Return value clamped between lower and upper.
        // NB: Like the CSS clamp() function, this lets a lower bound above the upper bound win, and keeps a NaN value.
        let at_most_upper = if upper[0].value < value[0].value {
            upper[0].value
        } else {
            value[0].value
        };
        let clamped = if at_most_upper < lower[0].value {
            lower[0].value
        } else {
            at_most_upper
        };
        Some(vec![SumValueItem {
            value: clamped,
            unit_map: value[0].unit_map.clone(),
        }])
    }

    pub(crate) fn create_calculation_node(&self) -> WebIdlResult<Arc<CalcNode>> {
        Ok(Arc::new(CalcNode::Clamp {
            min: Some(self.lower.create_calculation_node()?),
            center: self.value.create_calculation_node()?,
            max: Some(self.upper.create_calculation_node()?),
        }))
    }
}
