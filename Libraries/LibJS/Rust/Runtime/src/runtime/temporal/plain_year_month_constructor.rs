/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! %Temporal.PlainYearMonth%.

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::runtime::temporal::abstract_operations::to_integer_with_truncation;
use crate::runtime::temporal::calendar::canonicalize_calendar;
use crate::runtime::temporal::plain_date::{compare_iso_date, create_iso_date_record, is_valid_iso_date};
use crate::runtime::temporal::plain_year_month::{create_temporal_year_month, to_temporal_year_month};
use crate::utf16::Utf16View;

// 9.1 The Temporal.PlainYearMonth Constructor, https://tc39.es/proposal-temporal/#sec-temporal-plainyearmonth-constructor
#[repr(C)]
#[derive(Trace)]
pub struct PlainYearMonthConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    PlainYearMonthConstructor,
    initialize: PlainYearMonthConstructor::initialize,
    call: PlainYearMonthConstructor::call,
    construct: PlainYearMonthConstructor::construct
);

impl PlainYearMonthConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PlainYearMonthConstructor> {
        realm.create_object(
            vm,
            PlainYearMonthConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.PlainYearMonth.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 9.2.1 Temporal.PlainYearMonth.prototype, https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().temporal_plain_year_month_prototype(vm)),
            PropertyAttributes::new(0),
        );

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(2),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.from,
            raw_native!(PlainYearMonthConstructor::from),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.compare,
            raw_native!(PlainYearMonthConstructor::compare),
            2,
            attr,
            None,
        );
    }

    // 9.1.1 Temporal.PlainYearMonth ( isoYear, isoMonth [ , calendar [ , referenceISODay ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Temporal.PlainYearMonth"],
        )
    }

    // 9.1.1 Temporal.PlainYearMonth ( isoYear, isoMonth [ , calendar [ , referenceISODay ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let iso_year = vm.argument(0);
        let iso_month = vm.argument(1);
        let mut calendar_value = vm.argument(2);
        let mut reference_iso_day = vm.argument(3);

        // 2. If referenceISODay is undefined, then
        if reference_iso_day.is_undefined() {
            // a. Set referenceISODay to 1𝔽.
            reference_iso_day = Value::from_i32(1);
        }

        // 3. Let y be ? ToIntegerWithTruncation(isoYear).
        let year = to_integer_with_truncation(vm, iso_year, ErrorType::TemporalInvalidPlainYearMonth, &[])?;

        // 4. Let m be ? ToIntegerWithTruncation(isoMonth).
        let month = to_integer_with_truncation(vm, iso_month, ErrorType::TemporalInvalidPlainYearMonth, &[])?;

        // 5. If calendar is undefined, set calendar to "iso8601".
        if calendar_value.is_undefined() {
            calendar_value = Value::from_string(PrimitiveString::create_from_utf8(vm, "iso8601"));
        }

        // 6. If calendar is not a String, throw a TypeError exception.
        if !calendar_value.is_string() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAString, &[&"calendar"]);
        }

        // 7. Set calendar to ? CanonicalizeCalendar(calendar).
        let calendar_string = calendar_value.as_string().utf16_string();
        let calendar = canonicalize_calendar(vm, Utf16View::of_string(&calendar_string))?;

        // 8. Let ref be ? ToIntegerWithTruncation(referenceISODay).
        let reference =
            to_integer_with_truncation(vm, reference_iso_day, ErrorType::TemporalInvalidPlainYearMonth, &[])?;

        // 9. If IsValidISODate(y, m, ref) is false, throw a RangeError exception.
        if !is_valid_iso_date(year, month, reference) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainYearMonth, &[]);
        }

        // 10. Let isoDate be CreateISODateRecord(y, m, ref).
        let iso_date = create_iso_date_record(year, month, reference);

        // 11. Return ? CreateTemporalYearMonth(isoDate, calendar, NewTarget).
        Ok(create_temporal_year_month(vm, iso_date, calendar, Some(new_target))?.upcast())
    }

    // 9.2.2 Temporal.PlainYearMonth.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.from
    fn from(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? ToTemporalYearMonth(item, options).
        Ok(Value::from_object(to_temporal_year_month(
            vm,
            vm.argument(0),
            vm.argument(1),
        )?))
    }

    // 9.2.3 Temporal.PlainYearMonth.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.plainyearmonth.compare
    fn compare(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Set one to ? ToTemporalYearMonth(one).
        let one = to_temporal_year_month(vm, vm.argument(0), Value::UNDEFINED)?;

        // 2. Set two to ? ToTemporalYearMonth(two).
        let two = to_temporal_year_month(vm, vm.argument(1), Value::UNDEFINED)?;

        // 3. Return 𝔽(CompareISODate(one.[[ISODate]], two.[[ISODate]])).
        Ok(Value::from_i32(i32::from(compare_iso_date(
            one.iso_date(),
            two.iso_date(),
        ))))
    }
}
