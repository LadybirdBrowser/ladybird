/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! %Temporal.PlainDate%.

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
use crate::runtime::temporal::plain_date::{
    compare_iso_date, create_iso_date_record, create_temporal_date, is_valid_iso_date, to_temporal_date,
};
use crate::utf16::Utf16View;

// 3.1 The Temporal.PlainDate Constructor, https://tc39.es/proposal-temporal/#sec-temporal-plaindate-constructor
#[repr(C)]
#[derive(Trace)]
pub struct PlainDateConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    PlainDateConstructor,
    initialize: PlainDateConstructor::initialize,
    call: PlainDateConstructor::call,
    construct: PlainDateConstructor::construct
);

impl PlainDateConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PlainDateConstructor> {
        realm.create_object(
            vm,
            PlainDateConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.PlainDate.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 3.2.1 Temporal.PlainDate.prototype, https://tc39.es/proposal-temporal/#sec-temporal.plaindate.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().temporal_plain_date_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.from,
            raw_native!(PlainDateConstructor::from),
            1,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &names.compare,
            raw_native!(PlainDateConstructor::compare),
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

    // 3.1.1 Temporal.PlainDate ( isoYear, isoMonth, isoDay [ , calendar ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::ConstructorWithoutNew,
            &[&"Temporal.PlainDate"],
        )
    }

    // 3.1.1 Temporal.PlainDate ( isoYear, isoMonth, isoDay [ , calendar ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let iso_year = vm.argument(0);
        let iso_month = vm.argument(1);
        let iso_day = vm.argument(2);
        let calendar_value = vm.argument(3);

        // 2. Let y be ? ToIntegerWithTruncation(isoYear).
        let year = to_integer_with_truncation(vm, iso_year, ErrorType::TemporalInvalidPlainDate, &[])?;

        // 3. Let m be ? ToIntegerWithTruncation(isoMonth).
        let month = to_integer_with_truncation(vm, iso_month, ErrorType::TemporalInvalidPlainDate, &[])?;

        // 4. Let d be ? ToIntegerWithTruncation(isoDay).
        let day = to_integer_with_truncation(vm, iso_day, ErrorType::TemporalInvalidPlainDate, &[])?;

        // 5. If calendar is undefined, set calendar to "iso8601".
        // 6. If calendar is not a String, throw a TypeError exception.
        // 7. Set calendar to ? CanonicalizeCalendar(calendar).
        let calendar = if calendar_value.is_undefined() {
            canonicalize_calendar(vm, ascii_view(ISO8601_CALENDAR))?
        } else if !calendar_value.is_string() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAString, &[&"calendar"]);
        } else {
            let calendar_string = calendar_value.as_string().utf16_string();
            canonicalize_calendar(vm, Utf16View::of_string(&calendar_string))?
        };

        // 8. If IsValidISODate(y, m, d) is false, throw a RangeError exception.
        if !is_valid_iso_date(year, month, day) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainDate, &[]);
        }

        // 9. Let isoDate be CreateISODateRecord(y, m, d).
        let iso_date = create_iso_date_record(year, month, day);

        // 10. Return ? CreateTemporalDate(isoDate, calendar, NewTarget).
        Ok(create_temporal_date(vm, iso_date, calendar, Some(new_target))?.upcast())
    }

    // 3.2.2 3.2.2 Temporal.PlainDate.from ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.from
    fn from(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? ToTemporalDate(item, options).
        Ok(Value::from_object(to_temporal_date(
            vm,
            vm.argument(0),
            vm.argument(1),
        )?))
    }

    // 3.2.3 Temporal.PlainDate.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.plaindate.compare
    fn compare(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Set one to ? ToTemporalDate(one).
        let one = to_temporal_date(vm, vm.argument(0), Value::UNDEFINED)?;

        // 2. Set two to ? ToTemporalDate(two).
        let two = to_temporal_date(vm, vm.argument(1), Value::UNDEFINED)?;

        // 3. Return 𝔽(CompareISODate(one.[[ISODate]], two.[[ISODate]])).
        Ok(Value::from_i32(i32::from(compare_iso_date(
            one.iso_date(),
            two.iso_date(),
        ))))
    }
}
