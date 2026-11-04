/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSUnitValue, https://drafts.css-houdini.org/css-typed-om-1/#cssunitvalue

use core::cell::Cell;
use std::sync::Arc;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};
use libweb_rust::typed_om::{
    CalcNode, NumericValue, RetainedStyleValue, SumValue, SumValueItem, UnitMap, conversion_ratio,
    create_a_type_from_unit, in_canonical_unit, serialize_a_number,
};

use super::css_numeric_value::{AnyCSSNumericValue, CSSNumericValue};
use super::css_style_value::PerformTypeCheck;
use super::typed_om;
use crate::bindings::conversions::UsvString;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate};
use crate::bindings::{CallContext, Exception, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSUnitValue {
    numeric_value: CSSNumericValue,
    #[gc(untraced)]
    value: Cell<f64>,
    #[gc(untraced)]
    unit: String,
}

impl Wrappable for CSSUnitValue {}

impl Inherits<CSSNumericValue> for CSSUnitValue {
    fn parent(&self) -> &CSSNumericValue {
        &self.numeric_value
    }
}

impl CSSUnitValue {
    /// A new CSSUnitValue with a unit that is known to be one.
    pub(crate) fn create(vm: &Vm, value: f64, unit: &str) -> Ref<Self> {
        // The type of a CSSUnitValue is the result of creating a type from its unit internal slot.
        // https://drafts.css-houdini.org/css-typed-om-1/#type-of-a-cssunitvalue
        let numeric_type = create_a_type_from_unit(unit).expect("the unit of a CSSUnitValue is a unit");
        let object = allocate(vm, |_| Self {
            numeric_value: CSSNumericValue::new(numeric_type),
            value: Cell::new(value),
            // AD-HOC: WPT expects the unit to be lowercase but this doesn't seem to be specified anywhere.
            unit: unit.to_ascii_lowercase(),
        });
        AnyCSSNumericValue::CSSUnitValue(object).finish(vm);
        object
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-a-cssunitvalue-from-a-sum-value-item
    pub(crate) fn create_from_sum_value_item(vm: &Vm, item: &SumValueItem) -> Option<Ref<Self>> {
        // 1. If item has more than one entry in its unit map, return failure.
        if item.unit_map.len() > 1 {
            return None;
        }

        // 2. If item has no entries in its unit map, return a new CSSUnitValue whose unit internal slot is set to
        //    "number", and whose value internal slot is set to item’s value.
        let Some((unit, power)) = item.unit_map.iter().next() else {
            return Some(Self::create(vm, item.value, "number"));
        };

        // 3. Otherwise, item has a single entry in its unit map. If that entry’s value is anything other than 1, return
        //    failure.
        if *power != 1 {
            return None;
        }

        // 4. Otherwise, return a new CSSUnitValue whose unit internal slot is set to that entry’s key, and whose value
        //    internal slot is set to item’s value.
        Some(Self::create(vm, item.value, unit))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunitvalue-cssunitvalue
    pub(crate) fn construct(cx: &mut CallContext, value: f64, unit: UsvString) -> WebIdlResult<Ref<Self>> {
        // 1. If creating a type from unit returns failure, throw a TypeError and abort this algorithm.
        let unit = typed_om::to_rust_string(&unit.into_utf16_string());
        if create_a_type_from_unit(&unit).is_none() {
            return Err(Exception::type_error(
                "Cannot create CSSUnitValue with unrecognized unit",
            ));
        }

        // 2. Return a new CSSUnitValue with its value internal slot set to value and its unit set to unit.
        Ok(Self::create(cx.vm(), value, &unit))
    }

    pub(crate) fn raw_value(&self) -> f64 {
        self.value.get()
    }

    pub(crate) fn raw_unit(&self) -> &str {
        &self.unit
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunitvalue-value
    pub(crate) fn value(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.value.get())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunitvalue-value
    pub(crate) fn set_value(&self, _: &mut CallContext, value: f64) -> WebIdlResult<()> {
        // AD-HOC: No definition: https://github.com/w3c/css-houdini-drafts/issues/1146
        self.value.set(value);
        Ok(())
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunitvalue-unit
    pub(crate) fn unit(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(Utf16String::from_utf8(&self.unit))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssunitvalue
    pub(crate) fn serialize_unit_value(&self, s: &mut String, minimum: Option<f64>, maximum: Option<f64>) {
        // To serialize a CSSUnitValue this, with optional arguments minimum, a numeric value, and maximum, a numeric
        // value:

        // 1. Let value and unit be this‘s value and unit internal slots.
        let value = self.value.get();

        // 4. If minimum was passed and this is less than minimum, or if maximum was passed and this is greater than
        //    maximum, or either minimum and/or maximum were passed and the relative size of this and minimum/maximum
        //    can’t be determined with the available information at this time, prepend "calc(" to s, then append ")" to
        //    s.
        // NB: This step is done first to avoid prepending to the string.
        // FIXME: "or either minimum and/or maximum were passed and the relative size of this and minimum/maximum can't
        //        be determined with the available information at this time"
        let needs_calc_wrapper =
            minimum.is_some_and(|minimum| value < minimum) || maximum.is_some_and(|maximum| value > maximum);
        if needs_calc_wrapper {
            s.push_str("calc(");
        }

        // 2. Set s to the result of serializing a <number> from value, per CSSOM §6.7.2 Serializing CSS Values.
        serialize_a_number(s, value);

        // 3. If unit is:
        match self.unit.as_str() {
            // -> "number"
            // Do nothing.
            "number" => {}
            // -> "percent"
            // Append "%" to s.
            "percent" => s.push('%'),
            // -> anything else
            // Append unit to s.
            unit => s.push_str(unit),
        }

        if needs_calc_wrapper {
            s.push(')');
        }

        // 5. Return s.
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#convert-a-cssunitvalue
    pub(crate) fn converted_to_unit(&self, vm: &Vm, unit: &str) -> Option<Ref<Self>> {
        // 1. Let old unit be the value of this’s unit internal slot, and old value be the value of this’s value
        //    internal slot.
        // 2. If old unit and unit are not compatible units, return failure.
        let ratio = conversion_ratio(&self.unit, unit)?;

        // 3. Return a new CSSUnitValue whose unit internal slot is set to unit, and whose value internal slot is set to
        //    old value multiplied by the conversation ratio between old unit and unit.
        Some(Self::create(vm, self.value.get() * ratio, unit))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#equal-numeric-value
    pub(crate) fn is_equal_unit_value(&self, other: &Self) -> bool {
        // 2. If value1 and value2 are both CSSUnitValues, return true if they have equal unit and value internal slots,
        //    or false otherwise.
        self.unit == other.unit && self.value.get() == other.value.get()
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-a-sum-value
    pub(crate) fn create_a_sum_value(&self) -> SumValue {
        // 1. Let unit be the value of this’s unit internal slot, and value be the value of this’s value internal slot.
        // 2. If unit is a member of a set of compatible units, and is not the set’s canonical unit, multiply value by
        //    the conversion ratio between unit and the canonical unit, and change unit to the canonical unit.
        let (value, unit) = in_canonical_unit(self.value.get(), &self.unit);

        // 3. If unit is "number", return «(value, «[ ]»)».
        if unit == "number" {
            return vec![SumValueItem {
                value,
                unit_map: UnitMap::new(),
            }];
        }

        // 4. Otherwise, return «(value, «[unit → 1]»)».
        vec![SumValueItem {
            value,
            unit_map: UnitMap::from([(unit, 1)]),
        }]
    }

    pub(crate) fn create_calculation_node(&self) -> WebIdlResult<Arc<CalcNode>> {
        NumericValue::from_unit(self.value.get(), &self.unit)
            .map(NumericValue::calculation_node)
            .ok_or_else(|| Exception::type_error("Unable to create calculation node."))
    }

    // https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
    pub(crate) fn create_an_internal_representation(
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
        let Some(value) = NumericValue::from_unit(self.value.get(), &self.unit) else {
            return Err(Exception::type_error("Unrecognized unit."));
        };
        if perform_type_check == PerformTypeCheck::No {
            return Ok(value.style_value());
        }
        typed_om::numeric_style_value_for_property(property_id, value, self.numeric_value.numeric_type())
            .ok_or_else(|| Exception::type_error("Property does not accept values of this type."))
    }
}
