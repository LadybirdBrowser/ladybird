/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::ordinary_create_from_constructor_of;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::date::{Date, make_date, make_day, make_time, time_clip, utc_time};
use crate::runtime::date_parser::DateParser;
use crate::runtime::date_prototype::{this_time_value, to_date_string};
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::temporal::now::system_utc_epoch_milliseconds;
use crate::runtime::value::PreferredType;
use crate::runtime::value_conversions::to_integer_or_infinity;
use crate::utf16::Utf16View;

fn parse_date_string(vm: &Vm, date_string: Utf16View<'_>) -> f64 {
    let result = DateParser::parse(date_string);
    if result.is_nan() {
        // NB: The view can be into the string an unresolved substring was taken from, which a collection may free
        //     once the hook runs code that resolves the substring.
        let date_string = date_string.to_utf16_string();
        (vm.host_unrecognized_date_string())(vm, Utf16View::of_string(&date_string));
    }
    result
}

#[repr(C)]
#[derive(Trace)]
pub struct DateConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    DateConstructor,
    initialize: DateConstructor::initialize,
    call: DateConstructor::call,
    construct: DateConstructor::construct
);

/// The argument at `index` as a Number, or `fallback` if there is no such argument.
fn argument_or(vm: &Vm, index: usize, fallback: f64) -> ThrowCompletionOr<f64> {
    if vm.argument_count() > index {
        return Ok(vm.argument(index).to_number(vm)?.as_f64());
    }
    Ok(fallback)
}

impl DateConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<DateConstructor> {
        realm.create_object(
            vm,
            DateConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Date.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 21.4.3.3 Date.prototype, https://tc39.es/ecma262/#sec-date.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().date_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };
        define_native_function(&names.now, raw_native!(DateConstructor::now), 0);
        define_native_function(&names.parse, raw_native!(DateConstructor::parse), 1);
        define_native_function(&names.UTC, raw_native!(DateConstructor::utc), 7);

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(7),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 21.4.2.1 Date ( ...values ), https://tc39.es/ecma262/#sec-date
    // 14.6.1 Date ( ...values ), https://tc39.es/proposal-temporal/#sec-temporal-date
    #[allow(clippy::unnecessary_wraps, reason = "[[Call]] can throw for other functions")]
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. If NewTarget is undefined, return ToDateString(SystemUTCEpochMilliseconds()).
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            to_date_string(system_utc_epoch_milliseconds()),
        )))
    }

    // 21.4.2.1 Date ( ...values ), https://tc39.es/ecma262/#sec-date
    // 14.6.1 Date ( ...values ), https://tc39.es/proposal-temporal/#sec-temporal-date
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");

        // 2. Let numberOfArgs be the number of elements in values.
        let date_value = match vm.argument_count() {
            // 3. If numberOfArgs = 0, then
            0 => {
                // a. Let dv be SystemUTCEpochMilliseconds().
                system_utc_epoch_milliseconds()
            }
            // 4. Else if numberOfArgs = 1, then
            1 => {
                // a. Let value be values[0].
                let value = vm.argument(0);

                // b. If Type(value) is Object and value has a [[DateValue]] internal slot, then
                let time_value = if value.is_object() && value.as_object().is::<Date>() {
                    // i. Let tv be ! thisTimeValue(value).
                    this_time_value(vm, value).must()
                }
                // c. Else,
                else {
                    // i. Let v be ? ToPrimitive(value).
                    let primitive = value.to_primitive(vm, PreferredType::Default)?;

                    // ii. If Type(v) is String, then
                    if primitive.is_string() {
                        // 1. Assert: The next step never returns an abrupt completion because Type(v) is String.
                        // 2. Let tv be the result of parsing v as a date, in exactly the same manner as for the parse method (21.4.3.2).
                        parse_date_string(vm, primitive.as_string().utf16_string_view())
                    }
                    // iii. Else,
                    else {
                        // 1. Let tv be ? ToNumber(v).
                        primitive.to_number(vm)?.as_f64()
                    }
                };

                // d. Let dv be TimeClip(tv).
                time_clip(time_value)
            }
            // 5. Else,
            _ => {
                // a. Assert: numberOfArgs ≥ 2.
                // b. Let y be ? ToNumber(values[0]).
                let mut year = vm.argument(0).to_number(vm)?.as_f64();
                // c. Let m be ? ToNumber(values[1]).
                let month = vm.argument(1).to_number(vm)?.as_f64();

                // d. If numberOfArgs > 2, let dt be ? ToNumber(values[2]); else let dt be 1𝔽.
                let date = argument_or(vm, 2, 1.0)?;
                // e. If numberOfArgs > 3, let h be ? ToNumber(values[3]); else let h be +0𝔽.
                let hours = argument_or(vm, 3, 0.0)?;
                // f. If numberOfArgs > 4, let min be ? ToNumber(values[4]); else let min be +0𝔽.
                let minutes = argument_or(vm, 4, 0.0)?;
                // g. If numberOfArgs > 5, let s be ? ToNumber(values[5]); else let s be +0𝔽.
                let seconds = argument_or(vm, 5, 0.0)?;
                // h. If numberOfArgs > 6, let milli be ? ToNumber(values[6]); else let milli be +0𝔽.
                let milliseconds = argument_or(vm, 6, 0.0)?;

                // i. If y is NaN, let yr be NaN.
                // j. Else,
                if !year.is_nan() {
                    // i. Let yi be ! ToIntegerOrInfinity(y).
                    let year_integer = to_integer_or_infinity(year);

                    // ii. If 0 ≤ yi ≤ 99, let yr be 1900𝔽 + 𝔽(yi); otherwise, let yr be y.
                    if (0.0..=99.0).contains(&year_integer) {
                        year = 1900.0 + year_integer;
                    }
                }

                // k. Let finalDate be MakeDate(MakeDay(yr, m, dt), MakeTime(h, min, s, milli)).
                let day = make_day(year, month, date);
                let time = make_time(hours, minutes, seconds, milliseconds);
                let final_date = make_date(day, time);

                // l. Let dv be TimeClip(UTC(finalDate)).
                time_clip(utc_time(final_date))
            }
        };

        // 6. Let O be ? OrdinaryCreateFromConstructor(NewTarget, "%Date.prototype%", « [[DateValue]] »).
        // 7. Set O.[[DateValue]] to dv.
        // 8. Return O.
        Ok(
            ordinary_create_from_constructor_of(vm, realm, new_target, Intrinsics::date_prototype, |prototype| {
                Date::new(vm, date_value, prototype)
            })?
            .upcast(),
        )
    }

    // 21.4.3.1 Date.now ( ), https://tc39.es/ecma262/#sec-date.now
    // 14.7.1 Date.now ( ), https://tc39.es/proposal-temporal/#sec-temporal-date.now
    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn now(_: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return SystemUTCEpochMilliseconds().
        Ok(Value::from_f64(system_utc_epoch_milliseconds()))
    }

    // 21.4.3.2 Date.parse ( string ), https://tc39.es/ecma262/#sec-date.parse
    fn parse(vm: &Vm) -> ThrowCompletionOr<Value> {
        if vm.argument_count() == 0 {
            return Ok(Value::from_f64(f64::NAN));
        }

        // This function applies the ToString operator to its argument. If ToString results in an abrupt completion the
        // Completion Record is immediately returned.
        let date_string = vm.argument(0).to_utf16_string(vm)?;

        // Otherwise, this function interprets the resulting String as a date and time; it returns a Number, the UTC time
        // value corresponding to the date and time.
        Ok(Value::from_f64(parse_date_string(
            vm,
            Utf16View::of_string(&date_string),
        )))
    }

    // 21.4.3.4 Date.UTC ( year [ , month [ , date [ , hours [ , minutes [ , seconds [ , ms ] ] ] ] ] ] ), https://tc39.es/ecma262/#sec-date.utc
    fn utc(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let y be ? ToNumber(year).
        let mut year = vm.argument(0).to_number(vm)?.as_f64();
        // 2. If month is present, let m be ? ToNumber(month); else let m be +0𝔽.
        let month = argument_or(vm, 1, 0.0)?;
        // 3. If date is present, let dt be ? ToNumber(date); else let dt be 1𝔽.
        let date = argument_or(vm, 2, 1.0)?;
        // 4. If hours is present, let h be ? ToNumber(hours); else let h be +0𝔽.
        let hours = argument_or(vm, 3, 0.0)?;
        // 5. If minutes is present, let min be ? ToNumber(minutes); else let min be +0𝔽.
        let minutes = argument_or(vm, 4, 0.0)?;
        // 6. If seconds is present, let s be ? ToNumber(seconds); else let s be +0𝔽.
        let seconds = argument_or(vm, 5, 0.0)?;
        // 7. If ms is present, let milli be ? ToNumber(ms); else let milli be +0𝔽.
        let milliseconds = argument_or(vm, 6, 0.0)?;

        // 8. If y is NaN, let yr be NaN.
        // 9. Else,
        if !year.is_nan() {
            // a. Let yi be ! ToIntegerOrInfinity(y).
            let year_integer = to_integer_or_infinity(year);

            // b. If 0 ≤ yi ≤ 99, let yr be 1900𝔽 + 𝔽(yi); otherwise, let yr be y.
            if (0.0..=99.0).contains(&year_integer) {
                year = 1900.0 + year_integer;
            }
        }

        // 10. Return TimeClip(MakeDate(MakeDay(yr, m, dt), MakeTime(h, min, s, milli))).
        let day = make_day(year, month, date);
        let time = make_time(hours, minutes, seconds, milliseconds);
        Ok(Value::from_f64(time_clip(make_date(day, time))))
    }
}
