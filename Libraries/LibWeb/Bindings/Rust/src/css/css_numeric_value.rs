/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSNumericValue, https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue

use core::ops::Deref;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{MarkedVec, ThrowCompletionOr, Trace, Value, Vm};
use libweb_rust::typed_om::{
    BASE_TYPE_ANGLE, BASE_TYPE_FLEX, BASE_TYPE_FREQUENCY, BASE_TYPE_LENGTH, BASE_TYPE_PERCENT, BASE_TYPE_RESOLUTION,
    BASE_TYPE_TIME, CalcNode, NumericType, RetainedStyleValue, SumValue, create_a_type_from_unit,
    parse_a_numeric_value,
};

use super::css_math_clamp::CSSMathClamp;
use super::css_math_invert::CSSMathInvert;
use super::css_math_max::CSSMathMax;
use super::css_math_min::CSSMathMin;
use super::css_math_negate::CSSMathNegate;
use super::css_math_product::CSSMathProduct;
use super::css_math_sum::CSSMathSum;
use super::css_math_value::CSSMathValue;
use super::css_style_value::{AnyCSSStyleValue, CSSStyleValue, PerformTypeCheck};
use super::css_unit_value::CSSUnitValue;
use super::typed_om;
use crate::bindings::conversions::{ToJs, UsvString};
use crate::bindings::interface::{Inherits, Ref, Wrappable, wrap};
use crate::bindings::{CallContext, Exception, WebIdlResult};

/// The enumeration CSSNumericBaseType of CSS/CSSNumericValue.idl.
#[derive(Clone, Copy, Eq, PartialEq)]
pub(crate) enum CSSNumericBaseType {
    Length,
    Angle,
    Time,
    Frequency,
    Resolution,
    Flex,
    Percent,
}

/// The dictionary CSSNumericType of CSS/CSSNumericValue.idl.
#[derive(Default)]
pub(crate) struct CSSNumericType {
    pub(crate) length: Option<i32>,
    pub(crate) angle: Option<i32>,
    pub(crate) time: Option<i32>,
    pub(crate) frequency: Option<i32>,
    pub(crate) resolution: Option<i32>,
    pub(crate) flex: Option<i32>,
    pub(crate) percent: Option<i32>,
    pub(crate) percent_hint: Option<CSSNumericBaseType>,
}

/// The union (double or CSSNumericValue) of CSS/CSSNumericValue.idl.
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum CSSNumberish {
    Double(#[gc(untraced)] f64),
    CSSNumericValue(AnyCSSNumericValue),
}

/// An object of CSSNumericValue or of an interface that inherits from it.
#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
#[expect(clippy::enum_variant_names, reason = "the variants are named after the interfaces")]
pub(crate) enum AnyCSSNumericValue {
    CSSNumericValue(Ref<CSSNumericValue>),
    CSSMathClamp(Ref<CSSMathClamp>),
    CSSMathInvert(Ref<CSSMathInvert>),
    CSSMathMax(Ref<CSSMathMax>),
    CSSMathMin(Ref<CSSMathMin>),
    CSSMathNegate(Ref<CSSMathNegate>),
    CSSMathProduct(Ref<CSSMathProduct>),
    CSSMathSum(Ref<CSSMathSum>),
    CSSMathValue(Ref<CSSMathValue>),
    CSSUnitValue(Ref<CSSUnitValue>),
}

impl From<AnyCSSNumericValue> for AnyCSSStyleValue {
    fn from(value: AnyCSSNumericValue) -> Self {
        match value {
            AnyCSSNumericValue::CSSNumericValue(object) => Self::CSSNumericValue(object),
            AnyCSSNumericValue::CSSMathClamp(object) => Self::CSSMathClamp(object),
            AnyCSSNumericValue::CSSMathInvert(object) => Self::CSSMathInvert(object),
            AnyCSSNumericValue::CSSMathMax(object) => Self::CSSMathMax(object),
            AnyCSSNumericValue::CSSMathMin(object) => Self::CSSMathMin(object),
            AnyCSSNumericValue::CSSMathNegate(object) => Self::CSSMathNegate(object),
            AnyCSSNumericValue::CSSMathProduct(object) => Self::CSSMathProduct(object),
            AnyCSSNumericValue::CSSMathSum(object) => Self::CSSMathSum(object),
            AnyCSSNumericValue::CSSMathValue(object) => Self::CSSMathValue(object),
            AnyCSSNumericValue::CSSUnitValue(object) => Self::CSSUnitValue(object),
        }
    }
}

impl Deref for AnyCSSNumericValue {
    type Target = CSSNumericValue;

    fn deref(&self) -> &CSSNumericValue {
        match self {
            Self::CSSNumericValue(object) => object,
            Self::CSSMathClamp(object) => object.math_value().parent(),
            Self::CSSMathInvert(object) => object.math_value().parent(),
            Self::CSSMathMax(object) => object.math_value().parent(),
            Self::CSSMathMin(object) => object.math_value().parent(),
            Self::CSSMathNegate(object) => object.math_value().parent(),
            Self::CSSMathProduct(object) => object.math_value().parent(),
            Self::CSSMathSum(object) => object.math_value().parent(),
            Self::CSSMathValue(object) => <CSSMathValue as Inherits<CSSNumericValue>>::parent(object),
            Self::CSSUnitValue(object) => <CSSUnitValue as Inherits<CSSNumericValue>>::parent(object),
        }
    }
}

impl ToJs for AnyCSSNumericValue {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        Ok(Value::from_object(match self {
            Self::CSSNumericValue(object) => wrap(cx, object),
            Self::CSSMathClamp(object) => wrap(cx, object),
            Self::CSSMathInvert(object) => wrap(cx, object),
            Self::CSSMathMax(object) => wrap(cx, object),
            Self::CSSMathMin(object) => wrap(cx, object),
            Self::CSSMathNegate(object) => wrap(cx, object),
            Self::CSSMathProduct(object) => wrap(cx, object),
            Self::CSSMathSum(object) => wrap(cx, object),
            Self::CSSMathValue(object) => wrap(cx, object),
            Self::CSSUnitValue(object) => wrap(cx, object),
        }))
    }
}

/// The arguments of the serialization of a CSSNumericValue,
/// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssnumericvalue
#[derive(Clone, Copy, Default)]
pub(crate) struct SerializationParams {
    pub(crate) minimum: Option<f64>,
    pub(crate) maximum: Option<f64>,
    pub(crate) nested: bool,
    pub(crate) parenless: bool,
}

impl AnyCSSNumericValue {
    /// The object as the unit value that it is, if it is one.
    pub(crate) fn as_unit_value(&self) -> Option<Ref<CSSUnitValue>> {
        match self {
            Self::CSSUnitValue(object) => Some(*object),
            _ => None,
        }
    }

    pub(crate) fn numeric_value_style_value(&self) -> &CSSStyleValue {
        self.deref().parent()
    }

    /// Tells the part of the object that all CSSStyleValues have which object it is the part of.
    pub(crate) fn finish(self, vm: &Vm) -> Self {
        self.numeric_value_style_value().set_object(vm, self.into());
        self
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#equal-numeric-value
    pub(crate) fn is_equal_numeric_value(&self, other: &Self) -> bool {
        // 1. If value1 and value2 are not members of the same interface, return false.
        // NB: The steps of each interface return false for an object of another.
        match (self, other) {
            (Self::CSSUnitValue(first), Self::CSSUnitValue(second)) => first.is_equal_unit_value(second),
            (Self::CSSMathSum(first), Self::CSSMathSum(second)) => {
                first.numeric_array().is_equal_numeric_values(&second.numeric_array())
            }
            (Self::CSSMathProduct(first), Self::CSSMathProduct(second)) => {
                first.numeric_array().is_equal_numeric_values(&second.numeric_array())
            }
            (Self::CSSMathMin(first), Self::CSSMathMin(second)) => {
                first.numeric_array().is_equal_numeric_values(&second.numeric_array())
            }
            (Self::CSSMathMax(first), Self::CSSMathMax(second)) => {
                first.numeric_array().is_equal_numeric_values(&second.numeric_array())
            }
            // 4. Assert: value1 and value2 are both CSSMathNegates or CSSMathInverts.
            // 5. Return whether value1’s value and value2’s value are equal numeric values.
            (Self::CSSMathNegate(first), Self::CSSMathNegate(second)) => {
                first.inner_value().is_equal_numeric_value(&second.inner_value())
            }
            (Self::CSSMathInvert(first), Self::CSSMathInvert(second)) => {
                first.inner_value().is_equal_numeric_value(&second.inner_value())
            }
            (Self::CSSMathClamp(first), Self::CSSMathClamp(second)) => first.is_equal_clamp(second),
            _ => false,
        }
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-a-sum-value
    pub(crate) fn create_a_sum_value(&self) -> Option<SumValue> {
        match self {
            Self::CSSNumericValue(_) | Self::CSSMathValue(_) => {
                unreachable!("no object is only of an abstract interface")
            }
            Self::CSSMathClamp(object) => object.create_a_sum_value(),
            Self::CSSMathInvert(object) => object.create_a_sum_value(),
            Self::CSSMathMax(object) => object.create_a_sum_value(),
            Self::CSSMathMin(object) => object.create_a_sum_value(),
            Self::CSSMathNegate(object) => object.create_a_sum_value(),
            Self::CSSMathProduct(object) => object.create_a_sum_value(),
            Self::CSSMathSum(object) => object.create_a_sum_value(),
            Self::CSSUnitValue(object) => Some(object.create_a_sum_value()),
        }
    }

    pub(crate) fn create_calculation_node(&self) -> WebIdlResult<Arc<CalcNode>> {
        match self {
            Self::CSSNumericValue(_) | Self::CSSMathValue(_) => {
                unreachable!("no object is only of an abstract interface")
            }
            Self::CSSMathClamp(object) => object.create_calculation_node(),
            Self::CSSMathInvert(object) => object.create_calculation_node(),
            Self::CSSMathMax(object) => object.create_calculation_node(),
            Self::CSSMathMin(object) => object.create_calculation_node(),
            Self::CSSMathNegate(object) => object.create_calculation_node(),
            Self::CSSMathProduct(object) => object.create_calculation_node(),
            Self::CSSMathSum(object) => object.create_calculation_node(),
            Self::CSSUnitValue(object) => object.create_calculation_node(),
        }
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssnumericvalue
    pub(crate) fn serialize(&self, s: &mut String, params: SerializationParams) {
        // To serialize a CSSNumericValue this, given an optional minimum, a numeric value, and optional maximum, a
        // numeric value:
        // 1. If this is a CSSUnitValue, serialize a CSSUnitValue from this, passing minimum and maximum. Return the
        //    result.
        // 2. Otherwise, serialize a CSSMathValue from this, and return the result.
        let (nested, parenless) = (params.nested, params.parenless);
        match self {
            Self::CSSNumericValue(_) | Self::CSSMathValue(_) => {
                unreachable!("no object is only of an abstract interface")
            }
            Self::CSSUnitValue(object) => object.serialize_unit_value(s, params.minimum, params.maximum),
            Self::CSSMathClamp(object) => object.serialize_math_value(s),
            Self::CSSMathInvert(object) => object.serialize_math_value(s, nested, parenless),
            Self::CSSMathMax(object) => object.serialize_math_value(s),
            Self::CSSMathMin(object) => object.serialize_math_value(s),
            Self::CSSMathNegate(object) => object.serialize_math_value(s, nested, parenless),
            Self::CSSMathProduct(object) => object.serialize_math_value(s, nested, parenless),
            Self::CSSMathSum(object) => object.serialize_math_value(s, nested, parenless),
        }
    }

    pub(crate) fn to_string_with(self, params: SerializationParams) -> String {
        let mut s = String::new();
        self.serialize(&mut s, params);
        s
    }

    pub(crate) fn to_css_string(self) -> Utf16String {
        Utf16String::from_utf8(&self.to_string_with(SerializationParams::default()))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
    pub(crate) fn math_value_internal_representation(
        &self,
        property_id: u16,
        perform_type_check: PerformTypeCheck,
    ) -> WebIdlResult<RetainedStyleValue> {
        // If value is a CSSStyleValue subclass,
        //     If value does not match the grammar of a list-valued property iteration of property, throw a TypeError.
        //
        //     If any component of property’s CSS grammar has a limited numeric range, and the corresponding part of
        //     value is a CSSUnitValue that is outside of that range, replace that value with the result of wrapping it
        //     in a fresh CSSMathSum whose values internal slot contains only that part of value.
        //
        //     Return the value.
        let node = self.create_calculation_node()?;
        typed_om::calculated_style_value(property_id, node, self.numeric_type(), perform_type_check)
            .ok_or_else(|| Exception::type_error("Property does not accept values of this type."))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#cssmath-negate-a-cssnumericvalue
    pub(crate) fn negate(&self, vm: &Vm) -> Self {
        // 1. If this is a CSSMathNegate object, return this’s value internal slot.
        if let Self::CSSMathNegate(negate) = self {
            return negate.inner_value();
        }

        // 2. If this is a CSSUnitValue object, return a new CSSUnitValue with the same unit internal slot as this, and
        //    a value internal slot set to the negation of this’s.
        if let Self::CSSUnitValue(unit_value) = self {
            return Self::CSSUnitValue(CSSUnitValue::create(vm, -unit_value.raw_value(), unit_value.raw_unit()));
        }

        // 3. Otherwise, return a new CSSMathNegate object whose value internal slot is set to this.
        Self::CSSMathNegate(CSSMathNegate::create(vm, *self))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#cssmath-invert-a-cssnumericvalue
    pub(crate) fn invert(&self, vm: &Vm) -> WebIdlResult<Self> {
        // 1. If this is a CSSMathInvert object, return this’s value internal slot.
        if let Self::CSSMathInvert(invert) = self {
            return Ok(invert.inner_value());
        }

        // 2. If this is a CSSUnitValue object with unit internal slot set to "number":
        if let Self::CSSUnitValue(unit_value) = self
            && unit_value.raw_unit() == "number"
        {
            // 1. If this’s value internal slot is set to 0 or -0, throw a RangeError.
            if unit_value.raw_value() == 0.0 {
                return Err(Exception::range_error("Zero has no multiplicative inverse"));
            }

            // 2. Else return a new CSSUnitValue with the unit internal slot set to "number", and a value internal slot
            //    set to 1 divided by this’s {CSSUnitValue/value}} internal slot.
            return Ok(Self::CSSUnitValue(CSSUnitValue::create(
                vm,
                1.0 / unit_value.raw_value(),
                "number",
            )));
        }

        // 3. Otherwise, return a new CSSMathInvert object whose value internal slot is set to this.
        Ok(Self::CSSMathInvert(CSSMathInvert::create(vm, *self)))
    }

    /// The values of this object if it is of the interface that an operation makes, or this object, as the first steps
    /// of add(), mul(), min() and max() prepend them to values.
    fn with_rectified_values<'vm>(
        &self,
        vm: &'vm Vm,
        own_values: Option<&[Self]>,
        initial_values: &MarkedVec<'_, CSSNumberish>,
    ) -> MarkedVec<'vm, Self> {
        // 1. Replace each item of values with the result of rectifying a numberish value for the item.
        // 2. If this is a CSSMathSum object, prepend the items in this’s values internal slot to values.
        //    Otherwise, prepend this to values.
        // NB: We reorder the steps a little to avoid the awkward prepending.
        let values = MarkedVec::new(vm);
        match own_values {
            Some(own_values) => values.extend_from_slice(own_values),
            None => values.push(*self),
        }
        for value in initial_values.values().iter() {
            values.push(rectify_a_numberish_value(vm, value, None));
        }
        values
    }

    fn add(&self, vm: &Vm, initial_values: &MarkedVec<'_, CSSNumberish>) -> WebIdlResult<Self> {
        let own_values = match self {
            Self::CSSMathSum(math_sum) => Some(math_sum.numeric_array()),
            _ => None,
        };
        let own_values = own_values.as_ref().map(|array| array.values());
        let values = self.with_rectified_values(vm, own_values, initial_values);

        // 3. If all of the items in values are CSSUnitValues and have the same unit, return a new CSSUnitValue whose
        //    unit internal slot is set to that unit, and value internal slot is set to the sum of the value internal
        //    slots of the items in values. This addition must be done "left to right" - if values is « 1, 2, 3, 4 »,
        //    the result must be (((1 + 2) + 3) + 4). (This detail is necessary to ensure interoperability in the
        //    presence of floating-point arithmetic.)
        if let Some(result) = apply_math_operation_on_css_unit_values(vm, &values, |a, b| a + b) {
            return Ok(result);
        }

        // 4. Let type be the result of adding the types of every item in values. If type is failure, throw a TypeError.
        // 5. Return a new CSSMathSum object whose values internal slot is set to values.
        Ok(Self::CSSMathSum(CSSMathSum::add_all_types_into_math_sum(vm, &values)?))
    }

    fn mul(&self, vm: &Vm, initial_values: &MarkedVec<'_, CSSNumberish>) -> WebIdlResult<Self> {
        let own_values = match self {
            Self::CSSMathProduct(math_product) => Some(math_product.numeric_array()),
            _ => None,
        };
        let own_values = own_values.as_ref().map(|array| array.values());
        let values = self.with_rectified_values(vm, own_values, initial_values);

        // 3. If all of the items in values are CSSUnitValues with unit internal slot set to "number", return a new
        //    CSSUnitValue whose unit internal slot is set to "number", and value internal slot is set to the product of
        //    the value internal slots of the items in values.
        //
        //    This multiplication must be done "left to right" - if values is « 1, 2, 3, 4 », the result must be
        //    (((1 × 2) × 3) × 4). (This detail is necessary to ensure interoperability in the presence of
        //    floating-point arithmetic.)
        //
        // 4. If all of the items in values are CSSUnitValues with unit internal slot set to "number" except one which
        //    is set to unit, return a new CSSUnitValue whose unit internal slot is set to unit, and value internal slot
        //    is set to the product of the value internal slots of the items in values.
        //
        //    This multiplication must be done "left to right" - if values is « 1, 2, 3, 4 », the result must be
        //    (((1 × 2) × 3) × 4).
        if let Some(unit_values) = css_unit_values_of(vm, &values) {
            let unit_values = unit_values.values();
            let mut other_units = unit_values.iter().filter(|value| value.raw_unit() != "number");
            let first_other_unit = other_units.next();
            if other_units.next().is_none() {
                let product = unit_values
                    .iter()
                    .fold(1.0, |product, value| product * value.raw_value());
                let unit = first_other_unit.map_or("number", |value| value.raw_unit());
                return Ok(Self::CSSUnitValue(CSSUnitValue::create(vm, product, unit)));
            }
        }

        // 5. Let type be the result of multiplying the types of every item in values. If type is failure, throw a
        //    TypeError.
        // 6. Return a new CSSMathProduct object whose values internal slot is set to values.
        Ok(Self::CSSMathProduct(
            CSSMathProduct::multiply_all_types_into_math_product(vm, &values)?,
        ))
    }
}

/// The CSSUnitValues of the values, if that is all they are.
fn css_unit_values_of<'vm>(
    vm: &'vm Vm,
    values: &MarkedVec<'_, AnyCSSNumericValue>,
) -> Option<MarkedVec<'vm, Ref<CSSUnitValue>>> {
    let unit_values = MarkedVec::with_capacity(vm, values.len());
    for value in values.values().iter() {
        unit_values.push(value.as_unit_value()?);
    }
    Some(unit_values)
}

/// Whether all of the values are CSSUnitValues with the same unit, in which case the operation runs over them from left
/// to right, and the new CSSUnitValue that results.
fn apply_math_operation_on_css_unit_values(
    vm: &Vm,
    values: &MarkedVec<'_, AnyCSSNumericValue>,
    operation: impl Fn(f64, f64) -> f64,
) -> Option<AnyCSSNumericValue> {
    let unit_values = css_unit_values_of(vm, values)?;
    let unit_values = unit_values.values();
    let (first, rest) = unit_values.split_first().expect("an operation has at least one value");
    if rest.iter().any(|value| value.raw_unit() != first.raw_unit()) {
        return None;
    }
    let result = rest
        .iter()
        .fold(first.raw_value(), |result, value| operation(result, value.raw_value()));
    Some(AnyCSSNumericValue::CSSUnitValue(CSSUnitValue::create(
        vm,
        result,
        first.raw_unit(),
    )))
}

// https://drafts.css-houdini.org/css-typed-om-1/#rectify-a-numberish-value
pub(crate) fn rectify_a_numberish_value(vm: &Vm, numberish: &CSSNumberish, unit: Option<&str>) -> AnyCSSNumericValue {
    match numberish {
        CSSNumberish::CSSNumericValue(value) => *value,
        CSSNumberish::Double(value) => {
            AnyCSSNumericValue::CSSUnitValue(CSSUnitValue::create(vm, *value, unit.unwrap_or("number")))
        }
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSNumericValue {
    style_value: CSSStyleValue,
    // https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-type
    #[gc(untraced)]
    numeric_type: NumericType,
}

impl Wrappable for CSSNumericValue {}

impl Inherits<CSSStyleValue> for CSSNumericValue {
    fn parent(&self) -> &CSSStyleValue {
        &self.style_value
    }
}

fn to_om_numeric_base_type(base_type: usize) -> CSSNumericBaseType {
    match base_type {
        BASE_TYPE_LENGTH => CSSNumericBaseType::Length,
        BASE_TYPE_ANGLE => CSSNumericBaseType::Angle,
        BASE_TYPE_TIME => CSSNumericBaseType::Time,
        BASE_TYPE_FREQUENCY => CSSNumericBaseType::Frequency,
        BASE_TYPE_RESOLUTION => CSSNumericBaseType::Resolution,
        BASE_TYPE_FLEX => CSSNumericBaseType::Flex,
        BASE_TYPE_PERCENT => CSSNumericBaseType::Percent,
        _ => unreachable!("a numeric type has seven base types"),
    }
}

impl CSSNumericValue {
    pub(crate) fn new(numeric_type: NumericType) -> Self {
        Self {
            style_value: CSSStyleValue::empty(),
            numeric_type,
        }
    }

    pub(crate) fn numeric_type(&self) -> NumericType {
        self.numeric_type
    }

    /// The object that this is the CSSNumericValue part of.
    fn this(&self) -> AnyCSSNumericValue {
        self.style_value.object().as_numeric().expect("the object is numeric")
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-add
    pub(crate) fn add(
        &self,
        cx: &mut CallContext,
        values: MarkedVec<'_, CSSNumberish>,
    ) -> WebIdlResult<AnyCSSNumericValue> {
        self.this().add(cx.vm(), &values)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-sub
    pub(crate) fn sub(
        &self,
        cx: &mut CallContext,
        initial_values: MarkedVec<'_, CSSNumberish>,
    ) -> WebIdlResult<AnyCSSNumericValue> {
        // 1. Replace each item of values with the result of rectifying a numberish value for the item, then negating
        //    the value.
        let vm = cx.vm();
        let values = MarkedVec::new(vm);
        for value in initial_values.values().iter() {
            values.push(CSSNumberish::CSSNumericValue(
                rectify_a_numberish_value(vm, value, None).negate(vm),
            ));
        }

        // 2. Return the result of calling the add() internal algorithm with this and values.
        self.this().add(vm, &values)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-mul
    pub(crate) fn mul(
        &self,
        cx: &mut CallContext,
        values: MarkedVec<'_, CSSNumberish>,
    ) -> WebIdlResult<AnyCSSNumericValue> {
        self.this().mul(cx.vm(), &values)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-div
    pub(crate) fn div(
        &self,
        cx: &mut CallContext,
        initial_values: MarkedVec<'_, CSSNumberish>,
    ) -> WebIdlResult<AnyCSSNumericValue> {
        // 1. Replace each item of values with the result of rectifying a numberish value for the item, then inverting
        //    the value.
        let vm = cx.vm();
        let values = MarkedVec::new(vm);
        for value in initial_values.values().iter() {
            values.push(CSSNumberish::CSSNumericValue(
                rectify_a_numberish_value(vm, value, None).invert(vm)?,
            ));
        }

        // 2. Return the result of calling the mul() internal algorithm with this and values.
        self.this().mul(vm, &values)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-min
    pub(crate) fn min(
        &self,
        cx: &mut CallContext,
        initial_values: MarkedVec<'_, CSSNumberish>,
    ) -> WebIdlResult<AnyCSSNumericValue> {
        let vm = cx.vm();
        let this = self.this();
        let own_values = match this {
            AnyCSSNumericValue::CSSMathMin(math_min) => Some(math_min.numeric_array()),
            _ => None,
        };
        let own_values = own_values.as_ref().map(|array| array.values());
        let values = this.with_rectified_values(vm, own_values, &initial_values);

        // 3. If all of the items in values are CSSUnitValues and have the same unit, return a new CSSUnitValue whose
        //    unit internal slot is set to that unit, and value internal slot is set to the minimum of the value
        //    internal slots of the items in values.
        if let Some(result) = apply_math_operation_on_css_unit_values(vm, &values, |a, b| if b < a { b } else { a }) {
            return Ok(result);
        }

        // 4. Let type be the result of adding the types of every item in values. If type is failure, throw a TypeError.
        // 5. Return a new CSSMathMin object whose values internal slot is set to values.
        Ok(AnyCSSNumericValue::CSSMathMin(CSSMathMin::add_all_types_into_math_min(
            vm, &values,
        )?))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-max
    pub(crate) fn max(
        &self,
        cx: &mut CallContext,
        initial_values: MarkedVec<'_, CSSNumberish>,
    ) -> WebIdlResult<AnyCSSNumericValue> {
        let vm = cx.vm();
        let this = self.this();
        let own_values = match this {
            AnyCSSNumericValue::CSSMathMax(math_max) => Some(math_max.numeric_array()),
            _ => None,
        };
        let own_values = own_values.as_ref().map(|array| array.values());
        let values = this.with_rectified_values(vm, own_values, &initial_values);

        // 3. If all of the items in values are CSSUnitValues and have the same unit, return a new CSSUnitValue whose
        //    unit internal slot is set to that unit, and value internal slot is set to the maximum of the value
        //    internal slots of the items in values.
        if let Some(result) = apply_math_operation_on_css_unit_values(vm, &values, |a, b| if a < b { b } else { a }) {
            return Ok(result);
        }

        // 4. Let type be the result of adding the types of every item in values. If type is failure, throw a TypeError.
        // 5. Return a new CSSMathMax object whose values internal slot is set to values.
        Ok(AnyCSSNumericValue::CSSMathMax(CSSMathMax::add_all_types_into_math_max(
            vm, &values,
        )?))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-equals
    pub(crate) fn equals_for_bindings(
        &self,
        cx: &mut CallContext,
        values: MarkedVec<'_, CSSNumberish>,
    ) -> WebIdlResult<bool> {
        // The equals(...values) method, when called on a CSSNumericValue this, must perform the following steps:

        // 1. Replace each item of values with the result of rectifying a numberish value for the item.
        // 2. For each item in values, if the item is not an equal numeric value to this, return false.
        let this = self.this();
        for value in values.values().iter() {
            let rectified_value = rectify_a_numberish_value(cx.vm(), value, None);
            if !this.is_equal_numeric_value(&rectified_value) {
                return Ok(false);
            }
        }

        // 3. Return true.
        Ok(true)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-to
    pub(crate) fn to(&self, cx: &mut CallContext, unit: UsvString) -> WebIdlResult<Ref<CSSUnitValue>> {
        let unit = typed_om::to_rust_string(&unit.into_utf16_string());
        self.this().to_unit(cx.vm(), &unit)
    }

    pub(crate) fn type_for_bindings(&self, _: &mut CallContext) -> WebIdlResult<CSSNumericType> {
        let mut result = CSSNumericType::default();
        for (base_type, power) in self.numeric_type.exponents.iter().enumerate() {
            let Some(power) = power.filter(|power| *power != 0) else {
                continue;
            };
            let entry = match base_type {
                BASE_TYPE_LENGTH => &mut result.length,
                BASE_TYPE_ANGLE => &mut result.angle,
                BASE_TYPE_TIME => &mut result.time,
                BASE_TYPE_FREQUENCY => &mut result.frequency,
                BASE_TYPE_RESOLUTION => &mut result.resolution,
                BASE_TYPE_FLEX => &mut result.flex,
                BASE_TYPE_PERCENT => &mut result.percent,
                _ => unreachable!("a numeric type has seven base types"),
            };
            *entry = Some(power);
        }
        result.percent_hint = self
            .numeric_type
            .percent_hint
            .map(|percent_hint| to_om_numeric_base_type(usize::from(percent_hint)));
        Ok(result)
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-parse
    pub(crate) fn parse(cx: &mut CallContext, css_text: UsvString) -> WebIdlResult<AnyCSSNumericValue> {
        // The parse(cssText) method, when called, must perform the following steps:
        let value = parse_a_numeric_value(&css_text.into_utf16_string()).ok_or_else(|| {
            Exception::syntax_error(Utf16String::from_utf8("Input is not a numeric component value."))
        })?;
        typed_om::reify_a_numeric_value(cx.vm(), &value)
            .ok_or_else(|| Exception::syntax_error(Utf16String::from_utf8("Unable to reify this math function.")))
    }
}

impl AnyCSSNumericValue {
    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssnumericvalue-to
    pub(crate) fn to_unit(self, vm: &Vm, unit: &str) -> WebIdlResult<Ref<CSSUnitValue>> {
        // The to(unit) method converts an existing CSSNumericValue this into another one with the specified unit, if
        // possible. When called, it must perform the following steps:

        // 1. Let type be the result of creating a type from unit. If type is failure, throw a SyntaxError.
        if create_a_type_from_unit(unit).is_none() {
            return Err(Exception::syntax_error(Utf16String::from_utf8(&format!(
                "Unrecognized unit '{unit}'"
            ))));
        }

        // 2. Let sum be the result of creating a sum value from this. If sum is failure, throw a TypeError.
        let Some(sum) = self.create_a_sum_value() else {
            return Err(Exception::type_error(&format!(
                "Unable to create a sum from input '{}'",
                self.to_string_with(SerializationParams::default())
            )));
        };

        // 3. If sum has more than one item, throw a TypeError.
        //    Otherwise, let item be the result of creating a CSSUnitValue from the sole item in sum, then converting it
        //    to unit. If item is failure, throw a TypeError.
        if sum.len() > 1 {
            return Err(Exception::type_error("Sum contains more than one item"));
        }
        let Some(item) = CSSUnitValue::create_from_sum_value_item(vm, &sum[0]) else {
            return Err(Exception::type_error(&format!(
                "Unable to create CSSUnitValue from input '{}'",
                self.to_string_with(SerializationParams::default())
            )));
        };
        let Some(converted_item) = item.converted_to_unit(vm, unit) else {
            return Err(Exception::type_error("Unable to convert input to requested unit"));
        };

        // 4. Return item.
        Ok(converted_item)
    }
}
