/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::big_int::number_to_bigint;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::date::{
    Date, MS_PER_MINUTE, TimeStyle, date_from_time, day, format_offset_time_zone_identifier, format_time_string,
    get_named_time_zone_offset_milliseconds, hour_from_time, local_time, make_date, make_day, make_time, min_from_time,
    month_from_time, ms_from_time, parse_time_zone_identifier, sec_from_time, system_time_zone_identifier, time_clip,
    time_within_day, utc_time, week_day, year_from_time,
};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::{typed_this_object, typed_this_value};
use crate::runtime::realm::Realm;
use crate::runtime::value::PreferredType;
use crate::runtime::value_conversions::to_integer_or_infinity;
use crate::unicode::display_names::time_zone_display_name;
use crate::unicode::intl::default_locale;
use crate::unicode::time_zone::{InDST, current_time_zone};
use crate::utf16::{Utf16StringBuilder, Utf16View};

/// %Date.prototype%, which is an ordinary object.
#[repr(C)]
#[derive(Trace)]
pub struct DatePrototype {
    base: Object,
}

define_object_class!(DatePrototype, extends: [Object], methods: {
    initialize: DatePrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

const SHORT_DAY_NAMES: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const SHORT_MONTH_NAMES: [&str; 12] = [
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
];

fn nan() -> Value {
    Value::from_f64(f64::NAN)
}

fn invalid_date(vm: &Vm) -> Value {
    Value::from_string(PrimitiveString::create_from_utf8(vm, "Invalid Date"))
}

/// The Date this value, which thisTimeValue() has already checked is a Date.
fn this_date(vm: &Vm) -> Gc<Date> {
    vm.this_value()
        .as_object()
        .downcast::<Date>()
        .expect("thisTimeValue() checked that this value is a Date")
}

// thisTimeValue ( value ), https://tc39.es/ecma262/#thistimevalue
pub fn this_time_value(vm: &Vm, value: Value) -> ThrowCompletionOr<f64> {
    // 1. If Type(value) is Object and value has a [[DateValue]] internal slot, then
    if value.is_object()
        && let Some(date) = value.as_object().downcast::<Date>()
    {
        // a. Return value.[[DateValue]].
        return Ok(date.date_value());
    }

    // 2. Throw a TypeError exception.
    vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&"Date"])
}

fn argument_or_number(vm: &Vm, index: usize, fallback: f64) -> ThrowCompletionOr<f64> {
    if vm.argument_count() > index {
        return Ok(vm.argument(index).to_number(vm)?.as_f64());
    }

    Ok(fallback)
}

fn argument_or_empty(vm: &Vm, index: usize) -> ThrowCompletionOr<Option<f64>> {
    if vm.argument_count() > index {
        return Ok(Some(vm.argument(index).to_number(vm)?.as_f64()));
    }

    Ok(None)
}

impl DatePrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<DatePrototype> {
        realm.create_object(
            vm,
            DatePrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };

        define_native_function(&names.getDate, raw_native!(DatePrototype::get_date), 0);
        define_native_function(&names.getDay, raw_native!(DatePrototype::get_day), 0);
        define_native_function(&names.getFullYear, raw_native!(DatePrototype::get_full_year), 0);
        define_native_function(&names.getHours, raw_native!(DatePrototype::get_hours), 0);
        define_native_function(&names.getMilliseconds, raw_native!(DatePrototype::get_milliseconds), 0);
        define_native_function(&names.getMinutes, raw_native!(DatePrototype::get_minutes), 0);
        define_native_function(&names.getMonth, raw_native!(DatePrototype::get_month), 0);
        define_native_function(&names.getSeconds, raw_native!(DatePrototype::get_seconds), 0);
        define_native_function(&names.getTime, raw_native!(DatePrototype::get_time), 0);
        define_native_function(
            &names.getTimezoneOffset,
            raw_native!(DatePrototype::get_timezone_offset),
            0,
        );
        define_native_function(&names.getUTCDate, raw_native!(DatePrototype::get_utc_date), 0);
        define_native_function(&names.getUTCDay, raw_native!(DatePrototype::get_utc_day), 0);
        define_native_function(&names.getUTCFullYear, raw_native!(DatePrototype::get_utc_full_year), 0);
        define_native_function(&names.getUTCHours, raw_native!(DatePrototype::get_utc_hours), 0);
        define_native_function(
            &names.getUTCMilliseconds,
            raw_native!(DatePrototype::get_utc_milliseconds),
            0,
        );
        define_native_function(&names.getUTCMinutes, raw_native!(DatePrototype::get_utc_minutes), 0);
        define_native_function(&names.getUTCMonth, raw_native!(DatePrototype::get_utc_month), 0);
        define_native_function(&names.getUTCSeconds, raw_native!(DatePrototype::get_utc_seconds), 0);
        define_native_function(&names.setDate, raw_native!(DatePrototype::set_date), 1);
        define_native_function(&names.setFullYear, raw_native!(DatePrototype::set_full_year), 3);
        define_native_function(&names.setHours, raw_native!(DatePrototype::set_hours), 4);
        define_native_function(&names.setMilliseconds, raw_native!(DatePrototype::set_milliseconds), 1);
        define_native_function(&names.setMinutes, raw_native!(DatePrototype::set_minutes), 3);
        define_native_function(&names.setMonth, raw_native!(DatePrototype::set_month), 2);
        define_native_function(&names.setSeconds, raw_native!(DatePrototype::set_seconds), 2);
        define_native_function(&names.setTime, raw_native!(DatePrototype::set_time), 1);
        define_native_function(&names.setUTCDate, raw_native!(DatePrototype::set_utc_date), 1);
        define_native_function(&names.setUTCFullYear, raw_native!(DatePrototype::set_utc_full_year), 3);
        define_native_function(&names.setUTCHours, raw_native!(DatePrototype::set_utc_hours), 4);
        define_native_function(
            &names.setUTCMilliseconds,
            raw_native!(DatePrototype::set_utc_milliseconds),
            1,
        );
        define_native_function(&names.setUTCMinutes, raw_native!(DatePrototype::set_utc_minutes), 3);
        define_native_function(&names.setUTCMonth, raw_native!(DatePrototype::set_utc_month), 2);
        define_native_function(&names.setUTCSeconds, raw_native!(DatePrototype::set_utc_seconds), 2);
        define_native_function(&names.toDateString, raw_native!(DatePrototype::to_date_string), 0);
        define_native_function(&names.toISOString, raw_native!(DatePrototype::to_iso_string), 0);
        define_native_function(&names.toJSON, raw_native!(DatePrototype::to_json), 1);
        define_native_function(
            &names.toLocaleDateString,
            raw_native!(DatePrototype::to_locale_date_string),
            0,
        );
        define_native_function(&names.toLocaleString, raw_native!(DatePrototype::to_locale_string), 0);
        define_native_function(
            &names.toLocaleTimeString,
            raw_native!(DatePrototype::to_locale_time_string),
            0,
        );
        define_native_function(&names.toString, raw_native!(DatePrototype::to_string), 0);
        define_native_function(&names.toTimeString, raw_native!(DatePrototype::to_time_string), 0);
        define_native_function(&names.toUTCString, raw_native!(DatePrototype::to_utc_string), 0);

        define_native_function(
            &names.toTemporalInstant,
            raw_native!(DatePrototype::to_temporal_instant),
            0,
        );

        define_native_function(&names.getYear, raw_native!(DatePrototype::get_year), 0);
        define_native_function(&names.setYear, raw_native!(DatePrototype::set_year), 1);

        // 21.4.4.45 Date.prototype [ @@toPrimitive ] ( hint ), https://tc39.es/ecma262/#sec-date.prototype-@@toprimitive
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().to_primitive),
            raw_native!(DatePrototype::symbol_to_primitive),
            1,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
            None,
        );

        // Aliases.
        define_native_function(&names.valueOf, raw_native!(DatePrototype::get_time), 0);

        // B.2.4.3 Date.prototype.toGMTString ( ), https://tc39.es/ecma262/#sec-date.prototype.togmtstring
        // The initial value of the "toGMTString" property is %Date.prototype.toUTCString%, defined in 21.4.4.43.
        object.define_direct_property(
            vm,
            &names.toGMTString,
            object.get_without_side_effects(vm, &names.toUTCString),
            attr,
        );
    }

    // 21.4.4.2 Date.prototype.getDate ( ), https://tc39.es/ecma262/#sec-date.prototype.getdate
    fn get_date(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return DateFromTime(LocalTime(t)).
        Ok(Value::from_i32(i32::from(date_from_time(local_time(time)))))
    }

    // 21.4.4.3 Date.prototype.getDay ( ), https://tc39.es/ecma262/#sec-date.prototype.getday
    fn get_day(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return WeekDay(LocalTime(t)).
        Ok(Value::from_i32(i32::from(week_day(local_time(time)))))
    }

    // 21.4.4.4 Date.prototype.getFullYear ( ), https://tc39.es/ecma262/#sec-date.prototype.getfullyear
    fn get_full_year(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return YearFromTime(LocalTime(t)).
        Ok(Value::from_i32(year_from_time(local_time(time))))
    }

    // 21.4.4.5 Date.prototype.getHours ( ), https://tc39.es/ecma262/#sec-date.prototype.gethours
    fn get_hours(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return HourFromTime(LocalTime(t)).
        Ok(Value::from_i32(i32::from(hour_from_time(local_time(time)))))
    }

    // 21.4.4.6 Date.prototype.getMilliseconds ( ), https://tc39.es/ecma262/#sec-date.prototype.getmilliseconds
    fn get_milliseconds(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return msFromTime(LocalTime(t)).
        Ok(Value::from_i32(i32::from(ms_from_time(local_time(time)))))
    }

    // 21.4.4.7 Date.prototype.getMinutes ( ), https://tc39.es/ecma262/#sec-date.prototype.getminutes
    fn get_minutes(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return MinFromTime(LocalTime(t)).
        Ok(Value::from_i32(i32::from(min_from_time(local_time(time)))))
    }

    // 21.4.4.8 Date.prototype.getMonth ( ), https://tc39.es/ecma262/#sec-date.prototype.getmonth
    fn get_month(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return MonthFromTime(LocalTime(t)).
        Ok(Value::from_i32(i32::from(month_from_time(local_time(time)))))
    }

    // 21.4.4.9 Date.prototype.getSeconds ( ), https://tc39.es/ecma262/#sec-date.prototype.getseconds
    fn get_seconds(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return SecFromTime(LocalTime(t)).
        Ok(Value::from_i32(i32::from(sec_from_time(local_time(time)))))
    }

    // 21.4.4.10 Date.prototype.getTime ( ), https://tc39.es/ecma262/#sec-date.prototype.gettime
    fn get_time(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Return ? thisTimeValue(this value).
        Ok(Value::from_f64(this_time_value(vm, vm.this_value())?))
    }

    // 21.4.4.11 Date.prototype.getTimezoneOffset ( ), https://tc39.es/ecma262/#sec-date.prototype.gettimezoneoffset
    fn get_timezone_offset(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return (t - LocalTime(t)) / msPerMinute.
        Ok(Value::from_f64((time - local_time(time)) / MS_PER_MINUTE))
    }

    // 21.4.4.12 Date.prototype.getUTCDate ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcdate
    fn get_utc_date(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return DateFromTime(t).
        Ok(Value::from_i32(i32::from(date_from_time(time))))
    }

    // 21.4.4.13 Date.prototype.getUTCDay ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcday
    fn get_utc_day(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return WeekDay(t).
        Ok(Value::from_i32(i32::from(week_day(time))))
    }

    // 21.4.4.14 Date.prototype.getUTCFullYear ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcfullyear
    fn get_utc_full_year(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return YearFromTime(t).
        Ok(Value::from_i32(year_from_time(time)))
    }

    // 21.4.4.15 Date.prototype.getUTCHours ( ), https://tc39.es/ecma262/#sec-date.prototype.getutchours
    fn get_utc_hours(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return HourFromTime(t).
        Ok(Value::from_i32(i32::from(hour_from_time(time))))
    }

    // 21.4.4.16 Date.prototype.getUTCMilliseconds ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcmilliseconds
    fn get_utc_milliseconds(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return msFromTime(t).
        Ok(Value::from_i32(i32::from(ms_from_time(time))))
    }

    // 21.4.4.17 Date.prototype.getUTCMinutes ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcminutes
    fn get_utc_minutes(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return MinFromTime(t).
        Ok(Value::from_i32(i32::from(min_from_time(time))))
    }

    // 21.4.4.18 Date.prototype.getUTCMonth ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcmonth
    fn get_utc_month(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return MonthFromTime(t).
        Ok(Value::from_i32(i32::from(month_from_time(time))))
    }

    // 21.4.4.19 Date.prototype.getUTCSeconds ( ), https://tc39.es/ecma262/#sec-date.prototype.getutcseconds
    fn get_utc_seconds(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return SecFromTime(t).
        Ok(Value::from_i32(i32::from(sec_from_time(time))))
    }

    // 21.4.4.20 Date.prototype.setDate ( date ), https://tc39.es/ecma262/#sec-date.prototype.setdate
    fn set_date(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let this_time = this_time_value(vm, vm.this_value())?;

        // 2. Let dt be ? ToNumber(date).
        let date = vm.argument(0).to_number(vm)?.as_f64();

        // 3. If t is NaN, return NaN.
        if this_time.is_nan() {
            return Ok(nan());
        }

        // 4. Set t to LocalTime(t).
        let time = local_time(this_time);

        // 5. Let newDate be MakeDate(MakeDay(YearFromTime(t), MonthFromTime(t), dt), TimeWithinDay(t)).
        let year = year_from_time(time);
        let month = month_from_time(time);

        let day = make_day(f64::from(year), f64::from(month), date);
        let mut new_date = make_date(day, time_within_day(time));

        // 6. Let u be TimeClip(UTC(newDate)).
        new_date = time_clip(utc_time(new_date));

        // 7. Set the [[DateValue]] internal slot of this Date object to u.
        this_date(vm).set_date_value(new_date);

        // 8. Return u.
        Ok(Value::from_f64(new_date))
    }

    // 21.4.4.21 Date.prototype.setFullYear ( year [ , month [ , date ] ] ), https://tc39.es/ecma262/#sec-date.prototype.setfullyear
    fn set_full_year(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let this_time = this_time_value(vm, vm.this_value())?;

        // 2. Let y be ? ToNumber(year).
        let year = vm.argument(0).to_number(vm)?.as_f64();

        // 3. If t is NaN, set t to +0𝔽; otherwise, set t to LocalTime(t).
        let time = if this_time.is_nan() { 0.0 } else { local_time(this_time) };

        // 4. If month is not present, let m be MonthFromTime(t); otherwise, let m be ? ToNumber(month).
        let month = argument_or_number(vm, 1, f64::from(month_from_time(time)))?;

        // 5. If date is not present, let dt be DateFromTime(t); otherwise, let dt be ? ToNumber(date).
        let date = argument_or_number(vm, 2, f64::from(date_from_time(time)))?;

        // 6. Let newDate be MakeDate(MakeDay(y, m, dt), TimeWithinDay(t)).
        let day = make_day(year, month, date);
        let mut new_date = make_date(day, time_within_day(time));

        // 7. Let u be TimeClip(UTC(newDate)).
        new_date = time_clip(utc_time(new_date));

        // 8. Set the [[DateValue]] internal slot of this Date object to u.
        this_date(vm).set_date_value(new_date);

        // 9. Return u.
        Ok(Value::from_f64(new_date))
    }

    // 21.4.4.22 Date.prototype.setHours ( hour [ , min [ , sec [ , ms ] ] ] ), https://tc39.es/ecma262/#sec-date.prototype.sethours
    fn set_hours(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be LocalTime(? thisTimeValue(this value)).
        let this_time = this_time_value(vm, vm.this_value())?;

        // 2. Let h be ? ToNumber(hour).
        let hour = vm.argument(0).to_number(vm)?.as_f64();

        // 3. If min is present, let m be ? ToNumber(min).
        let minute = argument_or_empty(vm, 1)?;

        // 4. If sec is present, let s be ? ToNumber(sec).
        let second = argument_or_empty(vm, 2)?;

        // 5. If ms is present, let milli be ? ToNumber(ms).
        let millisecond = argument_or_empty(vm, 3)?;

        // 6. If t is NaN, return NaN.
        if this_time.is_nan() {
            return Ok(nan());
        }

        // 7. Set t to LocalTime(t).
        let time = local_time(this_time);

        // 8. If min is not present, let m be MinFromTime(t).
        let minute = minute.unwrap_or_else(|| f64::from(min_from_time(time)));

        // 9. If sec is not present, let s be SecFromTime(t).
        let second = second.unwrap_or_else(|| f64::from(sec_from_time(time)));

        // 10. If ms is not present, let milli be msFromTime(t).
        let millisecond = millisecond.unwrap_or_else(|| f64::from(ms_from_time(time)));

        // 11. Let date be MakeDate(Day(t), MakeTime(h, m, s, milli)).
        let new_time = make_time(hour, minute, second, millisecond);
        let mut date = make_date(day(time), new_time);

        // 12. Let u be TimeClip(UTC(date)).
        date = time_clip(utc_time(date));

        // 13. Set the [[DateValue]] internal slot of this Date object to u.
        this_date(vm).set_date_value(date);

        // 14. Return u.
        Ok(Value::from_f64(date))
    }

    // 21.4.4.23 Date.prototype.setMilliseconds ( ms ), https://tc39.es/ecma262/#sec-date.prototype.setmilliseconds
    fn set_milliseconds(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let this_time = this_time_value(vm, vm.this_value())?;

        // 2. Set ms to ? ToNumber(ms).
        let millisecond = vm.argument(0).to_number(vm)?.as_f64();

        // 3. If t is NaN, return NaN.
        if this_time.is_nan() {
            return Ok(nan());
        }

        // 4. Set t to LocalTime(t).
        let time = local_time(this_time);

        // 5. Let time be MakeTime(HourFromTime(t), MinFromTime(t), SecFromTime(t), ms).
        let hour = hour_from_time(time);
        let minute = min_from_time(time);
        let second = sec_from_time(time);

        let new_time = make_time(f64::from(hour), f64::from(minute), f64::from(second), millisecond);

        // 6. Let u be TimeClip(UTC(MakeDate(Day(t), time))).
        let mut date = make_date(day(time), new_time);
        date = time_clip(utc_time(date));

        // 7. Set the [[DateValue]] internal slot of this Date object to u.
        this_date(vm).set_date_value(date);

        // 8. Return u.
        Ok(Value::from_f64(date))
    }

    // 21.4.4.24 Date.prototype.setMinutes ( min [ , sec [ , ms ] ] ), https://tc39.es/ecma262/#sec-date.prototype.setminutes
    fn set_minutes(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let this_time = this_time_value(vm, vm.this_value())?;

        // 2. Let m be ? ToNumber(min).
        let minute = vm.argument(0).to_number(vm)?.as_f64();

        // 3. If sec is present, let s be ? ToNumber(sec).
        let second = argument_or_empty(vm, 1)?;

        // 4. If ms is present, let milli be ? ToNumber(ms).
        let millisecond = argument_or_empty(vm, 2)?;

        // 5. If t is NaN, return NaN.
        if this_time.is_nan() {
            return Ok(nan());
        }

        // 6. Set t to LocalTime(t).
        let time = local_time(this_time);

        // 7. If sec is not present, let s be SecFromTime(t).
        let second = second.unwrap_or_else(|| f64::from(sec_from_time(time)));

        // 8. If ms is not present, let milli be msFromTime(t).
        let millisecond = millisecond.unwrap_or_else(|| f64::from(ms_from_time(time)));

        // 9. Let date be MakeDate(Day(t), MakeTime(HourFromTime(t), m, s, milli)).
        let hour = hour_from_time(time);

        let new_time = make_time(f64::from(hour), minute, second, millisecond);
        let mut date = make_date(day(time), new_time);

        // 10. Let u be TimeClip(UTC(date)).
        date = time_clip(utc_time(date));

        // 11. Set the [[DateValue]] internal slot of this Date object to u.
        this_date(vm).set_date_value(date);

        // 12. Return u.
        Ok(Value::from_f64(date))
    }

    // 21.4.4.25 Date.prototype.setMonth ( month [ , date ] ), https://tc39.es/ecma262/#sec-date.prototype.setmonth
    fn set_month(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let this_time = this_time_value(vm, vm.this_value())?;

        // 2. Let m be ? ToNumber(month).
        let month = vm.argument(0).to_number(vm)?.as_f64();

        // 3. If date is present, let dt be ? ToNumber(date).
        let date = argument_or_empty(vm, 1)?;

        // 4. If t is NaN, return NaN.
        if this_time.is_nan() {
            return Ok(nan());
        }

        // 5. Set t to LocalTime(t).
        let time = local_time(this_time);

        // 6. If date is not present, let dt be DateFromTime(t).
        let date = date.unwrap_or_else(|| f64::from(date_from_time(time)));

        // 7. Let newDate be MakeDate(MakeDay(YearFromTime(t), m, dt), TimeWithinDay(t)).
        let year = year_from_time(time);

        let day = make_day(f64::from(year), month, date);
        let mut new_date = make_date(day, time_within_day(time));

        // 8. Let u be TimeClip(UTC(newDate)).
        new_date = time_clip(utc_time(new_date));

        // 9. Set the [[DateValue]] internal slot of this Date object to u.
        this_date(vm).set_date_value(new_date);

        // 10. Return u.
        Ok(Value::from_f64(new_date))
    }

    // 21.4.4.26 Date.prototype.setSeconds ( sec [ , ms ] ), https://tc39.es/ecma262/#sec-date.prototype.setseconds
    fn set_seconds(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let this_time = this_time_value(vm, vm.this_value())?;

        // 2. Let s be ? ToNumber(sec).
        let second = vm.argument(0).to_number(vm)?.as_f64();

        // 3. If ms is present, let milli be ? ToNumber(ms).
        let millisecond = argument_or_empty(vm, 1)?;

        // 4. If t is NaN, return NaN.
        if this_time.is_nan() {
            return Ok(nan());
        }

        // 5. Set t to LocalTime(t).
        let time = local_time(this_time);

        // 6. If ms is not present, let milli be msFromTime(t).
        let millisecond = millisecond.unwrap_or_else(|| f64::from(ms_from_time(time)));

        // 7. Let date be MakeDate(Day(t), MakeTime(HourFromTime(t), MinFromTime(t), s, milli)).
        let hour = hour_from_time(time);
        let minute = min_from_time(time);

        let new_time = make_time(f64::from(hour), f64::from(minute), second, millisecond);
        let mut new_date = make_date(day(time), new_time);

        // 8. Let u be TimeClip(UTC(date)).
        new_date = time_clip(utc_time(new_date));

        // 9. Set the [[DateValue]] internal slot of this Date object to u.
        this_date(vm).set_date_value(new_date);

        // 10. Return u.
        Ok(Value::from_f64(new_date))
    }

    // 21.4.4.27 Date.prototype.setTime ( time ), https://tc39.es/ecma262/#sec-date.prototype.settime
    fn set_time(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Perform ? thisTimeValue(this value).
        this_time_value(vm, vm.this_value())?;

        // 2. Let t be ? ToNumber(time).
        let mut time = vm.argument(0).to_number(vm)?.as_f64();

        // 3. Let v be TimeClip(t).
        time = time_clip(time);

        // 4. Set the [[DateValue]] internal slot of this Date object to v.
        this_date(vm).set_date_value(time);

        // 5. Return v.
        Ok(Value::from_f64(time))
    }

    // 21.4.4.28 Date.prototype.setUTCDate ( date ), https://tc39.es/ecma262/#sec-date.prototype.setutcdate
    fn set_utc_date(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. Let dt be ? ToNumber(date).
        let date = vm.argument(0).to_number(vm)?.as_f64();

        // 3. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 4. Let newDate be MakeDate(MakeDay(YearFromTime(t), MonthFromTime(t), dt), TimeWithinDay(t)).
        let year = year_from_time(time);
        let month = month_from_time(time);

        let day = make_day(f64::from(year), f64::from(month), date);
        let mut new_date = make_date(day, time_within_day(time));

        // 5. Let v be TimeClip(newDate).
        new_date = time_clip(new_date);

        // 6. Set the [[DateValue]] internal slot of this Date object to v.
        this_date(vm).set_date_value(new_date);

        // 7. Return v.
        Ok(Value::from_f64(new_date))
    }

    // 21.4.4.29 Date.prototype.setUTCFullYear ( year [ , month [ , date ] ] ), https://tc39.es/ecma262/#sec-date.prototype.setutcfullyear
    fn set_utc_full_year(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let this_time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, set t to +0𝔽.
        let time = if this_time.is_nan() { 0.0 } else { this_time };

        // 3. Let y be ? ToNumber(year).
        let year = vm.argument(0).to_number(vm)?.as_f64();

        // 4. If month is not present, let m be MonthFromTime(t); otherwise, let m be ? ToNumber(month).
        let month = argument_or_number(vm, 1, f64::from(month_from_time(time)))?;

        // 5. If date is not present, let dt be DateFromTime(t); otherwise, let dt be ? ToNumber(date).
        let date = argument_or_number(vm, 2, f64::from(date_from_time(time)))?;

        // 6. Let newDate be MakeDate(MakeDay(y, m, dt), TimeWithinDay(t)).
        let day = make_day(year, month, date);
        let mut new_date = make_date(day, time_within_day(time));

        // 7. Let v be TimeClip(newDate).
        new_date = time_clip(new_date);

        // 8. Set the [[DateValue]] internal slot of this Date object to v.
        this_date(vm).set_date_value(new_date);

        // 9. Return v.
        Ok(Value::from_f64(new_date))
    }

    // 21.4.4.30 Date.prototype.setUTCHours ( hour [ , min [ , sec [ , ms ] ] ] ), https://tc39.es/ecma262/#sec-date.prototype.setutchours
    fn set_utc_hours(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. Let h be ? ToNumber(hour).
        let hour = vm.argument(0).to_number(vm)?.as_f64();

        // 3. If min is present, let m be ? ToNumber(min).
        let minute = argument_or_empty(vm, 1)?;

        // 4. If sec is present, let s be ? ToNumber(sec).
        let second = argument_or_empty(vm, 2)?;

        // 5. If ms is present, let milli be ? ToNumber(ms).
        let millisecond = argument_or_empty(vm, 3)?;

        // 6. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 7. If min is not present, let m be MinFromTime(t).
        let minute = minute.unwrap_or_else(|| f64::from(min_from_time(time)));

        // 8. If sec is not present, let s be SecFromTime(t).
        let second = second.unwrap_or_else(|| f64::from(sec_from_time(time)));

        // 9. If ms is not present, let milli be msFromTime(t).
        let millisecond = millisecond.unwrap_or_else(|| f64::from(ms_from_time(time)));

        // 10. Let date be MakeDate(Day(t), MakeTime(h, m, s, milli)).
        let new_time = make_time(hour, minute, second, millisecond);
        let mut date = make_date(day(time), new_time);

        // 11. Let v be TimeClip(date).
        date = time_clip(date);

        // 12. Set the [[DateValue]] internal slot of this Date object to v.
        this_date(vm).set_date_value(date);

        // 13. Return v.
        Ok(Value::from_f64(date))
    }

    // 21.4.4.31 Date.prototype.setUTCMilliseconds ( ms ), https://tc39.es/ecma262/#sec-date.prototype.setutcmilliseconds
    fn set_utc_milliseconds(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. Set ms to ? ToNumber(ms).
        let millisecond = vm.argument(0).to_number(vm)?.as_f64();

        // 3. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 4. Let time be MakeTime(HourFromTime(t), MinFromTime(t), SecFromTime(t), ms).
        let hour = hour_from_time(time);
        let minute = min_from_time(time);
        let second = sec_from_time(time);

        let new_time = make_time(f64::from(hour), f64::from(minute), f64::from(second), millisecond);

        // 5. Let v be TimeClip(MakeDate(Day(t), time)).
        let mut date = make_date(day(time), new_time);
        date = time_clip(date);

        // 6. Set the [[DateValue]] internal slot of this Date object to v.
        this_date(vm).set_date_value(date);

        // 7. Return v.
        Ok(Value::from_f64(date))
    }

    // 21.4.4.32 Date.prototype.setUTCMinutes ( min [ , sec [ , ms ] ] ), https://tc39.es/ecma262/#sec-date.prototype.setutcminutes
    fn set_utc_minutes(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. Let m be ? ToNumber(min).
        let minute = vm.argument(0).to_number(vm)?.as_f64();

        // 3. If sec is present, let s be ? ToNumber(sec).
        let second = argument_or_empty(vm, 1)?;

        // 4. If ms is present, let milli be ? ToNumber(ms).
        let millisecond = argument_or_empty(vm, 2)?;

        // 5. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 6. If sec is not present, let s be SecFromTime(t).
        let second = second.unwrap_or_else(|| f64::from(sec_from_time(time)));

        // 7. If ms is not present, let milli be msFromTime(t).
        let millisecond = millisecond.unwrap_or_else(|| f64::from(ms_from_time(time)));

        // 8. Let date be MakeDate(Day(t), MakeTime(HourFromTime(t), m, s, milli)).
        let hour = hour_from_time(time);

        let new_time = make_time(f64::from(hour), minute, second, millisecond);
        let mut date = make_date(day(time), new_time);

        // 9. Let v be TimeClip(date).
        date = time_clip(date);

        // 10. Set the [[DateValue]] internal slot of this Date object to v.
        this_date(vm).set_date_value(date);

        // 11. Return v.
        Ok(Value::from_f64(date))
    }

    // 21.4.4.33 Date.prototype.setUTCMonth ( month [ , date ] ), https://tc39.es/ecma262/#sec-date.prototype.setutcmonth
    fn set_utc_month(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. Let m be ? ToNumber(month).
        let month = vm.argument(0).to_number(vm)?.as_f64();

        // 3. If date is present, let dt be ? ToNumber(date).
        let date = argument_or_empty(vm, 1)?;

        // 4. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 5. If date is not present, let dt be DateFromTime(t).
        let date = date.unwrap_or_else(|| f64::from(date_from_time(time)));

        // 6. Let newDate be MakeDate(MakeDay(YearFromTime(t), m, dt), TimeWithinDay(t)).
        let year = year_from_time(time);

        let day = make_day(f64::from(year), month, date);
        let mut new_date = make_date(day, time_within_day(time));

        // 7. Let v be TimeClip(newDate).
        new_date = time_clip(new_date);

        // 8. Set the [[DateValue]] internal slot of this Date object to v.
        this_date(vm).set_date_value(new_date);

        // 9. Return v.
        Ok(Value::from_f64(new_date))
    }

    // 21.4.4.34 Date.prototype.setUTCSeconds ( sec [ , ms ] ), https://tc39.es/ecma262/#sec-date.prototype.setutcseconds
    fn set_utc_seconds(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. Let s be ? ToNumber(sec).
        let second = vm.argument(0).to_number(vm)?.as_f64();

        // 3. If ms is present, let milli be ? ToNumber(ms).
        let millisecond = argument_or_empty(vm, 1)?;

        // 4. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 5. If ms is not present, let milli be msFromTime(t).
        let millisecond = millisecond.unwrap_or_else(|| f64::from(ms_from_time(time)));

        // 6. Let date be MakeDate(Day(t), MakeTime(HourFromTime(t), MinFromTime(t), s, milli)).
        let hour = hour_from_time(time);
        let minute = min_from_time(time);

        let new_time = make_time(f64::from(hour), f64::from(minute), second, millisecond);
        let mut new_date = make_date(day(time), new_time);

        // 7. Let v be TimeClip(date).
        new_date = time_clip(new_date);

        // 8. Set the [[DateValue]] internal slot of this Date object to v.
        this_date(vm).set_date_value(new_date);

        // 9. Return v.
        Ok(Value::from_f64(new_date))
    }

    // 21.4.4.35 Date.prototype.toDateString ( ), https://tc39.es/ecma262/#sec-date.prototype.todatestring
    fn to_date_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be this Date object.
        // 2. Let tv be ? thisTimeValue(O).
        let time = this_time_value(vm, vm.this_value())?;

        // 3. If tv is NaN, return "Invalid Date".
        if time.is_nan() {
            return Ok(invalid_date(vm));
        }

        // 4. Let t be LocalTime(tv).
        // 5. Return DateString(t).
        let string = date_string(local_time(time));
        Ok(Value::from_string(PrimitiveString::create(vm, string)))
    }

    // 21.4.4.36 Date.prototype.toISOString ( ), https://tc39.es/ecma262/#sec-date.prototype.toisostring
    fn to_iso_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_object = typed_this_object::<Date>(vm, "Date")?;

        if !this_object.date_value().is_finite() {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::InvalidTimeValue, &[]);
        }

        let string = this_object.iso_date_string();
        Ok(Value::from_string(PrimitiveString::create(vm, string)))
    }

    // 21.4.4.37 Date.prototype.toJSON ( key ), https://tc39.es/ecma262/#sec-date.prototype.tojson
    fn to_json(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_value = vm.this_value();

        let time_value = this_value.to_primitive(vm, PreferredType::Number)?;

        if time_value.is_number() && !time_value.is_finite_number() {
            return Ok(Value::NULL);
        }

        this_value.invoke(vm, &vm.names.toISOString, &[])
    }

    // 21.4.4.38 Date.prototype.toLocaleDateString ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-date.prototype.tolocaledatestring
    // 20.4.2 Date.prototype.toLocaleDateString ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sup-date.prototype.tolocaledatestring
    fn to_locale_date_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let x be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If x is NaN, return "Invalid Date".
        if time.is_nan() {
            return Ok(invalid_date(vm));
        }

        // 3. Let dateFormat be ? CreateDateTimeFormat(%DateTimeFormat%, locales, options, "date", "date").
        // 4. Return ? FormatDateTime(dateFormat, x).
        unimplemented_runtime_function(
            "Intl::create_date_time_format and Intl::format_date_time, for Date.prototype.toLocaleDateString, which \
             needs Intl.DateTimeFormat",
            0,
        )
    }

    // 21.4.4.39 Date.prototype.toLocaleString ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-date.prototype.tolocalestring
    // 20.4.1 Date.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sup-date.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let x be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If x is NaN, return "Invalid Date".
        if time.is_nan() {
            return Ok(invalid_date(vm));
        }

        // 3. Let dateFormat be ? CreateDateTimeFormat(%DateTimeFormat%, locales, options, "any", "all").
        // 4. Return ? FormatDateTime(dateFormat, x).
        unimplemented_runtime_function(
            "Intl::create_date_time_format and Intl::format_date_time, for Date.prototype.toLocaleString, which needs \
             Intl.DateTimeFormat",
            0,
        )
    }

    // 21.4.4.40 Date.prototype.toLocaleTimeString ( [ reserved1 [ , reserved2 ] ] ), https://tc39.es/ecma262/#sec-date.prototype.tolocaletimestring
    // 20.4.3 Date.prototype.toLocaleTimeString ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sup-date.prototype.tolocaletimestring
    fn to_locale_time_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let x be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If x is NaN, return "Invalid Date".
        if time.is_nan() {
            return Ok(invalid_date(vm));
        }

        // 3. Let timeFormat be ? CreateDateTimeFormat(%DateTimeFormat%, locales, options, "time", "time").
        // 4. Return ? FormatDateTime(timeFormat, x).
        unimplemented_runtime_function(
            "Intl::create_date_time_format and Intl::format_date_time, for Date.prototype.toLocaleTimeString, which \
             needs Intl.DateTimeFormat",
            0,
        )
    }

    // 21.4.4.41 Date.prototype.toString ( ), https://tc39.es/ecma262/#sec-date.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let tv be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. Return ToDateString(tv).
        Ok(Value::from_string(PrimitiveString::create(vm, to_date_string(time))))
    }

    // 21.4.4.42 Date.prototype.toTimeString ( ), https://tc39.es/ecma262/#sec-date.prototype.totimestring
    fn to_time_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be this Date object.
        // 2. Let tv be ? thisTimeValue(O).
        let time = this_time_value(vm, vm.this_value())?;

        // 3. If tv is NaN, return "Invalid Date".
        if time.is_nan() {
            return Ok(invalid_date(vm));
        }

        // 4. Let t be LocalTime(tv).
        // 5. Return the string-concatenation of TimeString(t) and TimeZoneString(tv).
        let mut string = Utf16StringBuilder::new();
        string.append(Utf16View::of_string(&time_string(local_time(time))));
        string.append(Utf16View::of_string(&time_zone_string(time)));
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            string.to_utf16_string(),
        )))
    }

    // 21.4.4.43 Date.prototype.toUTCString ( ), https://tc39.es/ecma262/#sec-date.prototype.toutcstring
    fn to_utc_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let O be this Date object.
        // 2. Let tv be ? thisTimeValue(O).
        let time = this_time_value(vm, vm.this_value())?;

        // 3. If tv is NaN, return "Invalid Date".
        if time.is_nan() {
            return Ok(invalid_date(vm));
        }

        // 4. Let weekday be the Name of the entry in Table 62 with the Number WeekDay(tv).
        let weekday = SHORT_DAY_NAMES[usize::from(week_day(time))];

        // 5. Let month be the Name of the entry in Table 63 with the Number MonthFromTime(tv).
        let month = SHORT_MONTH_NAMES[usize::from(month_from_time(time))];

        // 6. Let day be ToZeroPaddedDecimalString(ℝ(DateFromTime(tv)), 2).
        let day = date_from_time(time);

        // 7. Let yv be YearFromTime(tv).
        let year = year_from_time(time);

        // 8. If yv is +0𝔽 or yv > +0𝔽, let yearSign be the empty String; otherwise, let yearSign be "-".
        let year_sign = if year >= 0 { "" } else { "-" };

        // 9. Let paddedYear be ToZeroPaddedDecimalString(abs(ℝ(yv)), 4).
        // 10. Return the string-concatenation of weekday, ",", the code unit 0x0020 (SPACE), day, the code unit 0x0020 (SPACE), month, the code unit 0x0020 (SPACE), yearSign, paddedYear, the code unit 0x0020 (SPACE), and TimeString(tv).
        let string = format!(
            "{weekday}, {day:02} {month} {year_sign}{:04} {}",
            year.unsigned_abs(),
            Utf16View::of_string(&time_string(time)).to_utf8()
        );
        Ok(Value::from_string(PrimitiveString::create_from_utf8(vm, &string)))
    }

    // 21.4.4.45 Date.prototype [ @@toPrimitive ] ( hint ), https://tc39.es/ecma262/#sec-date.prototype-@@toprimitive
    fn symbol_to_primitive(vm: &Vm) -> ThrowCompletionOr<Value> {
        let this_value = vm.this_value();
        if !this_value.is_object() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&this_value]);
        }
        let hint_value = vm.argument(0);
        if !hint_value.is_string() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::InvalidHint, &[&hint_value]);
        }
        let hint_string = hint_value.as_string();
        let hint = hint_string.utf16_string_view();
        let try_first = if hint == "string" || hint == "default" {
            PreferredType::String
        } else if hint == "number" {
            PreferredType::Number
        } else {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::InvalidHint, &[&hint.to_utf8()]);
        };
        this_value.as_object().ordinary_to_primitive(vm, try_first)
    }

    // 14.8.1 Date.prototype.toTemporalInstant ( ), https://tc39.es/proposal-temporal/#sec-date.prototype.totemporalinstant
    fn to_temporal_instant(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let dateObject be the this value.
        // 2. Perform ? RequireInternalSlot(dateObject, [[DateValue]]).
        let date_object = typed_this_value::<Date>(vm, "Date")?;

        // 3. Let t be dateObject.[[DateValue]].
        let time = date_object.date_value();

        // 4. Let ns be ? NumberToBigInt(t) × ℤ(10**6).
        number_to_bigint(vm, Value::from_f64(time))?;

        // 5. Return ! CreateTemporalInstant(ns).
        unimplemented_runtime_function(
            "Temporal::create_temporal_instant, for Date.prototype.toTemporalInstant, which needs Temporal.Instant",
            0,
        )
    }

    // B.2.4.1 Date.prototype.getYear ( ), https://tc39.es/ecma262/#sec-date.prototype.getyear
    fn get_year(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, return NaN.
        if time.is_nan() {
            return Ok(nan());
        }

        // 3. Return YearFromTime(LocalTime(t)) - 1900𝔽.
        Ok(Value::from_i32(year_from_time(local_time(time)).wrapping_sub(1900)))
    }

    // B.2.4.2 Date.prototype.setYear ( year ), https://tc39.es/ecma262/#sec-date.prototype.setyear
    fn set_year(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let t be ? thisTimeValue(this value).
        let this_time = this_time_value(vm, vm.this_value())?;

        // 2. If t is NaN, set t to +0𝔽; otherwise, set t to LocalTime(t).
        let time = if this_time.is_nan() { 0.0 } else { local_time(this_time) };

        // 3. Let y be ? ToNumber(year).
        let mut year = vm.argument(0).to_number(vm)?.as_f64();

        let this_object = this_date(vm);

        // 4. If y is NaN, then
        if year.is_nan() {
            // a. Set the [[DateValue]] internal slot of this Date object to NaN.
            this_object.set_date_value(f64::NAN);

            // b. Return NaN.
            return Ok(nan());
        }

        // 5. Let yi be ! ToIntegerOrInfinity(y).
        let year_integer = to_integer_or_infinity(year);

        // 6. If 0 ≤ yi ≤ 99, let yyyy be 1900𝔽 + 𝔽(yi).
        if (0.0..=99.0).contains(&year_integer) {
            year = 1900.0 + year_integer;
        }
        // 7. Else, let yyyy be y.

        // 8. Let d be MakeDay(yyyy, MonthFromTime(t), DateFromTime(t)).
        let day = make_day(year, f64::from(month_from_time(time)), f64::from(date_from_time(time)));

        // 9. Let date be UTC(MakeDate(d, TimeWithinDay(t))).
        let date = utc_time(make_date(day, time_within_day(time)));

        // 10. Set the [[DateValue]] internal slot of this Date object to TimeClip(date).
        let new_date = time_clip(date);
        this_object.set_date_value(new_date);

        // 11. Return the value of the [[DateValue]] internal slot of this Date object.
        Ok(Value::from_f64(new_date))
    }
}

// 21.4.4.41.1 TimeString ( tv ), https://tc39.es/ecma262/#sec-timestring
// 14.5.8 TimeString ( tv ), https://tc39.es/proposal-temporal/#sec-timestring
pub fn time_string(time: f64) -> Utf16String {
    // 1. Let timeString be FormatTimeString(ℝ(HourFromTime(tv)), ℝ(MinFromTime(tv)), ℝ(SecFromTime(tv)), 0, 0).
    let time_string = format_time_string(
        hour_from_time(time),
        min_from_time(time),
        Some(sec_from_time(time)),
        None,
    );

    // 4. Return the string-concatenation of timeString, the code unit 0x0020 (SPACE), and "GMT".
    Utf16String::from_utf8(&format!("{time_string} GMT"))
}

// 21.4.4.41.2 DateString ( tv ), https://tc39.es/ecma262/#sec-datestring
pub fn date_string(time: f64) -> Utf16String {
    // 1. Let weekday be the Name of the entry in Table 62 with the Number WeekDay(tv).
    let weekday = SHORT_DAY_NAMES[usize::from(week_day(time))];

    // 2. Let month be the Name of the entry in Table 63 with the Number MonthFromTime(tv).
    let month = SHORT_MONTH_NAMES[usize::from(month_from_time(time))];

    // 3. Let day be ToZeroPaddedDecimalString(ℝ(DateFromTime(tv)), 2).
    let day = date_from_time(time);

    // 4. Let yv be YearFromTime(tv).
    let year = year_from_time(time);

    // 5. If yv is +0𝔽 or yv > +0𝔽, let yearSign be the empty String; otherwise, let yearSign be "-".
    let year_sign = if year >= 0 { "" } else { "-" };

    // 6. Let paddedYear be ToZeroPaddedDecimalString(abs(ℝ(yv)), 4).
    // 7. Return the string-concatenation of weekday, the code unit 0x0020 (SPACE), month, the code unit 0x0020 (SPACE), day, the code unit 0x0020 (SPACE), yearSign, and paddedYear.
    Utf16String::from_utf8(&format!(
        "{weekday} {month} {day:02} {year_sign}{:04}",
        year.unsigned_abs()
    ))
}

// 21.4.4.41.3 TimeZoneString ( tv ), https://tc39.es/ecma262/#sec-timezoneestring
// 14.5.9 TimeZoneString ( tv ), https://tc39.es/proposal-temporal/#sec-timezoneestring
pub fn time_zone_string(time: f64) -> Utf16String {
    // 1. Let systemTimeZoneIdentifier be SystemTimeZoneIdentifier().
    let system_time_zone_identifier = system_time_zone_identifier();

    // 2. Let offsetMinutes be ! ParseTimeZoneIdentifier(systemTimeZoneIdentifier).[[OffsetMinutes]].
    let mut offset_minutes =
        parse_time_zone_identifier(Utf16View::of_string(&system_time_zone_identifier)).offset_minutes;
    let mut in_dst = InDST::No;

    // 2. If offsetMinutes is EMPTY, then
    if offset_minutes.is_none() {
        // a. Let offsetNs be GetNamedTimeZoneOffsetNanoseconds(systemTimeZoneIdentifier, ℤ(ℝ(tv) × 10^6)).
        let offset = get_named_time_zone_offset_milliseconds(Utf16View::of_string(&system_time_zone_identifier), time);
        in_dst = offset.in_dst;

        // b. Set offsetMinutes to truncate(offsetNs / (60 × 10**9)).
        offset_minutes = Some((offset.offset_nanoseconds as f64 / 60_000_000_000.0).trunc() as i64);
    }

    // 3. Let offsetString be FormatOffsetTimeZoneIdentifier(offsetMinutes, UNSEPARATED).
    let offset_string = format_offset_time_zone_identifier(
        offset_minutes.expect("the offset minutes were just computed"),
        Some(TimeStyle::Unseparated),
    );

    // 5. Let tzName be an implementation-defined string that is either the empty String or the string-concatenation of
    //    the code unit 0x0020 (SPACE), the code unit 0x0028 (LEFT PARENTHESIS), an implementation-defined timezone name,
    //    and the code unit 0x0029 (RIGHT PARENTHESIS).
    let mut tz_name = current_time_zone();

    // Most implementations seem to prefer the long-form display name of the time zone. Not super important, but we may as well match that behavior.
    let locale = Utf16View::of_string(&default_locale()).to_utf8();
    let time_zone_identifier = Utf16View::of_string(&tz_name).to_utf8();
    if let Some(name) = time_zone_display_name(&locale, &time_zone_identifier, in_dst, time) {
        tz_name = name;
    }

    // 10. Return the string-concatenation of offsetString and tzName.
    let mut string = Utf16StringBuilder::new();
    string.append_ascii(&offset_string);
    string.append_ascii(" (");
    string.append(Utf16View::of_string(&tz_name));
    string.append_ascii(")");
    string.to_utf16_string()
}

// 21.4.4.41.4 ToDateString ( tv ), https://tc39.es/ecma262/#sec-todatestring
pub fn to_date_string(time: f64) -> Utf16String {
    // 1. If tv is NaN, return "Invalid Date".
    if time.is_nan() {
        return Utf16String::from_utf8("Invalid Date");
    }

    // 2. Let t be LocalTime(tv).
    let time = local_time(time);

    // 3. Return the string-concatenation of DateString(t), the code unit 0x0020 (SPACE), TimeString(t), and TimeZoneString(tv).
    // NB: The C++ passes t rather than tv to TimeZoneString, and so do we.
    let mut string = Utf16StringBuilder::new();
    string.append(Utf16View::of_string(&date_string(time)));
    string.append_ascii(" ");
    string.append(Utf16View::of_string(&time_string(time)));
    string.append(Utf16View::of_string(&time_zone_string(time)));
    string.to_utf16_string()
}
