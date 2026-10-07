/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! %Temporal.PlainMonthDay%.

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
use crate::runtime::temporal::plain_date::{create_iso_date_record, is_valid_iso_date};
use crate::runtime::temporal::plain_month_day::{create_temporal_month_day, to_temporal_month_day};
use crate::utf16::Utf16View;

// 10.1 The Temporal.PlainMonthDay Constructor, https://tc39.es/proposal-temporal/#sec-temporal-plainmonthday-constructor
#[repr(C)]
#[derive(Trace)]
pub struct PlainMonthDayConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    PlainMonthDayConstructor,
    initialize: PlainMonthDayConstructor::initialize,
    call: PlainMonthDayConstructor::call,
    construct: PlainMonthDayConstructor::construct
);

impl PlainMonthDayConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PlainMonthDayConstructor> {
        realm.create_object(
            vm,
            PlainMonthDayConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.PlainMonthDay.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 10.2.1 Temporal.PlainMonthDay.prototype, https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().temporal_plain_month_day_prototype(vm)),
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
            raw_native!(PlainMonthDayConstructor::from),
            1,
            attr,
            None,
        );
    }

    // 10.1.1 Temporal.PlainMonthDay ( isoMonth, isoDay [ , calendar [ , referenceISOYear ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Temporal.PlainMonthDay"],
        )
    }

    // 10.1.1 Temporal.PlainMonthDay ( isoMonth, isoDay [ , calendar [ , referenceISOYear ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let iso_month = vm.argument(0);
        let iso_day = vm.argument(1);
        let mut calendar_value = vm.argument(2);
        let mut reference_iso_year = vm.argument(3);

        // 2. If referenceISOYear is undefined, then
        if reference_iso_year.is_undefined() {
            // a. Set referenceISOYear to 1972𝔽 (the first ISO 8601 leap year after the epoch).
            reference_iso_year = Value::from_i32(1972);
        }

        // 3. Let m be ? ToIntegerWithTruncation(isoMonth).
        let month = to_integer_with_truncation(vm, iso_month, ErrorType::TemporalInvalidPlainMonthDay, &[])?;

        // 4. Let d be ? ToIntegerWithTruncation(isoDay).
        let day = to_integer_with_truncation(vm, iso_day, ErrorType::TemporalInvalidPlainMonthDay, &[])?;

        // 5. If calendar is undefined, set calendar to "iso8601".
        if calendar_value.is_undefined() {
            calendar_value = Value::from_string(PrimitiveString::create_from_utf8(vm, "iso8601"));
        }

        // 6. If calendar is not a String, throw a TypeError exception.
        if !calendar_value.is_string() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAString, &[&calendar_value]);
        }

        // 7. Set calendar to ? CanonicalizeCalendar(calendar).
        let calendar_string = calendar_value.as_string().utf16_string();
        let calendar = canonicalize_calendar(vm, Utf16View::of_string(&calendar_string))?;

        // 8. Let y be ? ToIntegerWithTruncation(referenceISOYear).
        let year = to_integer_with_truncation(vm, reference_iso_year, ErrorType::TemporalInvalidPlainMonthDay, &[])?;

        // 9. If IsValidISODate(y, m, d) is false, throw a RangeError exception.
        if !is_valid_iso_date(year, month, day) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainMonthDay, &[]);
        }

        // 10. Let isoDate be CreateISODateRecord(y, m, d).
        let iso_date = create_iso_date_record(year, month, day);

        // 11. Return ? CreateTemporalMonthDay(isoDate, calendar, NewTarget).
        Ok(create_temporal_month_day(vm, iso_date, calendar, Some(new_target))?.upcast())
    }

    // 10.2.2 Temporal.PlainMonthDay.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plainmonthday.from
    fn from(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? ToTemporalMonthDay(item, options).
        Ok(Value::from_object(to_temporal_month_day(
            vm,
            vm.argument(0),
            vm.argument(1),
        )?))
    }
}
