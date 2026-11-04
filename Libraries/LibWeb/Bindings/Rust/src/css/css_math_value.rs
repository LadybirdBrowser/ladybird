/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSMathValue, https://drafts.css-houdini.org/css-typed-om-1/#cssmathvalue

use libjs_rust::api::{MarkedVec, Trace, Vm};
use libweb_rust::typed_om::{NumericType, SumValue};

use super::css_numeric_value::{
    AnyCSSNumericValue, CSSNumberish, CSSNumericValue, SerializationParams, rectify_a_numberish_value,
};
use crate::bindings::interface::{Inherits, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};

/// The enumeration CSSMathOperator of CSS/CSSMathValue.idl.
#[derive(Clone, Copy, Eq, PartialEq)]
pub(crate) enum CSSMathOperator {
    Sum,
    Product,
    Negate,
    Invert,
    Min,
    Max,
    Clamp,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSMathValue {
    numeric_value: CSSNumericValue,
    #[gc(untraced)]
    operator: CSSMathOperator,
}

impl Wrappable for CSSMathValue {}

impl Inherits<CSSNumericValue> for CSSMathValue {
    fn parent(&self) -> &CSSNumericValue {
        &self.numeric_value
    }
}

impl CSSMathValue {
    pub(crate) fn new(operator: CSSMathOperator, numeric_type: NumericType) -> Self {
        Self {
            numeric_value: CSSNumericValue::new(numeric_type),
            operator,
        }
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssmathvalue-operator
    pub(crate) fn operator(&self, _: &mut CallContext) -> WebIdlResult<CSSMathOperator> {
        Ok(self.operator)
    }
}

/// The result of rectifying a numberish value for each item of the arguments of a constructor.
pub(crate) fn rectify_numberish_values<'vm>(
    vm: &'vm Vm,
    args: &MarkedVec<'_, CSSNumberish>,
) -> MarkedVec<'vm, AnyCSSNumericValue> {
    let values = MarkedVec::new(vm);
    for value in args.values().iter() {
        values.push(rectify_a_numberish_value(vm, value, None));
    }
    values
}

/// The result of adding the types of all the values, if that is not failure.
pub(crate) fn add_all_types(values: &[AnyCSSNumericValue]) -> Option<NumericType> {
    let (first, rest) = values.split_first().expect("a math value has at least one value");
    rest.iter().try_fold(first.numeric_type(), |numeric_type, value| {
        numeric_type.added_to(&value.numeric_type())
    })
}

/// The result of multiplying the types of all the values, if that is not failure.
pub(crate) fn multiply_all_types(values: &[AnyCSSNumericValue]) -> Option<NumericType> {
    let (first, rest) = values.split_first().expect("a math value has at least one value");
    rest.iter().try_fold(first.numeric_type(), |numeric_type, value| {
        numeric_type.multiplied_by(&value.numeric_type())
    })
}

/// What a math value that is not min(), max() or clamp() starts with: if paren-less is true, continue to the next step;
/// otherwise, if nested is true, append "(" to s; otherwise, append "calc(" to s.
pub(crate) fn serialize_opening(s: &mut String, nested: bool, parenless: bool) {
    if parenless {
        return;
    }
    s.push_str(if nested { "(" } else { "calc(" });
}

pub(crate) const NESTED: SerializationParams = SerializationParams {
    minimum: None,
    maximum: None,
    nested: true,
    parenless: false,
};

pub(crate) const NESTED_AND_PARENLESS: SerializationParams = SerializationParams {
    minimum: None,
    maximum: None,
    nested: true,
    parenless: true,
};

/// The steps that serialize a CSSMathMin or a CSSMathMax, https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssmathvalue
pub(crate) fn serialize_min_or_max(s: &mut String, function: &str, values: &[AnyCSSNumericValue]) {
    // 2. If this is a CSSMathMin or CSSMathMax:
    // 1. Append "min(" or "max(" to s, as appropriate.
    s.push_str(function);
    s.push('(');

    // 2. For each arg in this’s values internal slot, serialize arg with nested and paren-less both true, and append
    //    the result to s, appending a ", " between successive values.
    for (index, arg) in values.iter().enumerate() {
        if index != 0 {
            s.push_str(", ");
        }
        arg.serialize(s, NESTED_AND_PARENLESS);
    }

    // 3. Append ")" to s and return s.
    s.push(')');
}

/// The steps that create a sum value from a CSSMathMin or a CSSMathMax, which end in the item that `is_better_than`
/// prefers, https://drafts.css-houdini.org/css-typed-om-1/#create-a-sum-value
pub(crate) fn min_or_max_sum_value(
    values: &[AnyCSSNumericValue],
    is_better_than: impl Fn(f64, f64) -> bool,
) -> Option<SumValue> {
    // 1. Let args be the result of creating a sum value for each item in this’s values internal slot.
    let args: Vec<Option<SumValue>> = values.iter().map(AnyCSSNumericValue::create_a_sum_value).collect();

    let mut best: Option<SumValue> = args.first()?.clone();
    for item in &args {
        // 2. If any item of args is failure, or has a length greater than one, return failure.
        let item = item.as_ref().filter(|item| item.len() <= 1)?;

        // 3. If not all of the unit maps among the items of args are identical, return failure.
        let best_item = best.as_ref()?;
        if item[0].unit_map != best_item[0].unit_map {
            return None;
        }
        if is_better_than(item[0].value, best_item[0].value) {
            best = Some(item.clone());
        }
    }

    // 4. Return the item of args whose sole item has the smallest value.
    best
}
