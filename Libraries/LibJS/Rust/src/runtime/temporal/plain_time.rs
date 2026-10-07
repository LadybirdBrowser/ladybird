/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Temporal.PlainTime objects and the Time Record operations.

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    RoundingMode, big_floor, big_modulo, get_options_object, modulo, ordinary_create_from_constructor_of,
};
use crate::runtime::big_int::SignedBigInteger;
use crate::runtime::big_int_algorithms;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::temporal::abstract_operations::{
    ArithmeticOperation, DurationOperation, Overflow, SecondsPrecision, Unit, UnitGroup, format_time_string,
    get_difference_settings, get_temporal_overflow_option, parse_iso_date_time, round_number_to_increment,
    temporal_unit_length_in_nanoseconds, to_integer_with_truncation,
};
use crate::runtime::temporal::duration::{
    Duration, big_integer_from_double, combine_date_and_time_duration, create_negated_temporal_duration,
    round_time_duration, temporal_duration_from_internal, time_duration_from_components, to_internal_duration_record,
    to_temporal_duration, zero_date_duration,
};
use crate::runtime::temporal::instant::{
    HOURS_PER_DAY, MICROSECONDS_PER_MILLISECOND, MILLISECONDS_PER_SECOND, MINUTES_PER_HOUR, NANOSECONDS_PER_DAY,
    NANOSECONDS_PER_MICROSECOND, SECONDS_PER_MINUTE,
};
use crate::runtime::temporal::iso_records::{Time, TimeDuration, TimeOrStartOfDay};
use crate::runtime::temporal::iso8601::Production;
use crate::runtime::temporal::plain_date_time::PlainDateTime;
use crate::runtime::temporal::time_zone::get_iso_date_time_for;
use crate::runtime::temporal::zoned_date_time::ZonedDateTime;
use crate::utf16::Utf16View;

// 4 Temporal.PlainTime Objects, https://tc39.es/proposal-temporal/#sec-temporal-plaintime-objects
#[repr(C)]
#[derive(Trace)]
pub struct PlainTime {
    base: Object,
    #[gc(untraced)]
    time: Time, // [[Time]]
}

define_cell!(PlainTime, Object, extends: [Object]);

impl core::ops::Deref for PlainTime {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl PlainTime {
    fn new(vm: &Vm, time: Time, prototype: Gc<Object>) -> PlainTime {
        PlainTime {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            time,
        }
    }

    pub fn time(&self) -> Time {
        self.time
    }
}

// Table 5: TemporalTimeLike Record Fields, https://tc39.es/proposal-temporal/#table-temporal-temporaltimelike-record-fields
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct TemporalTimeLike {
    pub hour: Option<f64>,
    pub minute: Option<f64>,
    pub second: Option<f64>,
    pub millisecond: Option<f64>,
    pub microsecond: Option<f64>,
    pub nanosecond: Option<f64>,
}

impl TemporalTimeLike {
    pub fn zero() -> TemporalTimeLike {
        TemporalTimeLike {
            hour: Some(0.0),
            minute: Some(0.0),
            second: Some(0.0),
            millisecond: Some(0.0),
            microsecond: Some(0.0),
            nanosecond: Some(0.0),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Completeness {
    Complete,
    Partial,
}

// 4.5.2 CreateTimeRecord ( hour, minute, second, millisecond, microsecond, nanosecond [ , deltaDays ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtimerecord
pub fn create_time_record(
    hour: f64,
    minute: f64,
    second: f64,
    millisecond: f64,
    microsecond: f64,
    nanosecond: f64,
    delta_days: f64,
) -> Time {
    // 1. If deltaDays is not present, set deltaDays to 0.
    // 2. Assert: IsValidTime(hour, minute, second, millisecond, microsecond, nanosecond).
    assert!(is_valid_time(
        hour,
        minute,
        second,
        millisecond,
        microsecond,
        nanosecond
    ));

    // 3. Return Time Record { [[Days]]: deltaDays, [[Hour]]: hour, [[Minute]]: minute, [[Second]]: second, [[Millisecond]]: millisecond, [[Microsecond]]: microsecond, [[Nanosecond]]: nanosecond  }.
    Time {
        days: delta_days,
        hour: hour as u8,
        minute: minute as u8,
        second: second as u8,
        millisecond: millisecond as u16,
        microsecond: microsecond as u16,
        nanosecond: nanosecond as u16,
    }
}

// 4.5.3 MidnightTimeRecord ( ), https://tc39.es/proposal-temporal/#sec-temporal-midnighttimerecord
pub fn midnight_time_record() -> Time {
    // 1. Return Time Record { [[Days]]: 0, [[Hour]]: 0, [[Minute]]: 0, [[Second]]: 0, [[Millisecond]]: 0, [[Microsecond]]: 0, [[Nanosecond]]: 0  }.
    Time::default()
}

// 4.5.4 NoonTimeRecord ( ), https://tc39.es/proposal-temporal/#sec-temporal-noontimerecord
pub fn noon_time_record() -> Time {
    // 1. Return Time Record { [[Days]]: 0, [[Hour]]: 12, [[Minute]]: 0, [[Second]]: 0, [[Millisecond]]: 0, [[Microsecond]]: 0, [[Nanosecond]]: 0  }.
    Time {
        hour: 12,
        ..Time::default()
    }
}

// 4.5.5 DifferenceTime ( time1, time2 ), https://tc39.es/proposal-temporal/#sec-temporal-differencetime
pub fn difference_time(time1: &Time, time2: &Time) -> TimeDuration {
    // 1. Let hours be time2.[[Hour]] - time1.[[Hour]].
    let hours = f64::from(time2.hour) - f64::from(time1.hour);

    // 2. Let minutes be time2.[[Minute]] - time1.[[Minute]].
    let minutes = f64::from(time2.minute) - f64::from(time1.minute);

    // 3. Let seconds be time2.[[Second]] - time1.[[Second]].
    let seconds = f64::from(time2.second) - f64::from(time1.second);

    // 4. Let milliseconds be time2.[[Millisecond]] - time1.[[Millisecond]].
    let milliseconds = f64::from(time2.millisecond) - f64::from(time1.millisecond);

    // 5. Let microseconds be time2.[[Microsecond]] - time1.[[Microsecond]].
    let microseconds = f64::from(time2.microsecond) - f64::from(time1.microsecond);

    // 6. Let nanoseconds be time2.[[Nanosecond]] - time1.[[Nanosecond]].
    let nanoseconds = f64::from(time2.nanosecond) - f64::from(time1.nanosecond);

    // 7. Let timeDuration be TimeDurationFromComponents(hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
    let time_duration = time_duration_from_components(hours, minutes, seconds, milliseconds, microseconds, nanoseconds);

    // 8. Assert: abs(timeDuration) < nsPerDay.
    assert!(time_duration.magnitude() < NANOSECONDS_PER_DAY.magnitude());

    // 9. Return timeDuration.
    time_duration
}

// 4.5.6 ToTemporalTime ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaltime
pub fn to_temporal_time(vm: &Vm, item: Value, options: Value) -> ThrowCompletionOr<Gc<PlainTime>> {
    // 1. If options is not present, set options to undefined.

    // 2. If item is an Object, then
    let time = if item.is_object() {
        let object = item.as_object();

        // a. If item has an [[InitializedTemporalTime]] internal slot, then
        if let Some(plain_time) = object.downcast::<PlainTime>() {
            // i. Let resolvedOptions be ? GetOptionsObject(options).
            let resolved_options = get_options_object(vm, options)?;

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            get_temporal_overflow_option(vm, &resolved_options)?;

            // iii. Return ! CreateTemporalTime(item.[[Time]]).
            return Ok(create_temporal_time(vm, plain_time.time(), None).must());
        }

        // b. If item has an [[InitializedTemporalDateTime]] internal slot, then
        if let Some(plain_date_time) = object.downcast::<PlainDateTime>() {
            // i. Let resolvedOptions be ? GetOptionsObject(options).
            let resolved_options = get_options_object(vm, options)?;

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            get_temporal_overflow_option(vm, &resolved_options)?;

            // iii. Return ! CreateTemporalTime(item.[[ISODateTime]].[[Time]]).
            return Ok(create_temporal_time(vm, plain_date_time.iso_date_time().time, None).must());
        }

        // c. If item has an [[InitializedTemporalZonedDateTime]] internal slot, then
        if let Some(zoned_date_time) = object.downcast::<ZonedDateTime>() {
            // i. Let isoDateTime be GetISODateTimeFor(item.[[TimeZone]], item.[[EpochNanoseconds]]).
            let time_zone = zoned_date_time.time_zone();
            let iso_date_time = get_iso_date_time_for(
                Utf16View::of_string(&time_zone),
                zoned_date_time.epoch_nanoseconds().big_integer(),
            );

            // ii. Let resolvedOptions be ? GetOptionsObject(options).
            let resolved_options = get_options_object(vm, options)?;

            // iii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            get_temporal_overflow_option(vm, &resolved_options)?;

            // iv. Return ! CreateTemporalTime(isoDateTime.[[Time]]).
            return Ok(create_temporal_time(vm, iso_date_time.time, None).must());
        }

        // d. Let result be ? ToTemporalTimeRecord(item).
        let result = to_temporal_time_record(vm, &object, Completeness::Complete)?;

        // e. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, options)?;

        // f. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

        // g. Set result to ? RegulateTime(result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]], overflow).
        let field = |value: Option<f64>| value.expect("a complete record has every field");
        regulate_time(
            vm,
            field(result.hour),
            field(result.minute),
            field(result.second),
            field(result.millisecond),
            field(result.microsecond),
            field(result.nanosecond),
            overflow,
        )?
    }
    // 3. Else,
    else {
        // a. If item is not a String, throw a TypeError exception.
        if !item.is_string() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::TemporalInvalidPlainTime, &[]);
        }

        // b. Let parseResult be ? ParseISODateTime(item, « TemporalTimeString »).
        let item_string = item.as_string().utf16_string();
        let parse_result = parse_iso_date_time(
            vm,
            Utf16View::of_string(&item_string),
            &[Production::TemporalTimeString],
        )?;

        // c. Assert: parseResult.[[Time]] is not START-OF-DAY.
        // d. Set result to parseResult.[[Time]].
        let TimeOrStartOfDay::Time(time) = parse_result.time else {
            panic!("a time string has a time");
        };

        // e. NOTE: A successful parse using TemporalTimeString guarantees absence of ambiguity with respect to any
        //    ISO 8601 date-only, year-month, or month-day representation.

        // f. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, options)?;

        // g. Perform ? GetTemporalOverflowOption(resolvedOptions).
        get_temporal_overflow_option(vm, &resolved_options)?;

        time
    };

    // 4. Return ! CreateTemporalTime(result).
    Ok(create_temporal_time(vm, time, None).must())
}

// 4.5.7 ToTimeRecordOrMidnight ( item ), https://tc39.es/proposal-temporal/#sec-temporal-totimerecordormidnight
pub fn to_time_record_or_midnight(vm: &Vm, item: Value) -> ThrowCompletionOr<Time> {
    // 1. If item is undefined, return MidnightTimeRecord().
    if item.is_undefined() {
        return Ok(midnight_time_record());
    }

    // 2. Let plainTime be ? ToTemporalTime(item).
    let plain_time = to_temporal_time(vm, item, Value::UNDEFINED)?;

    // 3. Return plainTime.[[Time]].
    Ok(plain_time.time())
}

/// AK::clamp(), which keeps a value that is neither below the minimum nor above the maximum as it is.
fn clamp(value: f64, minimum: f64, maximum: f64) -> f64 {
    if value < minimum {
        return minimum;
    }
    if value > maximum {
        return maximum;
    }
    value
}

// 4.5.8 RegulateTime ( hour, minute, second, millisecond, microsecond, nanosecond, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-regulatetime
#[allow(clippy::too_many_arguments)]
pub fn regulate_time(
    vm: &Vm,
    mut hour: f64,
    mut minute: f64,
    mut second: f64,
    mut millisecond: f64,
    mut microsecond: f64,
    mut nanosecond: f64,
    overflow: Overflow,
) -> ThrowCompletionOr<Time> {
    match overflow {
        // 1. If overflow is CONSTRAIN, then
        Overflow::Constrain => {
            // a. Set hour to the result of clamping hour between 0 and 23.
            hour = clamp(hour, 0.0, 23.0);

            // b. Set minute to the result of clamping minute between 0 and 59.
            minute = clamp(minute, 0.0, 59.0);

            // c. Set second to the result of clamping second between 0 and 59.
            second = clamp(second, 0.0, 59.0);

            // d. Set millisecond to the result of clamping millisecond between 0 and 999.
            millisecond = clamp(millisecond, 0.0, 999.0);

            // e. Set microsecond to the result of clamping microsecond between 0 and 999.
            microsecond = clamp(microsecond, 0.0, 999.0);

            // f. Set nanosecond to the result of clamping nanosecond between 0 and 999.
            nanosecond = clamp(nanosecond, 0.0, 999.0);
        }
        // 2. Else,
        Overflow::Reject => {
            // a. Assert: overflow is REJECT.
            // b. If IsValidTime(hour, minute, second, millisecond, microsecond, nanosecond) is false, throw a RangeError exception.
            if !is_valid_time(hour, minute, second, millisecond, microsecond, nanosecond) {
                return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainTime, &[]);
            }
        }
    }

    // 3. Return CreateTimeRecord(hour, minute, second, millisecond, microsecond,nanosecond).
    Ok(create_time_record(
        hour,
        minute,
        second,
        millisecond,
        microsecond,
        nanosecond,
        0.0,
    ))
}

// 4.5.9 IsValidTime ( hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidtime
pub fn is_valid_time(hour: f64, minute: f64, second: f64, millisecond: f64, microsecond: f64, nanosecond: f64) -> bool {
    // 1. If hour < 0 or hour > 23, return false.
    if hour < 0.0 || hour > 23.0 {
        return false;
    }

    // 2. If minute < 0 or minute > 59, return false.
    if minute < 0.0 || minute > 59.0 {
        return false;
    }

    // 3. If second < 0 or second > 59, return false.
    if second < 0.0 || second > 59.0 {
        return false;
    }

    // 4. If millisecond < 0 or millisecond > 999, return false.
    if millisecond < 0.0 || millisecond > 999.0 {
        return false;
    }

    // 5. If microsecond < 0 or microsecond > 999, return false.
    if microsecond < 0.0 || microsecond > 999.0 {
        return false;
    }

    // 6. If nanosecond < 0 or nanosecond > 999, return false.
    if nanosecond < 0.0 || nanosecond > 999.0 {
        return false;
    }

    // 7. Return true.
    true
}

// 4.5.10 BalanceTime ( hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/proposal-temporal/#sec-temporal-balancetime
pub fn balance_time(
    mut hour: f64,
    mut minute: f64,
    mut second: f64,
    mut millisecond: f64,
    mut microsecond: f64,
    mut nanosecond: f64,
) -> Time {
    // 1. Set microsecond to microsecond + floor(nanosecond / 1000).
    microsecond += (nanosecond / 1000.0).floor();

    // 2. Set nanosecond to nanosecond modulo 1000.
    nanosecond = modulo(nanosecond, 1000.0);

    // 3. Set millisecond to millisecond + floor(microsecond / 1000).
    millisecond += (microsecond / 1000.0).floor();

    // 4. Set microsecond to microsecond modulo 1000.
    microsecond = modulo(microsecond, 1000.0);

    // 5. Set second to second + floor(millisecond / 1000).
    second += (millisecond / 1000.0).floor();

    // 6. Set millisecond to millisecond modulo 1000.
    millisecond = modulo(millisecond, 1000.0);

    // 7. Set minute to minute + floor(second / 60).
    minute += (second / 60.0).floor();

    // 8. Set second to second modulo 60.
    second = modulo(second, 60.0);

    // 9. Set hour to hour + floor(minute / 60).
    hour += (minute / 60.0).floor();

    // 10. Set minute to minute modulo 60.
    minute = modulo(minute, 60.0);

    // 11. Let deltaDays be floor(hour / 24).
    let delta_days = (hour / 24.0).floor();

    // 12. Set hour to hour modulo 24.
    hour = modulo(hour, 24.0);

    // 13. Return CreateTimeRecord(hour, minute, second, millisecond, microsecond, nanosecond, deltaDays).
    create_time_record(hour, minute, second, millisecond, microsecond, nanosecond, delta_days)
}

// 4.5.10 BalanceTime ( hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/proposal-temporal/#sec-temporal-balancetime
pub fn balance_time_with_big_nanoseconds(
    hour: f64,
    minute: f64,
    second: f64,
    millisecond: f64,
    microsecond: f64,
    nanosecond_value: &SignedBigInteger,
) -> Time {
    let to_double = big_int_algorithms::to_double;

    // 1. Set microsecond to microsecond + floor(nanosecond / 1000).
    let microsecond_value =
        big_integer_from_double(microsecond) + big_floor(nanosecond_value, &NANOSECONDS_PER_MICROSECOND);

    // 2. Set nanosecond to nanosecond modulo 1000.
    let nanosecond = to_double(&big_modulo(nanosecond_value, &NANOSECONDS_PER_MICROSECOND));

    // 3. Set millisecond to millisecond + floor(microsecond / 1000).
    let millisecond_value =
        big_integer_from_double(millisecond) + big_floor(&microsecond_value, &MICROSECONDS_PER_MILLISECOND);

    // 4. Set microsecond to microsecond modulo 1000.
    let microsecond = to_double(&big_modulo(&microsecond_value, &MICROSECONDS_PER_MILLISECOND));

    // 5. Set second to second + floor(millisecond / 1000).
    let second_value = big_integer_from_double(second) + big_floor(&millisecond_value, &MILLISECONDS_PER_SECOND);

    // 6. Set millisecond to millisecond modulo 1000.
    let millisecond = to_double(&big_modulo(&millisecond_value, &MILLISECONDS_PER_SECOND));

    // 7. Set minute to minute + floor(second / 60).
    let minute_value = big_integer_from_double(minute) + big_floor(&second_value, &SECONDS_PER_MINUTE);

    // 8. Set second to second modulo 60.
    let second = to_double(&big_modulo(&second_value, &SECONDS_PER_MINUTE));

    // 9. Set hour to hour + floor(minute / 60).
    let hour_value = big_integer_from_double(hour) + big_floor(&minute_value, &MINUTES_PER_HOUR);

    // 10. Set minute to minute modulo 60.
    let minute = to_double(&big_modulo(&minute_value, &MINUTES_PER_HOUR));

    // 11. Let deltaDays be floor(hour / 24).
    let delta_days = to_double(&big_floor(&hour_value, &HOURS_PER_DAY));

    // 12. Set hour to hour modulo 24.
    let hour = to_double(&big_modulo(&hour_value, &HOURS_PER_DAY));

    // 13. Return CreateTimeRecord(hour, minute, second, millisecond, microsecond, nanosecond, deltaDays).
    create_time_record(hour, minute, second, millisecond, microsecond, nanosecond, delta_days)
}

// 4.5.11 CreateTemporalTime ( time [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporaltime
pub fn create_temporal_time(
    vm: &Vm,
    time: Time,
    new_target: Option<Gc<FunctionObject>>,
) -> ThrowCompletionOr<Gc<PlainTime>> {
    let realm = vm.current_realm().expect("CreateTemporalTime runs in a realm");

    // 1. If newTarget is not present, set newTarget to %Temporal.PlainTime%.
    let new_target = new_target.unwrap_or_else(|| realm.intrinsics().temporal_plain_time_constructor(vm));

    // 2. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainTime.prototype%", « [[InitializedTemporalTime]], [[Time]] »).
    // 3. Set object.[[Time]] to time.
    let object = ordinary_create_from_constructor_of(
        vm,
        realm,
        new_target,
        Intrinsics::temporal_plain_time_prototype,
        |prototype| PlainTime::new(vm, time, prototype),
    )?;

    // 4. Return object.
    Ok(object)
}

// 4.5.12 ToTemporalTimeRecord ( temporalTimeLike [ , completeness ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaltimerecord
pub fn to_temporal_time_record(
    vm: &Vm,
    temporal_time_like: &Object,
    completeness: Completeness,
) -> ThrowCompletionOr<TemporalTimeLike> {
    // 1. If completeness is not present, set completeness to COMPLETE.

    // 2. If completeness is COMPLETE, then
    //     a. Let result be a new TemporalTimeLike Record with each field set to 0.
    // 3. Else,
    //     a. Let result be a new TemporalTimeLike Record with each field set to UNSET.
    let mut result = if completeness == Completeness::Complete {
        TemporalTimeLike::zero()
    } else {
        TemporalTimeLike::default()
    };

    // 4. Let any be false.
    let mut any = false;

    let mut apply_field = |key: &PropertyKey, result_field: &mut Option<f64>| -> ThrowCompletionOr<()> {
        let field = temporal_time_like.get(vm, key)?;
        if field.is_undefined() {
            return Ok(());
        }

        *result_field = Some(to_integer_with_truncation(
            vm,
            field,
            ErrorType::TemporalInvalidTimeLikeField,
            &[&field, key],
        )?);
        any = true;

        Ok(())
    };
    let names = &vm.names;

    // 5. Let hour be ? Get(temporalTimeLike, "hour").
    // 6. If hour is not undefined, then
    //     a. Set result.[[Hour]] to ? ToIntegerWithTruncation(hour).
    //     b. Set any to true.
    apply_field(&names.hour, &mut result.hour)?;

    // 7. Let microsecond be ? Get(temporalTimeLike, "microsecond").
    // 8. If microsecond is not undefined, then
    //     a. Set result.[[Microsecond]] to ? ToIntegerWithTruncation(microsecond).
    //     b. Set any to true.
    apply_field(&names.microsecond, &mut result.microsecond)?;

    // 9. Let millisecond be ? Get(temporalTimeLike, "millisecond").
    // 10. If millisecond is not undefined, then
    //     a. Set result.[[Millisecond]] to ? ToIntegerWithTruncation(millisecond).
    //     b. Set any to true.
    apply_field(&names.millisecond, &mut result.millisecond)?;

    // 11. Let minute be ? Get(temporalTimeLike, "minute").
    // 12. If minute is not undefined, then
    //     a. Set result.[[Minute]] to ? ToIntegerWithTruncation(minute).
    //     b. Set any to true.
    apply_field(&names.minute, &mut result.minute)?;

    // 13. Let nanosecond be ? Get(temporalTimeLike, "nanosecond").
    // 14. If nanosecond is not undefined, then
    //     a. Set result.[[Nanosecond]] to ? ToIntegerWithTruncation(nanosecond).
    //     b. Set any to true.
    apply_field(&names.nanosecond, &mut result.nanosecond)?;

    // 15. Let second be ? Get(temporalTimeLike, "second").
    // 16. If second is not undefined, then
    //     a. Set result.[[Second]] to ? ToIntegerWithTruncation(second).
    //     b. Set any to true.
    apply_field(&names.second, &mut result.second)?;

    // 17. If any is false, throw a TypeError exception.
    if !any {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::TemporalInvalidTime, &[]);
    }

    // 18. Return result.
    Ok(result)
}

// 4.5.13 TimeRecordToString ( time, precision ), https://tc39.es/proposal-temporal/#sec-temporal-timerecordtostring
pub fn time_record_to_string(time: &Time, precision: SecondsPrecision) -> String {
    // 1. Let subSecondNanoseconds be time.[[Millisecond]] × 10**6 + time.[[Microsecond]] × 10**3 + time.[[Nanosecond]].
    let sub_second_nanoseconds =
        u64::from(time.millisecond) * 1_000_000 + u64::from(time.microsecond) * 1000 + u64::from(time.nanosecond);

    // 2. Return FormatTimeString(time.[[Hour]], time.[[Minute]], time.[[Second]], subSecondNanoseconds, precision).
    format_time_string(
        time.hour,
        time.minute,
        time.second,
        sub_second_nanoseconds,
        precision,
        None,
    )
}

// 4.5.14 CompareTimeRecord ( time1, time2 ), https://tc39.es/proposal-temporal/#sec-temporal-comparetimerecord
pub fn compare_time_record(time1: &Time, time2: &Time) -> i8 {
    let fields = |time: &Time| {
        [
            u16::from(time.hour),
            u16::from(time.minute),
            u16::from(time.second),
            time.millisecond,
            time.microsecond,
            time.nanosecond,
        ]
    };

    // 1. If time1.[[Hour]] > time2.[[Hour]], return 1.
    // 2. If time1.[[Hour]] < time2.[[Hour]], return -1.
    // 3. If time1.[[Minute]] > time2.[[Minute]], return 1.
    // 4. If time1.[[Minute]] < time2.[[Minute]], return -1.
    // 5. If time1.[[Second]] > time2.[[Second]], return 1.
    // 6. If time1.[[Second]] < time2.[[Second]], return -1.
    // 7. If time1.[[Millisecond]] > time2.[[Millisecond]], return 1.
    // 8. If time1.[[Millisecond]] < time2.[[Millisecond]], return -1.
    // 9. If time1.[[Microsecond]] > time2.[[Microsecond]], return 1.
    // 10. If time1.[[Microsecond]] < time2.[[Microsecond]], return -1.
    // 11. If time1.[[Nanosecond]] > time2.[[Nanosecond]], return 1.
    // 12. If time1.[[Nanosecond]] < time2.[[Nanosecond]], return -1.
    for (field1, field2) in fields(time1).into_iter().zip(fields(time2)) {
        if field1 > field2 {
            return 1;
        }
        if field1 < field2 {
            return -1;
        }
    }

    // 13. Return 0.
    0
}

// 4.5.15 AddTime ( time, timeDuration ), https://tc39.es/proposal-temporal/#sec-temporal-addtime
pub fn add_time(time: &Time, time_duration: &TimeDuration) -> Time {
    let nanoseconds = time_duration + SignedBigInteger::from(time.nanosecond);

    // 1. Return BalanceTime(time.[[Hour]], time.[[Minute]], time.[[Second]], time.[[Millisecond]], time.[[Microsecond]], time.[[Nanosecond]] + timeDuration).
    balance_time_with_big_nanoseconds(
        f64::from(time.hour),
        f64::from(time.minute),
        f64::from(time.second),
        f64::from(time.millisecond),
        f64::from(time.microsecond),
        &nanoseconds,
    )
}

// 4.5.16 RoundTime ( time, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundtime
pub fn round_time(time: &Time, increment: u64, unit: Unit, rounding_mode: RoundingMode) -> Time {
    let hour = f64::from(time.hour);
    let minute = f64::from(time.minute);
    let second = f64::from(time.second);
    let millisecond = f64::from(time.millisecond);
    let microsecond = f64::from(time.microsecond);
    let nanosecond = f64::from(time.nanosecond);

    let quantity = match unit {
        // 1. If unit is either DAY or HOUR, then
        Unit::Day | Unit::Hour => {
            // a. Let quantity be ((((time.[[Hour]] × 60 + time.[[Minute]]) × 60 + time.[[Second]]) × 1000 + time.[[Millisecond]]) × 1000 + time.[[Microsecond]]) × 1000 + time.[[Nanosecond]].
            ((((hour * 60.0 + minute) * 60.0 + second) * 1000.0 + millisecond) * 1000.0 + microsecond) * 1000.0
                + nanosecond
        }

        // 2. Else if unit is MINUTE, then
        Unit::Minute => {
            // a. Let quantity be (((time.[[Minute]] × 60 + time.[[Second]]) × 1000 + time.[[Millisecond]]) × 1000 + time.[[Microsecond]]) × 1000 + time.[[Nanosecond]].
            (((minute * 60.0 + second) * 1000.0 + millisecond) * 1000.0 + microsecond) * 1000.0 + nanosecond
        }

        // 3. Else if unit is SECOND, then
        Unit::Second => {
            // a. Let quantity be ((time.[[Second]] × 1000 + time.[[Millisecond]]) × 1000 + time.[[Microsecond]]) × 1000 + time.[[Nanosecond]].
            ((second * 1000.0 + millisecond) * 1000.0 + microsecond) * 1000.0 + nanosecond
        }

        // 4. Else if unit is MILLISECOND, then
        Unit::Millisecond => {
            // a. Let quantity be (time.[[Millisecond]] × 1000 + time.[[Microsecond]]) × 1000 + time.[[Nanosecond]].
            (millisecond * 1000.0 + microsecond) * 1000.0 + nanosecond
        }

        // 5. Else if unit is MICROSECOND, then
        Unit::Microsecond => {
            // a. Let quantity be time.[[Microsecond]] × 1000 + time.[[Nanosecond]].
            microsecond * 1000.0 + nanosecond
        }

        // 6. Else,
        Unit::Nanosecond => {
            // a. Assert: unit is NANOSECOND.
            // b. Let quantity be time.[[Nanosecond]].
            nanosecond
        }

        _ => unreachable!(),
    };

    // 7. Let unitLength be the value in the "Length in Nanoseconds" column of the row of Table 21 whose "Value" column contains unit.
    let unit_length = big_int_algorithms::to_u64(temporal_unit_length_in_nanoseconds(unit));

    // 8. Let result be RoundNumberToIncrement(quantity, increment × unitLength, roundingMode) / unitLength.
    let result =
        round_number_to_increment(quantity, increment.wrapping_mul(unit_length), rounding_mode) / unit_length as f64;

    match unit {
        // 9. If unit is DAY, return CreateTimeRecord(0, 0, 0, 0, 0, 0, result).
        Unit::Day => create_time_record(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, result),

        // 10. If unit is HOUR, return BalanceTime(result, 0, 0, 0, 0, 0).
        Unit::Hour => balance_time(result, 0.0, 0.0, 0.0, 0.0, 0.0),

        // 11. If unit is MINUTE, return BalanceTime(time.[[Hour]], result, 0, 0, 0, 0).
        Unit::Minute => balance_time(hour, result, 0.0, 0.0, 0.0, 0.0),

        // 12. If unit is SECOND, return BalanceTime(time.[[Hour]], time.[[Minute]], result, 0, 0, 0).
        Unit::Second => balance_time(hour, minute, result, 0.0, 0.0, 0.0),

        // 13. If unit is MILLISECOND, return BalanceTime(time.[[Hour]], time.[[Minute]], time.[[Second]], result, 0, 0).
        Unit::Millisecond => balance_time(hour, minute, second, result, 0.0, 0.0),

        // 14. If unit is MICROSECOND, return BalanceTime(time.[[Hour]], time.[[Minute]], time.[[Second]], time.[[Millisecond]], result, 0).
        Unit::Microsecond => balance_time(hour, minute, second, millisecond, result, 0.0),

        // 15. Assert: unit is NANOSECOND.
        // 16. Return BalanceTime(time.[[Hour]], time.[[Minute]], time.[[Second]], time.[[Millisecond]], time.[[Microsecond]], result).
        Unit::Nanosecond => balance_time(hour, minute, second, millisecond, microsecond, result),

        _ => unreachable!(),
    }
}

// 4.5.17 DifferenceTemporalPlainTime ( operation, temporalTime, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalplaintime
pub fn difference_temporal_plain_time(
    vm: &Vm,
    operation: DurationOperation,
    temporal_time: &PlainTime,
    other_value: Value,
    options: Value,
) -> ThrowCompletionOr<Gc<Duration>> {
    // 1. Set other to ? ToTemporalTime(other).
    let other = to_temporal_time(vm, other_value, Value::UNDEFINED)?;

    // 2. Let resolvedOptions be ? GetOptionsObject(options).
    let resolved_options = get_options_object(vm, options)?;

    // 3. Let settings be ? GetDifferenceSettings(operation, resolvedOptions, TIME, « », NANOSECOND, HOUR).
    let settings = get_difference_settings(
        vm,
        operation,
        &resolved_options,
        UnitGroup::Time,
        &[],
        Unit::Nanosecond,
        Unit::Hour,
    )?;

    // 4. Let timeDuration be DifferenceTime(temporalTime.[[Time]], other.[[Time]]).
    let time_duration = difference_time(&temporal_time.time(), &other.time());

    // 5. Set timeDuration to ! RoundTimeDuration(timeDuration, settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]]).
    let time_duration = round_time_duration(
        vm,
        &time_duration,
        &SignedBigInteger::from(settings.rounding_increment),
        settings.smallest_unit,
        settings.rounding_mode,
    )
    .must();

    // 6. Let duration be CombineDateAndTimeDuration(ZeroDateDuration(), timeDuration).
    let duration = combine_date_and_time_duration(zero_date_duration(vm), time_duration);

    // 7. Let result be ! TemporalDurationFromInternal(duration, settings.[[LargestUnit]]).
    let mut result = temporal_duration_from_internal(vm, &duration, settings.largest_unit).must();

    // 8. If operation is SINCE, set result to CreateNegatedTemporalDuration(result).
    if operation == DurationOperation::Since {
        result = create_negated_temporal_duration(vm, &result);
    }

    // 9. Return result.
    Ok(result)
}

// 4.5.18 AddDurationToTime ( operation, temporalTime, temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtotime
pub fn add_duration_to_time(
    vm: &Vm,
    operation: ArithmeticOperation,
    temporal_time: &PlainTime,
    temporal_duration_like: Value,
) -> ThrowCompletionOr<Gc<PlainTime>> {
    // 1. Let duration be ? ToTemporalDuration(temporalDurationLike).
    let mut duration = to_temporal_duration(vm, temporal_duration_like)?;

    // 2. If operation is SUBTRACT, set duration to CreateNegatedTemporalDuration(duration).
    if operation == ArithmeticOperation::Subtract {
        duration = create_negated_temporal_duration(vm, &duration);
    }

    // 3. Let internalDuration be ToInternalDurationRecord(duration).
    let internal_duration = to_internal_duration_record(vm, &duration);

    // 4. Let result be AddTime(temporalTime.[[Time]], internalDuration.[[Time]]).
    let result = add_time(&temporal_time.time(), &internal_duration.time);

    // 5. Return ! CreateTemporalTime(result).
    Ok(create_temporal_time(vm, result, None).must())
}
