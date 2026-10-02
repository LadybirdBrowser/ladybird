/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/PlainDateTimePrototype.cpp: %Temporal.PlainDateTime.prototype%.

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    RoundingMode, get_options_object, get_rounding_increment_option, get_rounding_mode_option,
};
use crate::runtime::big_int::BigInt;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::date_time_format::{FormattableDateTime, format_date_time};
use crate::runtime::intl::date_time_format_constructor::{OptionDefaults, OptionRequired, create_date_time_format};
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::runtime::temporal::abstract_operations::{
    ArithmeticOperation, DateType, DurationOperation, SecondsPrecision, ShowCalendar, Unit, UnitDefault, UnitGroup,
    UnitValue, get_temporal_disambiguation_option, get_temporal_fractional_second_digits_option,
    get_temporal_overflow_option, get_temporal_show_calendar_name_option, get_temporal_unit_valued_option,
    is_partial_temporal_object, iso_date_to_fields, maximum_temporal_duration_rounding_increment,
    temporal_unit_to_string, to_seconds_string_precision_record, validate_temporal_rounding_increment,
    validate_temporal_unit_value,
};
use crate::runtime::temporal::calendar::{
    CalendarDate, CalendarField, CalendarFieldListOrPartial, calendar_equals, calendar_iso_to_date,
    calendar_merge_fields, prepare_calendar_fields, to_temporal_calendar_identifier,
};
use crate::runtime::temporal::plain_date::create_temporal_date;
use crate::runtime::temporal::plain_date_time::{
    PlainDateTime, add_duration_to_date_time, combine_iso_date_and_time_record, compare_iso_date_time,
    create_temporal_date_time, difference_temporal_plain_date_time, interpret_temporal_date_time_fields,
    iso_date_time_to_string, iso_date_time_within_limits, round_iso_date_time, to_temporal_date_time,
};
use crate::runtime::temporal::plain_time::{create_temporal_time, to_time_record_or_midnight};
use crate::runtime::temporal::time_zone::{get_epoch_nanoseconds_for, to_temporal_time_zone_identifier};
use crate::runtime::temporal::zoned_date_time::create_temporal_zoned_date_time;
use crate::utf16::Utf16View;

// 5.3 Properties of the Temporal.PlainDateTime Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plaindatetime-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct PlainDateTimePrototype {
    base: Object,
}

define_object_class!(PlainDateTimePrototype, extends: [Object], methods: {
    initialize: PlainDateTimePrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_plain_date_time(vm: &Vm) -> ThrowCompletionOr<Gc<PlainDateTime>> {
    typed_this_object::<PlainDateTime>(vm, "Temporal.PlainDateTime")
}

fn string_value(vm: &Vm, string: &str) -> Value {
    Value::from_string(PrimitiveString::create(vm, Utf16String::from_utf8(string)))
}

/// CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).
fn calendar_date_of(plain_date_time: &PlainDateTime) -> CalendarDate {
    let calendar = plain_date_time.calendar();
    calendar_iso_to_date(
        Utf16View::of_string(&calendar),
        plain_date_time.iso_date_time().iso_date,
    )
}

impl PlainDateTimePrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<PlainDateTimePrototype> {
        realm.create_object(
            vm,
            PlainDateTimePrototype {
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

        // 5.3.2 Temporal.PlainDateTime.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Temporal.PlainDateTime")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        let define_getter = |name: &PropertyKey, getter| {
            object.define_native_accessor(
                vm,
                realm,
                name,
                getter,
                None,
                PropertyAttributes::new(Attribute::CONFIGURABLE),
            );
        };
        define_getter(
            &names.calendarId,
            raw_native!(PlainDateTimePrototype::calendar_id_getter),
        );
        define_getter(&names.era, raw_native!(PlainDateTimePrototype::era_getter));
        define_getter(&names.eraYear, raw_native!(PlainDateTimePrototype::era_year_getter));
        define_getter(&names.year, raw_native!(PlainDateTimePrototype::year_getter));
        define_getter(&names.month, raw_native!(PlainDateTimePrototype::month_getter));
        define_getter(&names.monthCode, raw_native!(PlainDateTimePrototype::month_code_getter));
        define_getter(&names.day, raw_native!(PlainDateTimePrototype::day_getter));
        define_getter(&names.hour, raw_native!(PlainDateTimePrototype::hour_getter));
        define_getter(&names.minute, raw_native!(PlainDateTimePrototype::minute_getter));
        define_getter(&names.second, raw_native!(PlainDateTimePrototype::second_getter));
        define_getter(
            &names.millisecond,
            raw_native!(PlainDateTimePrototype::millisecond_getter),
        );
        define_getter(
            &names.microsecond,
            raw_native!(PlainDateTimePrototype::microsecond_getter),
        );
        define_getter(
            &names.nanosecond,
            raw_native!(PlainDateTimePrototype::nanosecond_getter),
        );
        define_getter(
            &names.dayOfWeek,
            raw_native!(PlainDateTimePrototype::day_of_week_getter),
        );
        define_getter(
            &names.dayOfYear,
            raw_native!(PlainDateTimePrototype::day_of_year_getter),
        );
        define_getter(
            &names.weekOfYear,
            raw_native!(PlainDateTimePrototype::week_of_year_getter),
        );
        define_getter(
            &names.yearOfWeek,
            raw_native!(PlainDateTimePrototype::year_of_week_getter),
        );
        define_getter(
            &names.daysInWeek,
            raw_native!(PlainDateTimePrototype::days_in_week_getter),
        );
        define_getter(
            &names.daysInMonth,
            raw_native!(PlainDateTimePrototype::days_in_month_getter),
        );
        define_getter(
            &names.daysInYear,
            raw_native!(PlainDateTimePrototype::days_in_year_getter),
        );
        define_getter(
            &names.monthsInYear,
            raw_native!(PlainDateTimePrototype::months_in_year_getter),
        );
        define_getter(
            &names.inLeapYear,
            raw_native!(PlainDateTimePrototype::in_leap_year_getter),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };
        define_native_function(&names.with, raw_native!(PlainDateTimePrototype::with), 1);
        define_native_function(
            &names.withPlainTime,
            raw_native!(PlainDateTimePrototype::with_plain_time),
            0,
        );
        define_native_function(
            &names.withCalendar,
            raw_native!(PlainDateTimePrototype::with_calendar),
            1,
        );
        define_native_function(&names.add, raw_native!(PlainDateTimePrototype::add), 1);
        define_native_function(&names.subtract, raw_native!(PlainDateTimePrototype::subtract), 1);
        define_native_function(&names.until, raw_native!(PlainDateTimePrototype::until), 1);
        define_native_function(&names.since, raw_native!(PlainDateTimePrototype::since), 1);
        define_native_function(&names.round, raw_native!(PlainDateTimePrototype::round), 1);
        define_native_function(&names.equals, raw_native!(PlainDateTimePrototype::equals), 1);
        define_native_function(&names.toString, raw_native!(PlainDateTimePrototype::to_string), 0);
        define_native_function(
            &names.toLocaleString,
            raw_native!(PlainDateTimePrototype::to_locale_string),
            0,
        );
        define_native_function(&names.toJSON, raw_native!(PlainDateTimePrototype::to_json), 0);
        define_native_function(&names.valueOf, raw_native!(PlainDateTimePrototype::value_of), 0);
        define_native_function(
            &names.toZonedDateTime,
            raw_native!(PlainDateTimePrototype::to_zoned_date_time),
            1,
        );
        define_native_function(
            &names.toPlainDate,
            raw_native!(PlainDateTimePrototype::to_plain_date),
            0,
        );
        define_native_function(
            &names.toPlainTime,
            raw_native!(PlainDateTimePrototype::to_plain_time),
            0,
        );
    }

    // 5.3.3 get Temporal.PlainDateTime.prototype.calendarId, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.calendarid
    fn calendar_id_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return plainDateTime.[[Calendar]].
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            plain_date_time.calendar(),
        )))
    }

    // 5.3.4 get Temporal.PlainDateTime.prototype.era, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.era
    fn era_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[Era]].
        let result = calendar_date_of(&plain_date_time).era;

        let Some(result) = result else {
            return Ok(Value::UNDEFINED);
        };

        Ok(Value::from_string(PrimitiveString::create(vm, result)))
    }

    // 5.3.5 get Temporal.PlainDateTime.prototype.eraYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.erayear
    fn era_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Let result be CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[EraYear]].
        let result = calendar_date_of(&plain_date_time).era_year;

        // 4. If result is undefined, return undefined.
        let Some(result) = result else {
            return Ok(Value::UNDEFINED);
        };

        // 5. Return 𝔽(result).
        Ok(Value::from_i32(result))
    }

    // 5.3.6 get Temporal.PlainDateTime.prototype.year, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.year
    fn year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[Year]]).
        Ok(Value::from_i32(calendar_date_of(&plain_date_time).year))
    }

    // 5.3.7 get Temporal.PlainDateTime.prototype.month, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.month
    fn month_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[Month]]).
        Ok(Value::from_i32(i32::from(calendar_date_of(&plain_date_time).month)))
    }

    // 5.3.8 get Temporal.PlainDateTime.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.monthcode
    fn month_code_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[MonthCode]].
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            calendar_date_of(&plain_date_time).month_code,
        )))
    }

    // 5.3.9 get Temporal.PlainDateTime.prototype.day, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.monthcode
    fn day_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[Day]]).
        Ok(Value::from_i32(i32::from(calendar_date_of(&plain_date_time).day)))
    }

    // 5.3.10 get Temporal.PlainDateTime.prototype.hour, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.hour
    fn hour_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(plainDateTime.[[ISODateTime]].[[Time]].[[Hour]]).
        Ok(Value::from_i32(i32::from(plain_date_time.iso_date_time().time.hour)))
    }

    // 5.3.11 get Temporal.PlainDateTime.prototype.minute, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.minute
    fn minute_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(plainDateTime.[[ISODateTime]].[[Time]].[[Minute]]).
        Ok(Value::from_i32(i32::from(plain_date_time.iso_date_time().time.minute)))
    }

    // 5.3.12 get Temporal.PlainDateTime.prototype.second, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.second
    fn second_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(plainDateTime.[[ISODateTime]].[[Time]].[[Second]]).
        Ok(Value::from_i32(i32::from(plain_date_time.iso_date_time().time.second)))
    }

    // 5.3.13 get Temporal.PlainDateTime.prototype.millisecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.millisecond
    fn millisecond_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(plainDateTime.[[ISODateTime]].[[Time]].[[Millisecond]]).
        Ok(Value::from_i32(i32::from(
            plain_date_time.iso_date_time().time.millisecond,
        )))
    }

    // 5.3.14 get Temporal.PlainDateTime.prototype.microsecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.microsecond
    fn microsecond_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(plainDateTime.[[ISODateTime]].[[Time]].[[Microsecond]]).
        Ok(Value::from_i32(i32::from(
            plain_date_time.iso_date_time().time.microsecond,
        )))
    }

    // 5.3.15 get Temporal.PlainDateTime.prototype.nanosecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.nanosecond
    fn nanosecond_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(plainDateTime.[[ISODateTime]].[[Time]].[[Nanosecond]]).
        Ok(Value::from_i32(i32::from(
            plain_date_time.iso_date_time().time.nanosecond,
        )))
    }

    // 5.3.16 get Temporal.PlainDateTime.prototype.dayOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.dayofweek
    fn day_of_week_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[DayOfWeek]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&plain_date_time).day_of_week,
        )))
    }

    // 5.3.17 get Temporal.PlainDateTime.prototype.dayOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.dayofyear
    fn day_of_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[DayOfYear]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&plain_date_time).day_of_year,
        )))
    }

    // 5.3.18 get Temporal.PlainDateTime.prototype.weekOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.weekofyear
    fn week_of_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Let result be CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[WeekOfYear]].[[Week]].
        let result = calendar_date_of(&plain_date_time).week_of_year.week;

        // 4. If result is undefined, return undefined.
        let Some(result) = result else {
            return Ok(Value::UNDEFINED);
        };

        // 5. Return 𝔽(result).
        Ok(Value::from_i32(i32::from(result)))
    }

    // 5.3.19 get Temporal.PlainDateTime.prototype.yearOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.yearofweek
    fn year_of_week_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Let result be CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[WeekOfYear]].[[Year]].
        let result = calendar_date_of(&plain_date_time).week_of_year.year;

        // 4. If result is undefined, return undefined.
        let Some(result) = result else {
            return Ok(Value::UNDEFINED);
        };

        // 5. Return 𝔽(result).
        Ok(Value::from_i32(result))
    }

    // 5.3.20 get Temporal.PlainDateTime.prototype.daysInWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.daysinweek
    fn days_in_week_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[DaysInWeek]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&plain_date_time).days_in_week,
        )))
    }

    // 5.3.21 get Temporal.PlainDateTime.prototype.daysInMonth, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.daysinmonth
    fn days_in_month_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[DaysInMonth]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&plain_date_time).days_in_month,
        )))
    }

    // 5.3.22 get Temporal.PlainDateTime.prototype.daysInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.daysinyear
    fn days_in_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[DaysInYear]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&plain_date_time).days_in_year,
        )))
    }

    // 5.3.23 get Temporal.PlainDateTime.prototype.monthsInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.monthsinyear
    fn months_in_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[MonthsInYear]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&plain_date_time).months_in_year,
        )))
    }

    // 5.3.24 get Temporal.PlainDateTime.prototype.inLeapYear, https://tc39.es/proposal-temporal/#sec-get-temporal.plaindatetime.prototype.inleapyear
    fn in_leap_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return 𝔽(CalendarISOToDate(plainDateTime.[[Calendar]], plainDateTime.[[ISODateTime]].[[ISODate]]).[[InLeapYear]]).
        Ok(Value::from_bool(calendar_date_of(&plain_date_time).in_leap_year))
    }

    // 5.3.25 Temporal.PlainDateTime.prototype.with ( temporalDateTimeLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.with
    fn with(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_date_time_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. If ? IsPartialTemporalObject(temporalDateTimeLike) is false, throw a TypeError exception.
        if !is_partial_temporal_object(vm, temporal_date_time_like)? {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::TemporalObjectMustBePartialTemporalObject,
                &[],
            );
        }

        // 4. Let calendar be plainDateTime.[[Calendar]].
        let calendar = plain_date_time.calendar();
        let iso_date_time = plain_date_time.iso_date_time();

        // 5. Let fields be ISODateToFields(calendar, plainDateTime.[[ISODateTime]].[[ISODate]], DATE).
        let mut fields = iso_date_to_fields(Utf16View::of_string(&calendar), iso_date_time.iso_date, DateType::Date);

        // 6. Set fields.[[Hour]] to plainDateTime.[[ISODateTime]].[[Time]].[[Hour]].
        fields.hour = Some(iso_date_time.time.hour);

        // 7. Set fields.[[Minute]] to plainDateTime.[[ISODateTime]].[[Time]].[[Minute]].
        fields.minute = Some(iso_date_time.time.minute);

        // 8. Set fields.[[Second]] to plainDateTime.[[ISODateTime]].[[Time]].[[Second]].
        fields.second = Some(iso_date_time.time.second);

        // 9. Set fields.[[Millisecond]] to plainDateTime.[[ISODateTime]].[[Time]].[[Millisecond]].
        fields.millisecond = Some(iso_date_time.time.millisecond);

        // 10. Set fields.[[Microsecond]] to plainDateTime.[[ISODateTime]].[[Time]].[[Microsecond]].
        fields.microsecond = Some(iso_date_time.time.microsecond);

        // 11. Set fields.[[Nanosecond]] to plainDateTime.[[ISODateTime]].[[Time]].[[Nanosecond]].
        fields.nanosecond = Some(iso_date_time.time.nanosecond);

        // 12. Let partialDateTime be ? PrepareCalendarFields(calendar, temporalDateTimeLike, « YEAR, MONTH, MONTH-CODE, DAY », « HOUR, MINUTE, SECOND, MILLISECOND, MICROSECOND, NANOSECOND », PARTIAL).
        const CALENDAR_FIELD_NAMES: [CalendarField; 4] = [
            CalendarField::Year,
            CalendarField::Month,
            CalendarField::MonthCode,
            CalendarField::Day,
        ];
        const NON_CALENDAR_FIELD_NAMES: [CalendarField; 6] = [
            CalendarField::Hour,
            CalendarField::Minute,
            CalendarField::Second,
            CalendarField::Millisecond,
            CalendarField::Microsecond,
            CalendarField::Nanosecond,
        ];
        let partial_date_time = prepare_calendar_fields(
            vm,
            Utf16View::of_string(&calendar),
            &temporal_date_time_like.as_object(),
            &CALENDAR_FIELD_NAMES,
            &NON_CALENDAR_FIELD_NAMES,
            CalendarFieldListOrPartial::Partial,
        )?;

        // 13. Set fields to CalendarMergeFields(calendar, fields, partialDateTime).
        let mut fields = calendar_merge_fields(Utf16View::of_string(&calendar), &fields, &partial_date_time);

        // 14. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, options)?;

        // 15. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

        // 16. Let result be ? InterpretTemporalDateTimeFields(calendar, fields, overflow).
        let result = interpret_temporal_date_time_fields(vm, Utf16View::of_string(&calendar), &mut fields, overflow)?;

        // 17. Return ? CreateTemporalDateTime(result, calendar).
        Ok(Value::from_object(create_temporal_date_time(
            vm, &result, calendar, None,
        )?))
    }

    // 5.3.26 Temporal.PlainDateTime.prototype.withPlainTime ( [ plainTimeLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.withplaintime
    fn with_plain_time(vm: &Vm) -> ThrowCompletionOr<Value> {
        let plain_time_like = vm.argument(0);

        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Let time be ? ToTimeRecordOrMidnight(plainTimeLike).
        let time = to_time_record_or_midnight(vm, plain_time_like)?;

        // 4. Let isoDateTime be CombineISODateAndTimeRecord(plainDateTime.[[ISODateTime]].[[ISODate]], time).
        let iso_date_time = combine_iso_date_and_time_record(plain_date_time.iso_date_time().iso_date, time);

        // 5. Return ? CreateTemporalDateTime(isoDateTime, plainDateTime.[[Calendar]]).
        Ok(Value::from_object(create_temporal_date_time(
            vm,
            &iso_date_time,
            plain_date_time.calendar(),
            None,
        )?))
    }

    // 5.3.27 Temporal.PlainDateTime.prototype.withCalendar ( calendarLike ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.withcalendar
    fn with_calendar(vm: &Vm) -> ThrowCompletionOr<Value> {
        let calendar_like = vm.argument(0);

        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Let calendar be ? ToTemporalCalendarIdentifier(calendarLike).
        let calendar = to_temporal_calendar_identifier(vm, calendar_like)?;

        // 4. Return ! CreateTemporalDateTime(plainDateTime.[[ISODateTime]], calendar).
        Ok(Value::from_object(create_temporal_date_time(
            vm,
            &plain_date_time.iso_date_time(),
            calendar,
            None,
        )?))
    }

    // 5.3.28 Temporal.PlainDateTime.prototype.add ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.add
    fn add(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_duration_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return ? AddDurationToDateTime(ADD, plainDateTime, temporalDurationLike, options).
        Ok(Value::from_object(add_duration_to_date_time(
            vm,
            ArithmeticOperation::Add,
            &plain_date_time,
            temporal_duration_like,
            options,
        )?))
    }

    // 5.3.29 Temporal.PlainDateTime.prototype.subtract ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.add
    fn subtract(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_duration_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return ? AddDurationToDateTime(SUBTRACT, plainDateTime, temporalDurationLike, options).
        Ok(Value::from_object(add_duration_to_date_time(
            vm,
            ArithmeticOperation::Subtract,
            &plain_date_time,
            temporal_duration_like,
            options,
        )?))
    }

    // 5.3.30 Temporal.PlainDateTime.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.until
    fn until(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return ? DifferenceTemporalPlainDateTime(UNTIL, plainDateTime, other, options).
        Ok(Value::from_object(difference_temporal_plain_date_time(
            vm,
            DurationOperation::Until,
            &plain_date_time,
            other,
            options,
        )?))
    }

    // 5.3.31 Temporal.PlainDateTime.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.since
    fn since(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return ? DifferenceTemporalPlainDateTime(SINCE, plainDateTime, other, options).
        Ok(Value::from_object(difference_temporal_plain_date_time(
            vm,
            DurationOperation::Since,
            &plain_date_time,
            other,
            options,
        )?))
    }

    // 5.3.32 Temporal.PlainDateTime.prototype.round ( roundTo ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.round
    fn round(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let round_to_value = vm.argument(0);

        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. If roundTo is undefined, throw a TypeError exception.
        if round_to_value.is_undefined() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::TemporalMissingOptionsObject, &[]);
        }

        // 4. If roundTo is a String, then
        let round_to = if round_to_value.is_string() {
            // a. Let paramString be roundTo.
            let param_string = round_to_value;

            // b. Set roundTo to OrdinaryObjectCreate(null).
            let round_to = Object::create(vm, realm, None);

            // c. Perform ! CreateDataPropertyOrThrow(roundTo, "smallestUnit", paramString).
            round_to
                .create_data_property_or_throw(vm, &vm.names.smallestUnit, param_string)
                .must();
            round_to
        }
        // 5. Else,
        else {
            // a. Set roundTo to ? GetOptionsObject(roundTo).
            get_options_object(vm, round_to_value)?
        };

        // 6. NOTE: The following steps read options and perform independent validation in alphabetical order
        //    (GetRoundingIncrementOption reads "roundingIncrement" and GetRoundingModeOption reads "roundingMode").

        // 7. Let roundingIncrement be ? GetRoundingIncrementOption(roundTo).
        let rounding_increment = get_rounding_increment_option(vm, &round_to)?;

        // 8. Let roundingMode be ? GetRoundingModeOption(roundTo, HALF-EXPAND).
        let rounding_mode = get_rounding_mode_option(vm, &round_to, RoundingMode::HalfExpand)?;

        // 9. Let smallestUnit be ? GetTemporalUnitValuedOption(roundTo, "smallestUnit", REQUIRED).
        let smallest_unit =
            get_temporal_unit_valued_option(vm, &round_to, &vm.names.smallestUnit, UnitDefault::Required)?;

        // 10. Perform ? ValidateTemporalUnitValue(smallestUnit, TIME, « DAY »).
        validate_temporal_unit_value(
            vm,
            &vm.names.smallestUnit,
            smallest_unit,
            UnitGroup::Time,
            &[UnitValue::Unit(Unit::Day)],
        )?;
        let UnitValue::Unit(smallest_unit_value) = smallest_unit else {
            unreachable!("the smallest unit was validated");
        };

        // 11. If smallestUnit is DAY, then
        let (maximum, inclusive) = if smallest_unit_value == Unit::Day {
            // a. Let maximum be 1.
            // b. Let inclusive be true.
            (1, true)
        }
        // 12. Else,
        else {
            // a. Let maximum be MaximumTemporalDurationRoundingIncrement(smallestUnit).
            // b. Assert: maximum is not UNSET.
            let maximum = maximum_temporal_duration_rounding_increment(smallest_unit_value)
                .expect("a time unit has a maximum rounding increment");

            // c. Let inclusive be false.
            (maximum, false)
        };

        // 13. Perform ? ValidateTemporalRoundingIncrement(roundingIncrement, maximum, inclusive).
        validate_temporal_rounding_increment(vm, rounding_increment, maximum, inclusive)?;

        // 14. If smallestUnit is NANOSECOND and roundingIncrement = 1, then
        if smallest_unit_value == Unit::Nanosecond && rounding_increment == 1 {
            // a. Return ! CreateTemporalDateTime(plainDateTime.[[ISODateTime]], plainDateTime.[[Calendar]]).
            return Ok(Value::from_object(
                create_temporal_date_time(vm, &plain_date_time.iso_date_time(), plain_date_time.calendar(), None)
                    .must(),
            ));
        }

        // 15. Let result be RoundISODateTime(plainDateTime.[[ISODateTime]], roundingIncrement, smallestUnit, roundingMode).
        let result = round_iso_date_time(
            &plain_date_time.iso_date_time(),
            rounding_increment,
            smallest_unit_value,
            rounding_mode,
        );

        // 16. Return ? CreateTemporalDateTime(result, plainDateTime.[[Calendar]]).
        Ok(Value::from_object(create_temporal_date_time(
            vm,
            &result,
            plain_date_time.calendar(),
            None,
        )?))
    }

    // 5.3.33 Temporal.PlainDateTime.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.equals
    fn equals(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Set other to ? ToTemporalDateTime(other).
        let other = to_temporal_date_time(vm, vm.argument(0), Value::UNDEFINED)?;

        // 4. If CompareISODateTime(plainDateTime.[[ISODateTime]], other.[[ISODateTime]]) ≠ 0, return false.
        if compare_iso_date_time(&plain_date_time.iso_date_time(), &other.iso_date_time()) != 0 {
            return Ok(Value::from_bool(false));
        }

        // 5. Return CalendarEquals(plainDateTime.[[Calendar]], other.[[Calendar]]).
        Ok(Value::from_bool(calendar_equals(
            Utf16View::of_string(&plain_date_time.calendar()),
            Utf16View::of_string(&other.calendar()),
        )))
    }

    // 5.3.34 Temporal.PlainDateTime.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, vm.argument(0))?;

        // 4. NOTE: The following steps read options and perform independent validation in alphabetical order
        //    (GetTemporalShowCalendarNameOption reads "calendarName", GetTemporalFractionalSecondDigitsOption reads
        //    "fractionalSecondDigits", and GetRoundingModeOption reads "roundingMode").

        // 5. Let showCalendar be ? GetTemporalShowCalendarNameOption(resolvedOptions).
        let show_calendar = get_temporal_show_calendar_name_option(vm, &resolved_options)?;

        // 6. Let digits be ? GetTemporalFractionalSecondDigitsOption(resolvedOptions).
        let digits = get_temporal_fractional_second_digits_option(vm, &resolved_options)?;

        // 7. Let roundingMode be ? GetRoundingModeOption(resolvedOptions, TRUNC).
        let rounding_mode = get_rounding_mode_option(vm, &resolved_options, RoundingMode::Trunc)?;

        // 8. Let smallestUnit be ? GetTemporalUnitValuedOption(resolvedOptions, "smallestUnit", UNSET).
        let smallest_unit =
            get_temporal_unit_valued_option(vm, &resolved_options, &vm.names.smallestUnit, UnitDefault::Unset)?;

        // 9. Perform ? ValidateTemporalUnitValue(smallestUnit, TIME).
        validate_temporal_unit_value(vm, &vm.names.smallestUnit, smallest_unit, UnitGroup::Time, &[])?;

        // 10. If smallestUnit is HOUR, throw a RangeError exception.
        if smallest_unit == UnitValue::Unit(Unit::Hour) {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::OptionIsNotValidValue,
                &[&temporal_unit_to_string(Unit::Hour), &vm.names.smallestUnit],
            );
        }

        // 11. Let precision be ToSecondsStringPrecisionRecord(smallestUnit, digits).
        let precision = to_seconds_string_precision_record(smallest_unit, digits);

        // 12. Let result be RoundISODateTime(plainDateTime.[[ISODateTime]], precision.[[Increment]], precision.[[Unit]], roundingMode).
        let result = round_iso_date_time(
            &plain_date_time.iso_date_time(),
            u64::from(precision.increment),
            precision.unit,
            rounding_mode,
        );

        // 13. If ISODateTimeWithinLimits(result) is false, throw a RangeError exception.
        if !iso_date_time_within_limits(&result) {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainDateTime, &[]);
        }

        // 14. Return ISODateTimeToString(result, plainDateTime.[[Calendar]], precision.[[Precision]], showCalendar).
        Ok(string_value(
            vm,
            &iso_date_time_to_string(
                &result,
                Utf16View::of_string(&plain_date_time.calendar()),
                precision.precision,
                show_calendar,
            ),
        ))
    }

    // 5.3.35 Temporal.PlainDateTime.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.tolocalestring
    // 15.11.4.1 Temporal.PlainDateTime.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sup-temporal.plaindatetime.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Let dateFormat be ? CreateDateTimeFormat(%Intl.DateTimeFormat%, locales, options, ANY, ALL).
        let date_format = create_date_time_format(
            vm,
            realm.intrinsics().intl_date_time_format_constructor(vm),
            locales,
            options,
            OptionRequired::Any,
            OptionDefaults::All,
            None,
        )?;

        // 4. Return ? FormatDateTime(dateFormat, plainDateTime).
        let formatted = format_date_time(vm, &date_format, &FormattableDateTime::PlainDateTime(plain_date_time))?;
        Ok(Value::from_string(PrimitiveString::create(vm, formatted)))
    }

    // 5.3.36 Temporal.PlainDateTime.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.tojson
    fn to_json(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return ISODateTimeToString(plainDateTime.[[ISODateTime]], plainDateTime.[[Calendar]], AUTO, AUTO).
        Ok(string_value(
            vm,
            &iso_date_time_to_string(
                &plain_date_time.iso_date_time(),
                Utf16View::of_string(&plain_date_time.calendar()),
                SecondsPrecision::Auto,
                ShowCalendar::Auto,
            ),
        ))
    }

    // 5.3.37 Temporal.PlainDateTime.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::Convert,
            &[&"Temporal.PlainDateTime", &"a primitive value"],
        )
    }

    // 5.3.38 Temporal.PlainDateTime.prototype.toZonedDateTime ( temporalTimeZoneLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.tozoneddatetime
    fn to_zoned_date_time(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_time_zone_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Let timeZone be ? ToTemporalTimeZoneIdentifier(temporalTimeZoneLike).
        let time_zone = to_temporal_time_zone_identifier(vm, temporal_time_zone_like)?;

        // 4. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, options)?;

        // 5. Let disambiguation be ? GetTemporalDisambiguationOption(resolvedOptions).
        let disambiguation = get_temporal_disambiguation_option(vm, &resolved_options)?;

        // 6. Let epochNs be ? GetEpochNanosecondsFor(timeZone, plainDateTime.[[ISODateTime]], disambiguation).
        let epoch_nanoseconds = get_epoch_nanoseconds_for(
            vm,
            Utf16View::of_string(&time_zone),
            &plain_date_time.iso_date_time(),
            disambiguation,
        )?;

        // 7. Return ! CreateTemporalZonedDateTime(epochNs, timeZone, plainDateTime.[[Calendar]]).
        Ok(Value::from_object(
            create_temporal_zoned_date_time(
                vm,
                BigInt::create(vm, epoch_nanoseconds),
                time_zone,
                plain_date_time.calendar(),
                None,
            )
            .must(),
        ))
    }

    // 5.3.39 Temporal.PlainDateTime.prototype.toPlainDate ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.toplaindate
    fn to_plain_date(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return ! CreateTemporalDate(plainDateTime.[[ISODateTime]].[[ISODate]], plainDateTime.[[Calendar]]).
        Ok(Value::from_object(
            create_temporal_date(
                vm,
                plain_date_time.iso_date_time().iso_date,
                plain_date_time.calendar(),
                None,
            )
            .must(),
        ))
    }

    // 5.3.40 Temporal.PlainDateTime.prototype.toPlainTime ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaindatetime.prototype.toplaintime
    fn to_plain_time(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let plainDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(plainDateTime, [[InitializedTemporalDateTime]]).
        let plain_date_time = typed_this_plain_date_time(vm)?;

        // 3. Return ! CreateTemporalTime(plainDateTime.[[ISODateTime]].[[Time]]).
        Ok(Value::from_object(
            create_temporal_time(vm, plain_date_time.iso_date_time().time, None).must(),
        ))
    }
}
