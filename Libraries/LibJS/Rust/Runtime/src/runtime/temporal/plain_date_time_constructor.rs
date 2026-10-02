/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/PlainDateTimeConstructor.cpp: %Temporal.PlainDateTime%.

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
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::realm::Realm;
use crate::runtime::temporal::abstract_operations::{ascii_view, to_integer_with_truncation};
use crate::runtime::temporal::calendar::{ISO8601_CALENDAR, canonicalize_calendar};
use crate::runtime::temporal::plain_date::{create_iso_date_record, is_valid_iso_date};
use crate::runtime::temporal::plain_date_time::{
    combine_iso_date_and_time_record, compare_iso_date_time, create_temporal_date_time, to_temporal_date_time,
};
use crate::runtime::temporal::plain_time::{create_time_record, is_valid_time};
use crate::utf16::Utf16View;

// 5.1 The Temporal.PlainDateTime Constructor, https://tc39.es/proposal-temporal/#sec-temporal-plaindatetime-constructor
#[repr(C)]
#[derive(Trace)]
pub struct PlainDateTimeConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    PlainDateTimeConstructor,
    initialize: PlainDateTimeConstructor::initialize,
    call: PlainDateTimeConstructor::call,
    construct: PlainDateTimeConstructor::construct
);

impl PlainDateTimeConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PlainDateTimeConstructor> {
        realm.create_object(
            vm,
            PlainDateTimeConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.PlainDateTime.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 5.2.1 Temporal.PlainDateTime.prototype, https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().temporal_plain_date_time_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.from,
            raw_native!(PlainDateTimeConstructor::from),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.compare,
            raw_native!(PlainDateTimeConstructor::compare),
            2,
            attr,
            None,
        );

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(3),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 5.1.1 Temporal.PlainDateTime ( isoYear, isoMonth, isoDay [ , hour [ , minute [ , second [ , millisecond [ , microsecond [ , nanosecond [ , calendar ] ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Temporal.PlainDateTime"],
        )
    }

    // 5.1.1 Temporal.PlainDateTime ( isoYear, isoMonth, isoDay [ , hour [ , minute [ , second [ , millisecond [ , microsecond [ , nanosecond [ , calendar ] ] ] ] ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let calendar_value = vm.argument(9);

        let mut index = 0;
        let mut next_integer_argument = |fallback: Option<f64>| -> ThrowCompletionOr<f64> {
            let value = vm.argument(index);
            index += 1;
            if value.is_undefined()
                && let Some(fallback) = fallback
            {
                return Ok(fallback);
            }

            to_integer_with_truncation(vm, value, ErrorType::TemporalInvalidPlainDateTime, &[])
        };

        // 2. Set isoYear to ? ToIntegerWithTruncation(isoYear).
        let iso_year = next_integer_argument(None)?;

        // 3. Set isoMonth to ? ToIntegerWithTruncation(isoMonth).
        let iso_month = next_integer_argument(None)?;

        // 4. Set isoDay to ? ToIntegerWithTruncation(isoDay).
        let iso_day = next_integer_argument(None)?;

        // 5. If hour is undefined, set hour to 0; else set hour to ? ToIntegerWithTruncation(hour).
        let hour = next_integer_argument(Some(0.0))?;

        // 6. If minute is undefined, set minute to 0; else set minute to ? ToIntegerWithTruncation(minute).
        let minute = next_integer_argument(Some(0.0))?;

        // 7. If second is undefined, set second to 0; else set second to ? ToIntegerWithTruncation(second).
        let second = next_integer_argument(Some(0.0))?;

        // 8. If millisecond is undefined, set millisecond to 0; else set millisecond to ? ToIntegerWithTruncation(millisecond).
        let millisecond = next_integer_argument(Some(0.0))?;

        // 9. If microsecond is undefined, set microsecond to 0; else set microsecond to ? ToIntegerWithTruncation(microsecond).
        let microsecond = next_integer_argument(Some(0.0))?;

        // 10. If nanosecond is undefined, set nanosecond to 0; else set nanosecond to ? ToIntegerWithTruncation(nanosecond).
        let nanosecond = next_integer_argument(Some(0.0))?;

        // 11. If calendar is undefined, set calendar to "iso8601".
        // 12. If calendar is not a String, throw a TypeError exception.
        // 13. Set calendar to ? CanonicalizeCalendar(calendar).
        let calendar = if calendar_value.is_undefined() {
            canonicalize_calendar(vm, ascii_view(ISO8601_CALENDAR))?
        } else if !calendar_value.is_string() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAString, &[&"calendar"]);
        } else {
            let calendar_string = calendar_value.as_string().utf16_string();
            canonicalize_calendar(vm, Utf16View::of_string(&calendar_string))?
        };

        // 14. If IsValidISODate(isoYear, isoMonth, isoDay) is false, throw a RangeError exception.
        if !is_valid_iso_date(iso_year, iso_month, iso_day) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainDateTime, &[]);
        }

        // 15. Let isoDate be CreateISODateRecord(isoYear, isoMonth, isoDay).
        let iso_date = create_iso_date_record(iso_year, iso_month, iso_day);

        // 16. If IsValidTime(hour, minute, second, millisecond, microsecond, nanosecond) is false, throw a RangeError exception.
        if !is_valid_time(hour, minute, second, millisecond, microsecond, nanosecond) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainDateTime, &[]);
        }

        // 17. Let time be CreateTimeRecord(hour, minute, second, millisecond, microsecond, nanosecond).
        let time = create_time_record(hour, minute, second, millisecond, microsecond, nanosecond, 0.0);

        // 18. Let isoDateTime be CombineISODateAndTimeRecord(isoDate, time).
        let iso_date_time = combine_iso_date_and_time_record(iso_date, time);

        // 19. Return ? CreateTemporalDateTime(isoDateTime, calendar, NewTarget).
        Ok(create_temporal_date_time(vm, &iso_date_time, calendar, Some(new_target))?.upcast())
    }

    // 5.2.2 Temporal.PlainDateTime.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.from
    fn from(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? ToTemporalDateTime(item, options).
        Ok(Value::from_object(to_temporal_date_time(
            vm,
            vm.argument(0),
            vm.argument(1),
        )?))
    }

    // 5.2.3 Temporal.PlainDateTime.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.compare
    fn compare(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Set one to ? ToTemporalDateTime(one).
        let one = to_temporal_date_time(vm, vm.argument(0), Value::UNDEFINED)?;

        // 2. Set two to ? ToTemporalDateTime(two).
        let two = to_temporal_date_time(vm, vm.argument(1), Value::UNDEFINED)?;

        // 3. Return 𝔽(CompareISODateTime(one.[[ISODateTime]], two.[[ISODateTime]])).
        Ok(Value::from_i32(i32::from(compare_iso_date_time(
            &one.iso_date_time(),
            &two.iso_date_time(),
        ))))
    }
}
