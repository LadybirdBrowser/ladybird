/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::intl_object::{IntlObject, ResolutionOptionDescriptor};
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_key::PropertyKey;
use crate::unicode::intl::{self as unicode, Style};
use crate::unicode::relative_time_format::{
    self as unicode_relative_time_format, NumericDisplay, RelativeTimeFormatPartition, TimeUnit,
};
use crate::utf16::Utf16View;

/// 18 RelativeTimeFormat Objects, https://tc39.es/ecma402/#relativetimeformat-objects
#[repr(C)]
#[derive(Trace)]
pub struct RelativeTimeFormat {
    base: Object,
    #[gc(untraced)]
    locale: GcRefCell<Utf16String>, // [[Locale]]
    #[gc(untraced)]
    numbering_system: GcRefCell<Utf16String>, // [[NumberingSystem]]
    #[gc(untraced)]
    style: Cell<Style>, // [[Style]]
    #[gc(untraced)]
    numeric: Cell<NumericDisplay>, // [[Numeric]]

    // Non-standard. Stores the ICU relative-time formatter for the Intl object's formatting options.
    #[gc(untraced)]
    formatter: GcRefCell<Option<unicode_relative_time_format::RelativeTimeFormat>>,
}

define_cell!(RelativeTimeFormat, Object, extends: [Object], finalize: finalize);

impl Deref for RelativeTimeFormat {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for RelativeTimeFormat {
    fn finalize(&self) {
        drop(self.formatter.replace(None));
    }
}

impl RelativeTimeFormat {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> RelativeTimeFormat {
        RelativeTimeFormat {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            locale: GcRefCell::default(),
            numbering_system: GcRefCell::default(),
            style: Cell::new(Style::Long),
            numeric: Cell::new(NumericDisplay::Always),
            formatter: GcRefCell::new(None),
        }
    }

    pub fn locale(&self) -> Utf16String {
        self.locale.borrow().clone()
    }

    pub fn set_locale(&self, locale: Utf16String) {
        self.locale.replace(locale);
    }

    pub fn numbering_system(&self) -> Utf16String {
        self.numbering_system.borrow().clone()
    }

    pub fn set_numbering_system(&self, numbering_system: Utf16String) {
        self.numbering_system.replace(numbering_system);
    }

    pub fn style(&self) -> Style {
        self.style.get()
    }

    pub fn set_style(&self, style: Utf16View<'_>) {
        self.style.set(unicode::style_from_string(style));
    }

    pub fn style_string(&self) -> &'static str {
        unicode::style_to_string(self.style.get())
    }

    pub fn numeric(&self) -> NumericDisplay {
        self.numeric.get()
    }

    pub fn set_numeric(&self, numeric: Utf16View<'_>) {
        self.numeric
            .set(unicode_relative_time_format::numeric_display_from_string(numeric));
    }

    pub fn numeric_string(&self) -> &'static str {
        unicode_relative_time_format::numeric_display_to_string(self.numeric.get())
    }

    /// Calls `callback` with the ICU relative-time formatter, which must not allocate or call into the VM.
    pub fn with_formatter<R>(
        &self,
        callback: impl FnOnce(&unicode_relative_time_format::RelativeTimeFormat) -> R,
    ) -> R {
        let formatter = self.formatter.borrow();
        callback(
            formatter
                .as_ref()
                .expect("the Intl.RelativeTimeFormat constructor creates the ICU relative-time formatter"),
        )
    }

    pub fn set_formatter(&self, formatter: unicode_relative_time_format::RelativeTimeFormat) {
        self.formatter.replace(Some(formatter));
    }
}

impl IntlObject for RelativeTimeFormat {
    // 18.2.3 Internal slots, https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat-internal-slots
    fn relevant_extension_keys(&self) -> &'static [&'static str] {
        // The value of the [[RelevantExtensionKeys]] internal slot is « "nu" ».
        &["nu"]
    }

    // 18.2.3 Internal slots, https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat-internal-slots
    fn resolution_option_descriptors<'vm>(&self, vm: &'vm Vm) -> Vec<ResolutionOptionDescriptor<'vm>> {
        // The value of the [[ResolutionOptionDescriptors]] internal slot is « { [[Key]]: "nu", [[Property]]: "numberingSystem" } ».
        vec![ResolutionOptionDescriptor::string("nu", &vm.names.numberingSystem)]
    }
}

// 18.5.1 SingularRelativeTimeUnit ( unit ), https://tc39.es/ecma402/#sec-singularrelativetimeunit
pub fn singular_relative_time_unit(vm: &Vm, unit: Utf16View<'_>) -> ThrowCompletionOr<TimeUnit> {
    // 1. If unit is "seconds", return "second".
    if unit == "seconds" {
        return Ok(TimeUnit::Second);
    }
    // 2. If unit is "minutes", return "minute".
    if unit == "minutes" {
        return Ok(TimeUnit::Minute);
    }
    // 3. If unit is "hours", return "hour".
    if unit == "hours" {
        return Ok(TimeUnit::Hour);
    }
    // 4. If unit is "days", return "day".
    if unit == "days" {
        return Ok(TimeUnit::Day);
    }
    // 5. If unit is "weeks", return "week".
    if unit == "weeks" {
        return Ok(TimeUnit::Week);
    }
    // 6. If unit is "months", return "month".
    if unit == "months" {
        return Ok(TimeUnit::Month);
    }
    // 7. If unit is "quarters", return "quarter".
    if unit == "quarters" {
        return Ok(TimeUnit::Quarter);
    }
    // 8. If unit is "years", return "year".
    if unit == "years" {
        return Ok(TimeUnit::Year);
    }

    // 9. If unit is not one of "second", "minute", "hour", "day", "week", "month", "quarter", or "year", throw a RangeError exception.
    // 10. Return unit.
    if let Some(time_unit) = unicode_relative_time_format::time_unit_from_string(unit) {
        return Ok(time_unit);
    }
    vm.throw_completion_with_utf16_message(ErrorKind::RangeError, ErrorType::IntlInvalidUnit.utf16_message(&[unit]))
}

/// The first steps of PartitionRelativeTimePattern, which FormatRelativeTime performs as well.
fn validate_relative_time_value_and_unit(vm: &Vm, value: f64, unit: Utf16View<'_>) -> ThrowCompletionOr<TimeUnit> {
    // 1. If value is NaN, +∞𝔽, or -∞𝔽, throw a RangeError exception.
    if !value.is_finite() {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::NumberIsNaNOrInfinity, &[]);
    }

    // 2. Let unit be ? SingularRelativeTimeUnit(unit).
    singular_relative_time_unit(vm, unit)
}

// 18.5.2 PartitionRelativeTimePattern ( relativeTimeFormat, value, unit ), https://tc39.es/ecma402/#sec-PartitionRelativeTimePattern
pub fn partition_relative_time_pattern(
    vm: &Vm,
    relative_time_format: &RelativeTimeFormat,
    value: f64,
    unit: Utf16View<'_>,
) -> ThrowCompletionOr<Vec<RelativeTimeFormatPartition>> {
    // 1. If value is NaN, +∞𝔽, or -∞𝔽, throw a RangeError exception.
    // 2. Let unit be ? SingularRelativeTimeUnit(unit).
    let time_unit = validate_relative_time_value_and_unit(vm, value, unit)?;

    let numeric = relative_time_format.numeric();
    Ok(relative_time_format.with_formatter(|formatter| formatter.format_to_parts(value, time_unit, numeric)))
}

// 18.5.4 FormatRelativeTime ( relativeTimeFormat, value, unit ), https://tc39.es/ecma402/#sec-FormatRelativeTime
pub fn format_relative_time(
    vm: &Vm,
    relative_time_format: &RelativeTimeFormat,
    value: f64,
    unit: Utf16View<'_>,
) -> ThrowCompletionOr<Utf16String> {
    // 1. Let parts be ? PartitionRelativeTimePattern(relativeTimeFormat, value, unit).
    // NOTE: We short-circuit PartitionRelativeTimePattern as we do not need individual partitions. But we must still
    //       perform the NaN/Infinity sanity checks and unit parsing from its first steps.
    let time_unit = validate_relative_time_value_and_unit(vm, value, unit)?;

    // 2. Let result be the empty String.
    // 3. For each Record { [[Type]], [[Value]], [[Unit]] } part in parts, do
    //     a. Set result to the string-concatenation of result and part.[[Value]].
    // 4. Return result.
    let numeric = relative_time_format.numeric();
    Ok(relative_time_format.with_formatter(|formatter| formatter.format(value, time_unit, numeric)))
}

// 18.5.5 FormatRelativeTimeToParts ( relativeTimeFormat, value, unit ), https://tc39.es/ecma402/#sec-FormatRelativeTimeToParts
pub fn format_relative_time_to_parts(
    vm: &Vm,
    relative_time_format: &RelativeTimeFormat,
    value: f64,
    unit: Utf16View<'_>,
) -> ThrowCompletionOr<Gc<Array>> {
    let realm = vm.current_realm().expect("FormatRelativeTimeToParts runs in a realm");

    // 1. Let parts be ? PartitionRelativeTimePattern(relativeTimeFormat, value, unit).
    let parts = partition_relative_time_pattern(vm, relative_time_format, value, unit)?;

    // 2. Let result be ! ArrayCreate(0).
    let result = Array::create(vm, realm, 0, None).must();

    // 3. Let n be 0.
    // 4. For each Record { [[Type]], [[Value]], [[Unit]] } part in parts, do
    for (n, part) in parts.into_iter().enumerate() {
        // a. Let O be OrdinaryObjectCreate(%Object.prototype%).
        let object = Object::create(vm, realm, Some(realm.object_prototype()));

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.type_,
                Value::from_string(PrimitiveString::create(vm, part.type_)),
            )
            .must();

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.value,
                Value::from_string(PrimitiveString::create(vm, part.value)),
            )
            .must();

        // d. If part.[[Unit]] is not empty, then
        if !part.unit.is_empty() {
            // i. Perform ! CreateDataPropertyOrThrow(O, "unit", part.[[Unit]]).
            object
                .create_data_property_or_throw(
                    vm,
                    &vm.names.unit,
                    Value::from_string(PrimitiveString::create(vm, part.unit)),
                )
                .must();
        }

        // e. Perform ! CreateDataPropertyOrThrow(result, ! ToString(n), O).
        result
            .create_data_property_or_throw(vm, &PropertyKey::from_number(n as u64), Value::from_object(object))
            .must();

        // f. Increment n by 1.
    }

    // 5. Return result.
    Ok(result)
}
