/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/ZonedDateTimePrototype.cpp: %Temporal.ZonedDateTime.prototype%.

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    RoundingMode, big_floor, get_options_object, get_rounding_increment_option, get_rounding_mode_option,
};
use crate::runtime::big_int::BigInt;
use crate::runtime::big_int_algorithms;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::date::{is_offset_time_zone_identifier, parse_date_time_utc_offset};
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
    ArithmeticOperation, DateType, Direction, Disambiguation, DurationOperation, OffsetOption, SecondsPrecision,
    ShowCalendar, ShowOffset, ShowTimeZoneName, Unit, UnitDefault, UnitGroup, UnitValue, get_direction_option,
    get_temporal_disambiguation_option, get_temporal_fractional_second_digits_option, get_temporal_offset_option,
    get_temporal_overflow_option, get_temporal_show_calendar_name_option, get_temporal_show_offset_option,
    get_temporal_show_time_zone_name_option, get_temporal_unit_valued_option, is_partial_temporal_object,
    iso_date_to_fields, maximum_temporal_duration_rounding_increment, temporal_unit_to_string,
    to_seconds_string_precision_record, validate_temporal_rounding_increment, validate_temporal_unit_value,
};
use crate::runtime::temporal::calendar::{
    CalendarField, CalendarFieldListOrPartial, ISO8601_CALENDAR, calendar_equals, calendar_iso_to_date,
    calendar_merge_fields, prepare_calendar_fields, to_temporal_calendar_identifier,
};
use crate::runtime::temporal::duration::{
    add_time_duration_to_epoch_nanoseconds, round_time_duration_to_increment,
    time_duration_from_epoch_nanoseconds_difference, total_time_duration,
};
use crate::runtime::temporal::instant::{NANOSECONDS_PER_MILLISECOND, create_temporal_instant};
use crate::runtime::temporal::iso_records::{ISODateTime, TimeOrStartOfDay};
use crate::runtime::temporal::plain_date::{add_days_to_iso_date, create_temporal_date};
use crate::runtime::temporal::plain_date_time::{
    combine_iso_date_and_time_record, create_temporal_date_time, interpret_temporal_date_time_fields,
    round_iso_date_time,
};
use crate::runtime::temporal::plain_time::{create_temporal_time, to_temporal_time};
use crate::runtime::temporal::time_zone::{
    format_utc_offset_nanoseconds, get_epoch_nanoseconds_for, get_iso_date_time_for,
    get_named_time_zone_next_transition, get_named_time_zone_previous_transition, get_offset_nanoseconds_for,
    get_start_of_day, time_zone_equals, to_temporal_time_zone_identifier,
};
use crate::runtime::temporal::zoned_date_time::{
    MatchBehavior, OffsetBehavior, ZonedDateTime, add_duration_to_zoned_date_time, create_temporal_zoned_date_time,
    difference_temporal_zoned_date_time, interpret_iso_date_time_offset, temporal_zoned_date_time_to_string,
    to_temporal_zoned_date_time,
};
use crate::unicode::calendar::CalendarDate;
use crate::utf16::Utf16View;

// 6.3 Properties of the Temporal.ZonedDateTime Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-zoneddatetime-prototype-object
#[repr(C)]
#[derive(Trace)]
pub struct ZonedDateTimePrototype {
    base: Object,
}

define_object_class!(ZonedDateTimePrototype, extends: [Object], methods: {
    initialize: ZonedDateTimePrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_zoned_date_time(vm: &Vm) -> ThrowCompletionOr<Gc<ZonedDateTime>> {
    typed_this_object::<ZonedDateTime>(vm, "Temporal.ZonedDateTime")
}

fn string_value(vm: &Vm, string: &str) -> Value {
    Value::from_string(PrimitiveString::create(vm, Utf16String::from_utf8(string)))
}

/// GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
fn iso_date_time_of(zoned_date_time: &ZonedDateTime) -> ISODateTime {
    let time_zone = zoned_date_time.time_zone();
    get_iso_date_time_for(
        Utf16View::of_string(&time_zone),
        zoned_date_time.epoch_nanoseconds().big_integer(),
    )
}

/// CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).
fn calendar_date_of(zoned_date_time: &ZonedDateTime, iso_date_time: &ISODateTime) -> CalendarDate {
    let calendar = zoned_date_time.calendar();
    calendar_iso_to_date(Utf16View::of_string(&calendar), iso_date_time.iso_date)
}

impl ZonedDateTimePrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<ZonedDateTimePrototype> {
        realm.create_object(
            vm,
            ZonedDateTimePrototype {
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

        // 6.3.2 Temporal.ZonedDateTime.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype-%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Temporal.ZonedDateTime")),
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
            raw_native!(ZonedDateTimePrototype::calendar_id_getter),
        );
        define_getter(
            &names.timeZoneId,
            raw_native!(ZonedDateTimePrototype::time_zone_id_getter),
        );
        define_getter(&names.era, raw_native!(ZonedDateTimePrototype::era_getter));
        define_getter(&names.eraYear, raw_native!(ZonedDateTimePrototype::era_year_getter));
        define_getter(&names.year, raw_native!(ZonedDateTimePrototype::year_getter));
        define_getter(&names.month, raw_native!(ZonedDateTimePrototype::month_getter));
        define_getter(&names.monthCode, raw_native!(ZonedDateTimePrototype::month_code_getter));
        define_getter(&names.day, raw_native!(ZonedDateTimePrototype::day_getter));
        define_getter(&names.hour, raw_native!(ZonedDateTimePrototype::hour_getter));
        define_getter(&names.minute, raw_native!(ZonedDateTimePrototype::minute_getter));
        define_getter(&names.second, raw_native!(ZonedDateTimePrototype::second_getter));
        define_getter(
            &names.millisecond,
            raw_native!(ZonedDateTimePrototype::millisecond_getter),
        );
        define_getter(
            &names.microsecond,
            raw_native!(ZonedDateTimePrototype::microsecond_getter),
        );
        define_getter(
            &names.nanosecond,
            raw_native!(ZonedDateTimePrototype::nanosecond_getter),
        );
        define_getter(
            &names.epochMilliseconds,
            raw_native!(ZonedDateTimePrototype::epoch_milliseconds_getter),
        );
        define_getter(
            &names.epochNanoseconds,
            raw_native!(ZonedDateTimePrototype::epoch_nanoseconds_getter),
        );
        define_getter(
            &names.dayOfWeek,
            raw_native!(ZonedDateTimePrototype::day_of_week_getter),
        );
        define_getter(
            &names.dayOfYear,
            raw_native!(ZonedDateTimePrototype::day_of_year_getter),
        );
        define_getter(
            &names.weekOfYear,
            raw_native!(ZonedDateTimePrototype::week_of_year_getter),
        );
        define_getter(
            &names.yearOfWeek,
            raw_native!(ZonedDateTimePrototype::year_of_week_getter),
        );
        define_getter(
            &names.hoursInDay,
            raw_native!(ZonedDateTimePrototype::hours_in_day_getter),
        );
        define_getter(
            &names.daysInWeek,
            raw_native!(ZonedDateTimePrototype::days_in_week_getter),
        );
        define_getter(
            &names.daysInMonth,
            raw_native!(ZonedDateTimePrototype::days_in_month_getter),
        );
        define_getter(
            &names.daysInYear,
            raw_native!(ZonedDateTimePrototype::days_in_year_getter),
        );
        define_getter(
            &names.monthsInYear,
            raw_native!(ZonedDateTimePrototype::months_in_year_getter),
        );
        define_getter(
            &names.inLeapYear,
            raw_native!(ZonedDateTimePrototype::in_leap_year_getter),
        );
        define_getter(
            &names.offsetNanoseconds,
            raw_native!(ZonedDateTimePrototype::offset_nanoseconds_getter),
        );
        define_getter(&names.offset, raw_native!(ZonedDateTimePrototype::offset_getter));

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        let define_native_function = |name: &PropertyKey, function, length| {
            object.define_native_function(vm, realm, name, function, length, attr, None);
        };
        define_native_function(&names.with, raw_native!(ZonedDateTimePrototype::with), 1);
        define_native_function(
            &names.withPlainTime,
            raw_native!(ZonedDateTimePrototype::with_plain_time),
            0,
        );
        define_native_function(
            &names.withTimeZone,
            raw_native!(ZonedDateTimePrototype::with_time_zone),
            1,
        );
        define_native_function(
            &names.withCalendar,
            raw_native!(ZonedDateTimePrototype::with_calendar),
            1,
        );
        define_native_function(&names.add, raw_native!(ZonedDateTimePrototype::add), 1);
        define_native_function(&names.subtract, raw_native!(ZonedDateTimePrototype::subtract), 1);
        define_native_function(&names.until, raw_native!(ZonedDateTimePrototype::until), 1);
        define_native_function(&names.since, raw_native!(ZonedDateTimePrototype::since), 1);
        define_native_function(&names.round, raw_native!(ZonedDateTimePrototype::round), 1);
        define_native_function(&names.equals, raw_native!(ZonedDateTimePrototype::equals), 1);
        define_native_function(&names.toString, raw_native!(ZonedDateTimePrototype::to_string), 0);
        define_native_function(
            &names.toLocaleString,
            raw_native!(ZonedDateTimePrototype::to_locale_string),
            0,
        );
        define_native_function(&names.toJSON, raw_native!(ZonedDateTimePrototype::to_json), 0);
        define_native_function(&names.valueOf, raw_native!(ZonedDateTimePrototype::value_of), 0);
        define_native_function(&names.startOfDay, raw_native!(ZonedDateTimePrototype::start_of_day), 0);
        define_native_function(
            &names.getTimeZoneTransition,
            raw_native!(ZonedDateTimePrototype::get_time_zone_transition),
            1,
        );
        define_native_function(&names.toInstant, raw_native!(ZonedDateTimePrototype::to_instant), 0);
        define_native_function(
            &names.toPlainDate,
            raw_native!(ZonedDateTimePrototype::to_plain_date),
            0,
        );
        define_native_function(
            &names.toPlainTime,
            raw_native!(ZonedDateTimePrototype::to_plain_time),
            0,
        );
        define_native_function(
            &names.toPlainDateTime,
            raw_native!(ZonedDateTimePrototype::to_plain_date_time),
            0,
        );
    }

    // 6.3.3 get Temporal.ZonedDateTime.prototype.calendarId, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.calendarid
    fn calendar_id_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Return zonedDateTime.[[Calendar]].
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            zoned_date_time.calendar(),
        )))
    }

    // 6.3.4 get Temporal.ZonedDateTime.prototype.timeZoneId, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.timezoneid
    fn time_zone_id_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Return zonedDateTime.[[TimeZone]].
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            zoned_date_time.time_zone(),
        )))
    }

    // 6.3.5 get Temporal.ZonedDateTime.prototype.era, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.era
    fn era_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[Era]].
        let result = calendar_date_of(&zoned_date_time, &iso_date_time).era;

        // 5. If result is undefined, return undefined.
        let Some(result) = result else {
            return Ok(Value::UNDEFINED);
        };

        // 6. Return 𝔽(result).
        Ok(Value::from_string(PrimitiveString::create(vm, result)))
    }

    // 6.3.6 get Temporal.ZonedDateTime.prototype.eraYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.erayear
    fn era_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Let result be CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[EraYear]].
        let result = calendar_date_of(&zoned_date_time, &iso_date_time).era_year;

        // 5. If result is undefined, return undefined.
        let Some(result) = result else {
            return Ok(Value::UNDEFINED);
        };

        // 6. Return 𝔽(result).
        Ok(Value::from_i32(result))
    }

    // 6.3.7 get Temporal.ZonedDateTime.prototype.year, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.year
    fn year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[Year]]).
        Ok(Value::from_i32(calendar_date_of(&zoned_date_time, &iso_date_time).year))
    }

    // 6.3.8 get Temporal.ZonedDateTime.prototype.month, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.month
    fn month_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[Month]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&zoned_date_time, &iso_date_time).month,
        )))
    }

    // 6.3.9 get Temporal.ZonedDateTime.prototype.monthCode, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.monthcode
    fn month_code_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[MonthCode]].
        Ok(Value::from_string(PrimitiveString::create(
            vm,
            calendar_date_of(&zoned_date_time, &iso_date_time).month_code,
        )))
    }

    // 6.3.10 get Temporal.ZonedDateTime.prototype.day, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.day
    fn day_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[Day]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&zoned_date_time, &iso_date_time).day,
        )))
    }

    // 6.3.11 get Temporal.ZonedDateTime.prototype.hour, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.hour
    fn hour_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(isoDateTime.[[Time]].[[Hour]]).
        Ok(Value::from_i32(i32::from(iso_date_time.time.hour)))
    }

    // 6.3.12 get Temporal.ZonedDateTime.prototype.minute, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.minute
    fn minute_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(isoDateTime.[[Time]].[[Minute]]).
        Ok(Value::from_i32(i32::from(iso_date_time.time.minute)))
    }

    // 6.3.13 get Temporal.ZonedDateTime.prototype.second, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.second
    fn second_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(isoDateTime.[[Time]].[[Second]]).
        Ok(Value::from_i32(i32::from(iso_date_time.time.second)))
    }

    // 6.3.14 get Temporal.ZonedDateTime.prototype.millisecond, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.millisecond
    fn millisecond_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(isoDateTime.[[Time]].[[Millisecond]]).
        Ok(Value::from_i32(i32::from(iso_date_time.time.millisecond)))
    }

    // 6.3.15 get Temporal.ZonedDateTime.prototype.microsecond, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.microsecond
    fn microsecond_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(isoDateTime.[[Time]].[[Microsecond]]).
        Ok(Value::from_i32(i32::from(iso_date_time.time.microsecond)))
    }

    // 6.3.16 get Temporal.ZonedDateTime.prototype.nanosecond, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.nanosecond
    fn nanosecond_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(isoDateTime.[[Time]].[[Nanosecond]]).
        Ok(Value::from_i32(i32::from(iso_date_time.time.nanosecond)))
    }

    // 6.3.17 get Temporal.ZonedDateTime.prototype.epochMilliseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.epochmilliseconds
    fn epoch_milliseconds_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let ns be zonedDateTime.[[EpochNanoseconds]].
        let nanoseconds = zoned_date_time.epoch_nanoseconds();

        // 4. Let ms be floor(ℝ(ns) / 10**6).
        let milliseconds = big_floor(nanoseconds.big_integer(), &NANOSECONDS_PER_MILLISECOND);

        // 5. Return 𝔽(ms).
        Ok(Value::from_f64(big_int_algorithms::to_double(&milliseconds)))
    }

    // 6.3.18 get Temporal.ZonedDateTime.prototype.epochNanoseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.epochnanoseconds
    fn epoch_nanoseconds_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Return zonedDateTime.[[EpochNanoseconds]].
        Ok(Value::from_bigint(zoned_date_time.epoch_nanoseconds()))
    }

    // 6.3.19 get Temporal.ZonedDateTime.prototype.dayOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.dayofweek
    fn day_of_week_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[DayOfWeek]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&zoned_date_time, &iso_date_time).day_of_week,
        )))
    }

    // 6.3.20 get Temporal.ZonedDateTime.prototype.dayOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.dayofyear
    fn day_of_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[DayOfYear]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&zoned_date_time, &iso_date_time).day_of_year,
        )))
    }

    // 6.3.21 get Temporal.ZonedDateTime.prototype.weekOfYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.weekofyear
    fn week_of_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Let result be CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[WeekOfYear]].[[Week]].
        let result = calendar_date_of(&zoned_date_time, &iso_date_time).week_of_year.week;

        // 5. If result is undefined, return undefined.
        let Some(result) = result else {
            return Ok(Value::UNDEFINED);
        };

        // 6. Return 𝔽(result).
        Ok(Value::from_i32(i32::from(result)))
    }

    // 6.3.22 get Temporal.ZonedDateTime.prototype.yearOfWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.yearofweek
    fn year_of_week_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Let result be CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[WeekOfYear]].[[Year]].
        let result = calendar_date_of(&zoned_date_time, &iso_date_time).week_of_year.year;

        // 5. If result is undefined, return undefined.
        let Some(result) = result else {
            return Ok(Value::UNDEFINED);
        };

        // 6. Return 𝔽(result).
        Ok(Value::from_i32(result))
    }

    // 6.3.23 get Temporal.ZonedDateTime.prototype.hoursInDay, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.hoursinday
    fn hours_in_day_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let timeZone be zonedDateTime.[[TimeZone]].
        let time_zone = zoned_date_time.time_zone();
        let time_zone = Utf16View::of_string(&time_zone);

        // 4. Let isoDateTime be GetISODateTimeFor(timeZone, zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = get_iso_date_time_for(time_zone, zoned_date_time.epoch_nanoseconds().big_integer());

        // 5. Let today be isoDateTime.[[ISODate]].
        let today = iso_date_time.iso_date;

        // 6. Let tomorrow be AddDaysToISODate(today, 1).
        let tomorrow = add_days_to_iso_date(today, 1.0);

        // 7. Let todayNs be ? GetStartOfDay(timeZone, today).
        let today_nanoseconds = get_start_of_day(vm, time_zone, today)?;

        // 8. Let tomorrowNs be ? GetStartOfDay(timeZone, tomorrow).
        let tomorrow_nanoseconds = get_start_of_day(vm, time_zone, tomorrow)?;

        // 9. Let diff be TimeDurationFromEpochNanosecondsDifference(tomorrowNs, todayNs).
        let diff = time_duration_from_epoch_nanoseconds_difference(&tomorrow_nanoseconds, &today_nanoseconds);

        // 10. Return 𝔽(TotalTimeDuration(diff, HOUR)).
        Ok(Value::from_f64(total_time_duration(&diff, Unit::Hour).to_double()))
    }

    // 6.3.24 get Temporal.ZonedDateTime.prototype.daysInWeek, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.daysinweek
    fn days_in_week_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[DaysInWeek]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&zoned_date_time, &iso_date_time).days_in_week,
        )))
    }

    // 6.3.25 get Temporal.ZonedDateTime.prototype.daysInMonth, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.daysinmonth
    fn days_in_month_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[DaysInMonth]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&zoned_date_time, &iso_date_time).days_in_month,
        )))
    }

    // 6.3.26 get Temporal.ZonedDateTime.prototype.daysInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.daysinyear
    fn days_in_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[DaysInYear]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&zoned_date_time, &iso_date_time).days_in_year,
        )))
    }

    // 6.3.27 get Temporal.ZonedDateTime.prototype.monthsInYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.monthsinyear
    fn months_in_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return 𝔽(CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[MonthsInYear]]).
        Ok(Value::from_i32(i32::from(
            calendar_date_of(&zoned_date_time, &iso_date_time).months_in_year,
        )))
    }

    // 6.3.28 get Temporal.ZonedDateTime.prototype.inLeapYear, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.inleapyear
    fn in_leap_year_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return CalendarISOToDate(zonedDateTime.[[Calendar]], isoDateTime.[[ISODate]]).[[InLeapYear]].
        Ok(Value::from_bool(
            calendar_date_of(&zoned_date_time, &iso_date_time).in_leap_year,
        ))
    }

    // 6.3.29 get Temporal.ZonedDateTime.prototype.offsetNanoseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.offsetnanoseconds
    fn offset_nanoseconds_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Return 𝔽(GetOffsetNanosecondsFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]])).
        let time_zone = zoned_date_time.time_zone();
        let offset_nanoseconds = get_offset_nanoseconds_for(
            Utf16View::of_string(&time_zone),
            zoned_date_time.epoch_nanoseconds().big_integer(),
        );
        Ok(Value::from_f64(offset_nanoseconds as f64))
    }

    // 6.3.30 get Temporal.ZonedDateTime.prototype.offset, https://tc39.es/proposal-temporal/#sec-get-temporal.zoneddatetime.prototype.offset
    fn offset_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let offsetNanoseconds be GetOffsetNanosecondsFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let time_zone = zoned_date_time.time_zone();
        let offset_nanoseconds = get_offset_nanoseconds_for(
            Utf16View::of_string(&time_zone),
            zoned_date_time.epoch_nanoseconds().big_integer(),
        );

        // 4. Return FormatUTCOffsetNanoseconds(offsetNanoseconds).
        Ok(string_value(vm, &format_utc_offset_nanoseconds(offset_nanoseconds)))
    }

    // 6.3.31 Temporal.ZonedDateTime.prototype.with ( temporalZonedDateTimeLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.with
    fn with(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_zoned_date_time_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. If ? IsPartialTemporalObject(temporalZonedDateTimeLike) is false, throw a TypeError exception.
        if !is_partial_temporal_object(vm, temporal_zoned_date_time_like)? {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::TemporalObjectMustBePartialTemporalObject,
                &[],
            );
        }

        // 4. Let epochNs be zonedDateTime.[[EpochNanoseconds]].
        let epoch_nanoseconds = zoned_date_time.epoch_nanoseconds().big_integer().clone();

        // 5. Let timeZone be zonedDateTime.[[TimeZone]].
        let time_zone = zoned_date_time.time_zone();
        let time_zone_view = Utf16View::of_string(&time_zone);

        // 6. Let calendar be zonedDateTime.[[Calendar]].
        let calendar = zoned_date_time.calendar();
        let calendar_view = Utf16View::of_string(&calendar);

        // 7. Let offsetNanoseconds be GetOffsetNanosecondsFor(timeZone, epochNs).
        let offset_nanoseconds = get_offset_nanoseconds_for(time_zone_view, &epoch_nanoseconds);

        // 8. Let isoDateTime be GetISODateTimeFor(timeZone, epochNs).
        let iso_date_time = get_iso_date_time_for(time_zone_view, &epoch_nanoseconds);

        // 9. Let fields be ISODateToFields(calendar, isoDateTime.[[ISODate]], DATE).
        let mut fields = iso_date_to_fields(calendar_view, iso_date_time.iso_date, DateType::Date);

        // 10. Set fields.[[Hour]] to isoDateTime.[[Time]].[[Hour]].
        fields.hour = Some(iso_date_time.time.hour);

        // 11. Set fields.[[Minute]] to isoDateTime.[[Time]].[[Minute]].
        fields.minute = Some(iso_date_time.time.minute);

        // 12. Set fields.[[Second]] to isoDateTime.[[Time]].[[Second]].
        fields.second = Some(iso_date_time.time.second);

        // 13. Set fields.[[Millisecond]] to isoDateTime.[[Time]].[[Millisecond]].
        fields.millisecond = Some(iso_date_time.time.millisecond);

        // 14. Set fields.[[Microsecond]] to isoDateTime.[[Time]].[[Microsecond]].
        fields.microsecond = Some(iso_date_time.time.microsecond);

        // 15. Set fields.[[Nanosecond]] to isoDateTime.[[Time]].[[Nanosecond]].
        fields.nanosecond = Some(iso_date_time.time.nanosecond);

        // 16. Set fields.[[OffsetString]] to FormatUTCOffsetNanoseconds(offsetNanoseconds).
        fields.offset_string = Some(Utf16String::from_utf8(&format_utc_offset_nanoseconds(
            offset_nanoseconds,
        )));

        // 17. Let partialZonedDateTime be ? PrepareCalendarFields(calendar, temporalZonedDateTimeLike, « YEAR, MONTH, MONTH-CODE, DAY », « HOUR, MINUTE, SECOND, MILLISECOND, MICROSECOND, NANOSECOND, OFFSET », PARTIAL).
        let partial_zoned_date_time = prepare_calendar_fields(
            vm,
            calendar_view,
            &temporal_zoned_date_time_like.as_object(),
            &[
                CalendarField::Year,
                CalendarField::Month,
                CalendarField::MonthCode,
                CalendarField::Day,
            ],
            &[
                CalendarField::Hour,
                CalendarField::Minute,
                CalendarField::Second,
                CalendarField::Millisecond,
                CalendarField::Microsecond,
                CalendarField::Nanosecond,
                CalendarField::Offset,
            ],
            CalendarFieldListOrPartial::Partial,
        )?;

        // 18. Set fields to CalendarMergeFields(calendar, fields, partialZonedDateTime).
        let mut fields = calendar_merge_fields(calendar_view, &fields, &partial_zoned_date_time);

        // 19. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, options)?;

        // 20. Let disambiguation be ? GetTemporalDisambiguationOption(resolvedOptions).
        let disambiguation = get_temporal_disambiguation_option(vm, &resolved_options)?;

        // 21. Let offset be ? GetTemporalOffsetOption(resolvedOptions, PREFER).
        let offset = get_temporal_offset_option(vm, &resolved_options, OffsetOption::Prefer)?;

        // 22. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

        // 23. Let dateTimeResult be ? InterpretTemporalDateTimeFields(calendar, fields, overflow).
        let date_time_result = interpret_temporal_date_time_fields(vm, calendar_view, &mut fields, overflow)?;

        // 24. Let newOffsetNanoseconds be ! ParseDateTimeUTCOffset(fields.[[OffsetString]]).
        let new_offset_nanoseconds = parse_date_time_utc_offset(Utf16View::of_string(
            fields
                .offset_string
                .as_ref()
                .expect("the merged fields have an offset string"),
        ));

        // 25. Let epochNanoseconds be ? InterpretISODateTimeOffset(dateTimeResult.[[ISODate]], dateTimeResult.[[Time]], OPTION, newOffsetNanoseconds, timeZone, disambiguation, offset, MATCH-EXACTLY).
        let new_epoch_nanoseconds = interpret_iso_date_time_offset(
            vm,
            date_time_result.iso_date,
            TimeOrStartOfDay::Time(date_time_result.time),
            OffsetBehavior::Option,
            new_offset_nanoseconds,
            time_zone_view,
            disambiguation,
            offset,
            MatchBehavior::MatchExactly,
        )?;

        // 26. Return ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
        Ok(Value::from_object(
            create_temporal_zoned_date_time(vm, BigInt::create(vm, new_epoch_nanoseconds), time_zone, calendar, None)
                .must(),
        ))
    }

    // 6.3.32 Temporal.ZonedDateTime.prototype.withPlainTime ( [ plainTimeLike ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.withplaintime
    fn with_plain_time(vm: &Vm) -> ThrowCompletionOr<Value> {
        let plain_time_like = vm.argument(0);

        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let timeZone be zonedDateTime.[[TimeZone]].
        let time_zone = zoned_date_time.time_zone();

        // 4. Let calendar be zonedDateTime.[[Calendar]].
        let calendar = zoned_date_time.calendar();

        // 5. Let isoDateTime be GetISODateTimeFor(timeZone, zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = get_iso_date_time_for(
            Utf16View::of_string(&time_zone),
            zoned_date_time.epoch_nanoseconds().big_integer(),
        );

        // 6. If plainTimeLike is undefined, then
        let epoch_nanoseconds = if plain_time_like.is_undefined() {
            // a. Let epochNs be ? GetStartOfDay(timeZone, isoDateTime.[[ISODate]]).
            get_start_of_day(vm, Utf16View::of_string(&time_zone), iso_date_time.iso_date)?
        }
        // 7. Else,
        else {
            // a. Let plainTime be ? ToTemporalTime(plainTimeLike).
            let plain_time = to_temporal_time(vm, plain_time_like, Value::UNDEFINED)?;

            // b. Let resultISODateTime be CombineISODateAndTimeRecord(isoDateTime.[[ISODate]], plainTime.[[Time]]).
            let result_iso_date_time = combine_iso_date_and_time_record(iso_date_time.iso_date, plain_time.time());

            // c. Let epochNs be ? GetEpochNanosecondsFor(timeZone, resultISODateTime, COMPATIBLE).
            get_epoch_nanoseconds_for(
                vm,
                Utf16View::of_string(&time_zone),
                &result_iso_date_time,
                Disambiguation::Compatible,
            )?
        };

        // 8. Return ! CreateTemporalZonedDateTime(epochNs, timeZone, calendar).
        Ok(Value::from_object(
            create_temporal_zoned_date_time(vm, BigInt::create(vm, epoch_nanoseconds), time_zone, calendar, None)
                .must(),
        ))
    }

    // 6.3.33 Temporal.ZonedDateTime.prototype.withTimeZone ( timeZoneLike ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.withtimezone
    fn with_time_zone(vm: &Vm) -> ThrowCompletionOr<Value> {
        let time_zone_like = vm.argument(0);

        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let timeZone be ? ToTemporalTimeZoneIdentifier(timeZoneLike).
        let time_zone = to_temporal_time_zone_identifier(vm, time_zone_like)?;

        // 4. Return ! CreateTemporalZonedDateTime(zonedDateTime.[[EpochNanoseconds]], timeZone, zonedDateTime.[[Calendar]]).
        Ok(Value::from_object(
            create_temporal_zoned_date_time(
                vm,
                zoned_date_time.epoch_nanoseconds(),
                time_zone,
                zoned_date_time.calendar(),
                None,
            )
            .must(),
        ))
    }

    // 6.3.34 Temporal.ZonedDateTime.prototype.withCalendar ( calendarLike ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.withcalendar
    fn with_calendar(vm: &Vm) -> ThrowCompletionOr<Value> {
        let calendar_like = vm.argument(0);

        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let calendar be ? ToTemporalCalendarIdentifier(calendarLike).
        let calendar = to_temporal_calendar_identifier(vm, calendar_like)?;

        // 4. Return ! CreateTemporalZonedDateTime(zonedDateTime.[[EpochNanoseconds]], zonedDateTime.[[TimeZone]], calendar).
        Ok(Value::from_object(
            create_temporal_zoned_date_time(
                vm,
                zoned_date_time.epoch_nanoseconds(),
                zoned_date_time.time_zone(),
                calendar,
                None,
            )
            .must(),
        ))
    }

    // 6.3.35 Temporal.ZonedDateTime.prototype.add ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.add
    fn add(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_duration_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Return ? AddDurationToZonedDateTime(ADD, zonedDateTime, temporalDurationLike, options).
        Ok(Value::from_object(add_duration_to_zoned_date_time(
            vm,
            ArithmeticOperation::Add,
            &zoned_date_time,
            temporal_duration_like,
            options,
        )?))
    }

    // 6.3.36 Temporal.ZonedDateTime.prototype.subtract ( temporalDurationLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.subtract
    fn subtract(vm: &Vm) -> ThrowCompletionOr<Value> {
        let temporal_duration_like = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Return ? AddDurationToZonedDateTime(SUBTRACT, zonedDateTime, temporalDurationLike, options).
        Ok(Value::from_object(add_duration_to_zoned_date_time(
            vm,
            ArithmeticOperation::Subtract,
            &zoned_date_time,
            temporal_duration_like,
            options,
        )?))
    }

    // 6.3.37 Temporal.ZonedDateTime.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.until
    fn until(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Return ? DifferenceTemporalZonedDateTime(UNTIL, zonedDateTime, other, options).
        Ok(Value::from_object(difference_temporal_zoned_date_time(
            vm,
            DurationOperation::Until,
            &zoned_date_time,
            other,
            options,
        )?))
    }

    // 6.3.38 Temporal.ZonedDateTime.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.since
    fn since(vm: &Vm) -> ThrowCompletionOr<Value> {
        let other = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Return ? DifferenceTemporalZonedDateTime(SINCE, zonedDateTime, other, options).
        Ok(Value::from_object(difference_temporal_zoned_date_time(
            vm,
            DurationOperation::Since,
            &zoned_date_time,
            other,
            options,
        )?))
    }

    // 6.3.39 Temporal.ZonedDateTime.prototype.round ( roundTo ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.round
    fn round(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let round_to_value = vm.argument(0);

        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

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
            let maximum = maximum_temporal_duration_rounding_increment(smallest_unit_value);

            // b. Assert: maximum is not UNSET.
            let maximum = maximum.expect("a time unit has a maximum rounding increment");

            // c. Let inclusive be false.
            (maximum, false)
        };

        // 13. Perform ? ValidateTemporalRoundingIncrement(roundingIncrement, maximum, inclusive).
        validate_temporal_rounding_increment(vm, rounding_increment, maximum, inclusive)?;

        // 14. If smallestUnit is NANOSECOND and roundingIncrement = 1, then
        if smallest_unit_value == Unit::Nanosecond && rounding_increment == 1 {
            // a. Return ! CreateTemporalZonedDateTime(zonedDateTime.[[EpochNanoseconds]], zonedDateTime.[[TimeZone]], zonedDateTime.[[Calendar]]).
            return Ok(Value::from_object(
                create_temporal_zoned_date_time(
                    vm,
                    zoned_date_time.epoch_nanoseconds(),
                    zoned_date_time.time_zone(),
                    zoned_date_time.calendar(),
                    None,
                )
                .must(),
            ));
        }

        // 15. Let thisNs be zonedDateTime.[[EpochNanoseconds]].
        let this_nanoseconds = zoned_date_time.epoch_nanoseconds().big_integer().clone();

        // 16. Let timeZone be zonedDateTime.[[TimeZone]].
        let time_zone = zoned_date_time.time_zone();
        let time_zone_view = Utf16View::of_string(&time_zone);

        // 17. Let calendar be zonedDateTime.[[Calendar]].
        let calendar = zoned_date_time.calendar();

        // 18. Let isoDateTime be GetISODateTimeFor(timeZone, thisNs).
        let iso_date_time = get_iso_date_time_for(time_zone_view, &this_nanoseconds);

        // 19. If smallestUnit is day, then
        let epoch_nanoseconds = if smallest_unit_value == Unit::Day {
            // a. Let dateStart be isoDateTime.[[ISODate]].
            let date_start = iso_date_time.iso_date;

            // b. Let dateEnd be AddDaysToISODate(dateStart, 1).
            let date_end = add_days_to_iso_date(date_start, 1.0);

            // c. Let startNs be ? GetStartOfDay(timeZone, dateStart).
            let start_nanoseconds = get_start_of_day(vm, time_zone_view, date_start)?;

            // d. Assert: thisNs ≥ startNs.
            assert!(this_nanoseconds >= start_nanoseconds);

            // e. Let endNs be ? GetStartOfDay(timeZone, dateEnd).
            let end_nanoseconds = get_start_of_day(vm, time_zone_view, date_end)?;

            // f. Set thisNs to min(thisNs, endNs - 1).
            let this_nanoseconds_clamped = core::cmp::min(this_nanoseconds, &end_nanoseconds - 1);

            // g. Let dayLengthNs be ℝ(endNs - startNs).
            let day_length_nanoseconds = &end_nanoseconds - &start_nanoseconds;

            // h. Let dayProgressNs be TimeDurationFromEpochNanosecondsDifference(thisNs, startNs).
            let day_progress_nanoseconds =
                time_duration_from_epoch_nanoseconds_difference(&this_nanoseconds_clamped, &start_nanoseconds);

            // i. Let roundedDayNs be ! RoundTimeDurationToIncrement(dayProgressNs, dayLengthNs, roundingMode).
            let rounded_day_nanoseconds =
                round_time_duration_to_increment(vm, &day_progress_nanoseconds, &day_length_nanoseconds, rounding_mode)
                    .must();

            // j. Let epochNanoseconds be AddTimeDurationToEpochNanoseconds(roundedDayNs, startNs).
            add_time_duration_to_epoch_nanoseconds(&rounded_day_nanoseconds, &start_nanoseconds)
        }
        // 20. Else,
        else {
            // a. Let roundResult be RoundISODateTime(isoDateTime, roundingIncrement, smallestUnit, roundingMode).
            let round_result =
                round_iso_date_time(&iso_date_time, rounding_increment, smallest_unit_value, rounding_mode);

            // b. Let offsetNanoseconds be GetOffsetNanosecondsFor(timeZone, thisNs).
            let offset_nanoseconds = get_offset_nanoseconds_for(time_zone_view, &this_nanoseconds);

            // c. Let epochNanoseconds be ? InterpretISODateTimeOffset(roundResult.[[ISODate]], roundResult.[[Time]], OPTION, offsetNanoseconds, timeZone, COMPATIBLE, PREFER, MATCH-EXACTLY).
            interpret_iso_date_time_offset(
                vm,
                round_result.iso_date,
                TimeOrStartOfDay::Time(round_result.time),
                OffsetBehavior::Option,
                offset_nanoseconds as f64,
                time_zone_view,
                Disambiguation::Compatible,
                OffsetOption::Prefer,
                MatchBehavior::MatchExactly,
            )?
        };

        // 21. Return ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
        Ok(Value::from_object(
            create_temporal_zoned_date_time(vm, BigInt::create(vm, epoch_nanoseconds), time_zone, calendar, None)
                .must(),
        ))
    }

    // 6.3.40 Temporal.ZonedDateTime.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.equals
    fn equals(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Set other to ? ToTemporalZonedDateTime(other).
        let other = to_temporal_zoned_date_time(vm, vm.argument(0), Value::UNDEFINED)?;

        // 4. If zonedDateTime.[[EpochNanoseconds]] ≠ other.[[EpochNanoseconds]], return false.
        if zoned_date_time.epoch_nanoseconds().big_integer() != other.epoch_nanoseconds().big_integer() {
            return Ok(Value::from_bool(false));
        }

        // 5. If TimeZoneEquals(zonedDateTime.[[TimeZone]], other.[[TimeZone]]) is false, return false.
        if !time_zone_equals(
            Utf16View::of_string(&zoned_date_time.time_zone()),
            Utf16View::of_string(&other.time_zone()),
        ) {
            return Ok(Value::from_bool(false));
        }

        // 6. Return CalendarEquals(zonedDateTime.[[Calendar]], other.[[Calendar]]).
        Ok(Value::from_bool(calendar_equals(
            Utf16View::of_string(&zoned_date_time.calendar()),
            Utf16View::of_string(&other.calendar()),
        )))
    }

    // 6.3.41 Temporal.ZonedDateTime.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.tostring
    fn to_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, vm.argument(0))?;

        // 4. NOTE: The following steps read options and perform independent validation in alphabetical order
        //    (GetTemporalShowCalendarNameOption reads "calendarName", GetTemporalFractionalSecondDigitsOption reads
        //    "fractionalSecondDigits", GetTemporalShowOffsetOption reads "offset", and GetRoundingModeOption reads "roundingMode").

        // 5. Let showCalendar be ? GetTemporalShowCalendarNameOption(resolvedOptions).
        let show_calendar = get_temporal_show_calendar_name_option(vm, &resolved_options)?;

        // 6. Let digits be ? GetTemporalFractionalSecondDigitsOption(resolvedOptions).
        let digits = get_temporal_fractional_second_digits_option(vm, &resolved_options)?;

        // 7. Let showOffset be ? GetTemporalShowOffsetOption(resolvedOptions).
        let show_offset = get_temporal_show_offset_option(vm, &resolved_options)?;

        // 8. Let roundingMode be ? GetRoundingModeOption(resolvedOptions, TRUNC).
        let rounding_mode = get_rounding_mode_option(vm, &resolved_options, RoundingMode::Trunc)?;

        // 9. Let smallestUnit be ? GetTemporalUnitValuedOption(resolvedOptions, "smallestUnit", UNSET).
        let smallest_unit =
            get_temporal_unit_valued_option(vm, &resolved_options, &vm.names.smallestUnit, UnitDefault::Unset)?;

        // 10. Let showTimeZone be ? GetTemporalShowTimeZoneNameOption(resolvedOptions).
        let show_time_zone = get_temporal_show_time_zone_name_option(vm, &resolved_options)?;

        // 11. Perform ? ValidateTemporalUnitValue(smallestUnit, TIME).
        validate_temporal_unit_value(vm, &vm.names.smallestUnit, smallest_unit, UnitGroup::Time, &[])?;

        // 12. If smallestUnit is HOUR, throw a RangeError exception.
        if smallest_unit == UnitValue::Unit(Unit::Hour) {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::OptionIsNotValidValue,
                &[&temporal_unit_to_string(Unit::Hour), &vm.names.smallestUnit],
            );
        }

        // 13. Let precision be ToSecondsStringPrecisionRecord(smallestUnit, digits).
        let precision = to_seconds_string_precision_record(smallest_unit, digits);

        // 14. Return TemporalZonedDateTimeToString(zonedDateTime, precision.[[Precision]], showCalendar, showTimeZone, showOffset, precision.[[Increment]], precision.[[Unit]], roundingMode).
        Ok(string_value(
            vm,
            &temporal_zoned_date_time_to_string(
                &zoned_date_time,
                precision.precision,
                show_calendar,
                show_time_zone,
                show_offset,
                u64::from(precision.increment),
                precision.unit,
                rounding_mode,
            ),
        ))
    }

    // 6.3.42 Temporal.ZonedDateTime.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.tolocalestring
    // 15.11.8.1 Temporal.ZonedDateTime.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sup-temporal.zoneddatetime.prototype.tolocalestring
    fn to_locale_string(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let locales = vm.argument(0);
        let options = vm.argument(1);

        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let dateTimeFormat be ? CreateDateTimeFormat(%Intl.DateTimeFormat%, locales, options, ANY, ALL, zonedDateTime.[[TimeZone]]).
        let time_zone = zoned_date_time.time_zone();
        let date_time_format = create_date_time_format(
            vm,
            realm.intrinsics().intl_date_time_format_constructor(vm),
            locales,
            options,
            OptionRequired::Any,
            OptionDefaults::All,
            Some(Utf16View::of_string(&time_zone)),
        )?;

        // 4. If zonedDateTime.[[Calendar]] is not "iso8601" and CalendarEquals(zonedDateTime.[[Calendar]], dateTimeFormat.[[Calendar]]) is false, throw a RangeError exception.
        let calendar = zoned_date_time.calendar();
        let date_time_format_calendar = date_time_format.calendar();
        if Utf16View::of_string(&calendar) != ISO8601_CALENDAR
            && !calendar_equals(
                Utf16View::of_string(&calendar),
                Utf16View::of_string(&date_time_format_calendar),
            )
        {
            return vm.throw_completion_with_utf16_message(
                ErrorKind::RangeError,
                ErrorType::IntlTemporalInvalidCalendar.utf16_message(&[
                    Utf16View::Ascii(b"Temporal.ZonedDateTime"),
                    Utf16View::of_string(&calendar),
                    Utf16View::of_string(&date_time_format_calendar),
                ]),
            );
        }

        // 5. Let instant be ! CreateTemporalInstant(zonedDateTime.[[EpochNanoseconds]]).
        let instant = create_temporal_instant(vm, zoned_date_time.epoch_nanoseconds(), None).must();

        // 6. Return ? FormatDateTime(dateTimeFormat, instant).
        let formatted = format_date_time(vm, &date_time_format, &FormattableDateTime::Instant(instant))?;
        Ok(Value::from_string(PrimitiveString::create(vm, formatted)))
    }

    // 6.3.43 Temporal.ZonedDateTime.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.tojson
    fn to_json(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Return TemporalZonedDateTimeToString(zonedDateTime, AUTO, AUTO, AUTO, AUTO).
        Ok(string_value(
            vm,
            &temporal_zoned_date_time_to_string(
                &zoned_date_time,
                SecondsPrecision::Auto,
                ShowCalendar::Auto,
                ShowTimeZoneName::Auto,
                ShowOffset::Auto,
                1,
                Unit::Nanosecond,
                RoundingMode::Trunc,
            ),
        ))
    }

    // 6.3.44 Temporal.ZonedDateTime.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.valueof
    fn value_of(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Throw a TypeError exception.
        vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::Convert,
            &[&"Temporal.ZonedDateTime", &"a primitive value"],
        )
    }

    // 6.3.45 Temporal.ZonedDateTime.prototype.startOfDay ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.startofday
    fn start_of_day(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let timeZone be zonedDateTime.[[TimeZone]].
        let time_zone = zoned_date_time.time_zone();

        // 4. Let calendar be zonedDateTime.[[Calendar]].
        let calendar = zoned_date_time.calendar();

        // 5. Let isoDateTime be GetISODateTimeFor(timeZone, zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = get_iso_date_time_for(
            Utf16View::of_string(&time_zone),
            zoned_date_time.epoch_nanoseconds().big_integer(),
        );

        // 6. Let epochNanoseconds be ? GetStartOfDay(timeZone, isoDateTime.[[ISODate]]).
        let epoch_nanoseconds = get_start_of_day(vm, Utf16View::of_string(&time_zone), iso_date_time.iso_date)?;

        // 7. Return ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
        Ok(Value::from_object(
            create_temporal_zoned_date_time(vm, BigInt::create(vm, epoch_nanoseconds), time_zone, calendar, None)
                .must(),
        ))
    }

    // 6.3.46 Temporal.ZonedDateTime.prototype.getTimeZoneTransition ( directionParam ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.gettimezonetransition
    fn get_time_zone_transition(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let direction_param_value = vm.argument(0);

        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let timeZone be zonedDateTime.[[TimeZone]].
        let time_zone = zoned_date_time.time_zone();

        // 4. If directionParam is undefined, throw a TypeError exception.
        if direction_param_value.is_undefined() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::IsUndefined,
                &[&"Transition direction parameter"],
            );
        }

        // 5. If directionParam is a String, then
        let direction_param = if direction_param_value.is_string() {
            // a. Let paramString be directionParam.
            let param_string = direction_param_value;

            // b. Set directionParam to OrdinaryObjectCreate(null).
            let direction_param = Object::create(vm, realm, None);

            // c. Perform ! CreateDataPropertyOrThrow(directionParam, "direction", paramString).
            direction_param
                .create_data_property_or_throw(vm, &vm.names.direction, param_string)
                .must();
            direction_param
        }
        // 6. Else,
        else {
            // a. Set directionParam to ? GetOptionsObject(directionParam).
            get_options_object(vm, direction_param_value)?
        };

        // 7. Let direction be ? GetDirectionOption(directionParam).
        let direction = get_direction_option(vm, &direction_param)?;

        // 8. If IsOffsetTimeZoneIdentifier(timeZone) is true, return null.
        if is_offset_time_zone_identifier(Utf16View::of_string(&time_zone)) {
            return Ok(Value::NULL);
        }

        let epoch_nanoseconds = zoned_date_time.epoch_nanoseconds().big_integer().clone();

        let transition = match direction {
            // 9. If direction is NEXT, then
            Direction::Next => {
                // a. Let transition be GetNamedTimeZoneNextTransition(timeZone, zonedDateTime.[[EpochNanoseconds]]).
                get_named_time_zone_next_transition(Utf16View::of_string(&time_zone), &epoch_nanoseconds)
            }
            // 10. Else,
            Direction::Previous => {
                // a. Assert: direction is PREVIOUS.
                // b. Let transition be GetNamedTimeZonePreviousTransition(timeZone, zonedDateTime.[[EpochNanoseconds]]).
                get_named_time_zone_previous_transition(Utf16View::of_string(&time_zone), &epoch_nanoseconds)
            }
        };

        // 11. If transition is null, return null.
        let Some(transition) = transition else {
            return Ok(Value::NULL);
        };

        // 12. Return ! CreateTemporalZonedDateTime(transition, timeZone, zonedDateTime.[[Calendar]]).
        Ok(Value::from_object(
            create_temporal_zoned_date_time(
                vm,
                BigInt::create(vm, transition),
                time_zone,
                zoned_date_time.calendar(),
                None,
            )
            .must(),
        ))
    }

    // 6.3.47 Temporal.ZonedDateTime.prototype.toInstant ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toinstant
    fn to_instant(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Return ! CreateTemporalInstant(zonedDateTime.[[EpochNanoseconds]]).
        Ok(Value::from_object(
            create_temporal_instant(vm, zoned_date_time.epoch_nanoseconds(), None).must(),
        ))
    }

    // 6.3.48 Temporal.ZonedDateTime.prototype.toPlainDate ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplaindate
    fn to_plain_date(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return ! CreateTemporalDate(isoDateTime.[[ISODate]], zonedDateTime.[[Calendar]]).
        Ok(Value::from_object(
            create_temporal_date(vm, iso_date_time.iso_date, zoned_date_time.calendar(), None).must(),
        ))
    }

    // 6.3.49 Temporal.ZonedDateTime.prototype.toPlainTime ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplaintime
    fn to_plain_time(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return ! CreateTemporalTime(isoDateTime.[[Time]]).
        Ok(Value::from_object(
            create_temporal_time(vm, iso_date_time.time, None).must(),
        ))
    }

    // 6.3.50 Temporal.ZonedDateTime.prototype.toPlainDateTime ( ), https://tc39.es/proposal-temporal/#sec-temporal.zoneddatetime.prototype.toplaindatetime
    fn to_plain_date_time(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let zonedDateTime be the this value.
        // 2. Perform ? RequireInternalSlot(zonedDateTime, [[InitializedTemporalZonedDateTime]]).
        let zoned_date_time = typed_this_zoned_date_time(vm)?;

        // 3. Let isoDateTime be GetISODateTimeFor(zonedDateTime.[[TimeZone]], zonedDateTime.[[EpochNanoseconds]]).
        let iso_date_time = iso_date_time_of(&zoned_date_time);

        // 4. Return ! CreateTemporalDateTime(isoDateTime, zonedDateTime.[[Calendar]]).
        Ok(Value::from_object(
            create_temporal_date_time(vm, &iso_date_time, zoned_date_time.calendar(), None).must(),
        ))
    }
}
