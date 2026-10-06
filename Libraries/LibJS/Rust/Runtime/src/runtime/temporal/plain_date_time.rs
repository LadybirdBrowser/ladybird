/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Temporal.PlainDateTime objects and the ISO Date-Time Record operations.

use std::sync::LazyLock;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{RoundingMode, get_options_object, ordinary_create_from_constructor_of};
use crate::runtime::big_fraction::BigFraction;
use crate::runtime::big_int::SignedBigInteger;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::date::{
    date_from_time, get_utc_epoch_nanoseconds, hour_from_time, min_from_time, month_from_time, ms_from_time,
    sec_from_time, year_from_time,
};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::temporal::abstract_operations::{
    ArithmeticOperation, DurationOperation, Overflow, SecondsPrecision, ShowCalendar, Unit, UnitGroup, ascii_view,
    format_time_string, get_difference_settings, get_temporal_overflow_option, iso_date_to_epoch_days,
    larger_of_two_temporal_units, parse_iso_date_time,
};
use crate::runtime::temporal::calendar::{
    CalendarField, CalendarFieldListOrPartial, CalendarFields, ISO8601_CALENDAR, calendar_date_add,
    calendar_date_from_fields, calendar_date_until, calendar_equals, canonicalize_calendar, format_calendar_annotation,
    get_temporal_calendar_identifier_with_iso_default, prepare_calendar_fields,
};
use crate::runtime::temporal::duration::{
    Duration, DurationFields, InternalDuration, add_24_hour_days_to_time_duration, adjust_date_duration_record,
    combine_date_and_time_duration, create_negated_temporal_duration, create_temporal_duration,
    round_relative_duration, temporal_duration_from_internal, time_duration_sign,
    to_internal_duration_record_with_24_hour_days, to_temporal_duration, total_relative_duration, zero_date_duration,
};
use crate::runtime::temporal::iso_records::{ISODate, ISODateTime, Time, TimeDuration, TimeOrStartOfDay};
use crate::runtime::temporal::iso8601::Production;
use crate::runtime::temporal::plain_date::{
    PlainDate, add_days_to_iso_date, compare_iso_date, create_iso_date_record, pad_iso_year,
};
use crate::runtime::temporal::plain_time::{
    add_time, balance_time, compare_time_record, create_time_record, difference_time, midnight_time_record,
    regulate_time, round_time,
};
use crate::runtime::temporal::time_zone::get_iso_date_time_for;
use crate::runtime::temporal::zoned_date_time::ZonedDateTime;
use crate::utf16::Utf16View;

#[repr(C)]
#[derive(Trace)]
pub struct PlainDateTime {
    base: Object,
    #[gc(untraced)]
    iso_date_time: ISODateTime, // [[ISODateTime]]
    calendar: Utf16String, // [[Calendar]]
}

define_cell!(PlainDateTime, Object, extends: [Object]);

impl core::ops::Deref for PlainDateTime {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl PlainDateTime {
    fn new(vm: &Vm, iso_date_time: ISODateTime, calendar: Utf16String, prototype: Gc<Object>) -> PlainDateTime {
        PlainDateTime {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            iso_date_time,
            calendar,
        }
    }

    pub fn iso_date_time(&self) -> ISODateTime {
        self.iso_date_time
    }

    pub fn calendar(&self) -> Utf16String {
        self.calendar.clone()
    }
}

// 5.5.2 TimeValueToISODateTimeRecord ( t ), https://tc39.es/proposal-temporal/#sec-temporal-timevaluetoisodatetimerecord
pub fn time_value_to_iso_date_time_record(time_value: f64) -> ISODateTime {
    // 1. Let isoDate be CreateISODateRecord(ℝ(YearFromTime(t)), ℝ(MonthFromTime(t)) + 1, ℝ(DateFromTime(t))).
    let iso_date = create_iso_date_record(
        f64::from(year_from_time(time_value)),
        f64::from(month_from_time(time_value)) + 1.0,
        f64::from(date_from_time(time_value)),
    );

    // 2. Let time be CreateTimeRecord(ℝ(HourFromTime(t)), ℝ(MinFromTime(t)), ℝ(SecFromTime(t)), ℝ(msFromTime(t)), 0, 0).
    let time = create_time_record(
        f64::from(hour_from_time(time_value)),
        f64::from(min_from_time(time_value)),
        f64::from(sec_from_time(time_value)),
        f64::from(ms_from_time(time_value)),
        0.0,
        0.0,
        0.0,
    );

    // 3. Return ISO Date-Time Record { [[ISODate]]: isoDate, [[Time]]: time }.
    ISODateTime { iso_date, time }
}

// 5.5.3 CombineISODateAndTimeRecord ( isoDate, time ), https://tc39.es/proposal-temporal/#sec-temporal-combineisodateandtimerecord
pub fn combine_iso_date_and_time_record(iso_date: ISODate, time: Time) -> ISODateTime {
    // 1. NOTE: time.[[Days]] is ignored.
    // 2. Return ISO Date-Time Record { [[ISODate]]: isoDate, [[Time]]: time }.
    ISODateTime { iso_date, time }
}

// nsMinInstant - nsPerDay
static DATETIME_NANOSECONDS_MIN: LazyLock<SignedBigInteger> =
    LazyLock::new(|| "-8640000086400000000000".parse().expect("a valid integer"));

// nsMaxInstant + nsPerDay
static DATETIME_NANOSECONDS_MAX: LazyLock<SignedBigInteger> =
    LazyLock::new(|| "8640000086400000000000".parse().expect("a valid integer"));

// 5.5.4 ISODateTimeWithinLimits ( isoDateTime ), https://tc39.es/proposal-temporal/#sec-temporal-isodatetimewithinlimits
pub fn iso_date_time_within_limits(iso_date_time: &ISODateTime) -> bool {
    // 1. If abs(ISODateToEpochDays(isoDateTime.[[ISODate]].[[Year]], isoDateTime.[[ISODate]].[[Month]] - 1, isoDateTime.[[ISODate]].[[Day]])) > 10**8 + 1, return false.
    if iso_date_to_epoch_days(
        f64::from(iso_date_time.iso_date.year),
        f64::from(iso_date_time.iso_date.month) - 1.0,
        f64::from(iso_date_time.iso_date.day),
    )
    .abs()
        > 100_000_001.0
    {
        return false;
    }

    // 2. Let ns be ℝ(GetUTCEpochNanoseconds(isoDateTime)).
    let nanoseconds = get_utc_epoch_nanoseconds(iso_date_time);

    // 3. If ns ≤ nsMinInstant - nsPerDay, return false.
    if nanoseconds <= *DATETIME_NANOSECONDS_MIN {
        return false;
    }

    // 4. If ns ≥ nsMaxInstant + nsPerDay, return false.
    if nanoseconds >= *DATETIME_NANOSECONDS_MAX {
        return false;
    }

    // 5. Return true.
    true
}

// 5.5.5 InterpretTemporalDateTimeFields ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-interprettemporaldatetimefields
pub fn interpret_temporal_date_time_fields(
    vm: &Vm,
    calendar: Utf16View<'_>,
    fields: &mut CalendarFields,
    overflow: Overflow,
) -> ThrowCompletionOr<ISODateTime> {
    // 1. Let isoDate be ? CalendarDateFromFields(calendar, fields, overflow).
    let iso_date = calendar_date_from_fields(vm, calendar, fields, overflow)?;

    // 2. Let time be ? RegulateTime(fields.[[Hour]], fields.[[Minute]], fields.[[Second]], fields.[[Millisecond]], fields.[[Microsecond]], fields.[[Nanosecond]], overflow).
    let field = |value: Option<u16>| f64::from(value.expect("the time fields have defaults"));
    let time = regulate_time(
        vm,
        field(fields.hour.map(u16::from)),
        field(fields.minute.map(u16::from)),
        field(fields.second.map(u16::from)),
        field(fields.millisecond),
        field(fields.microsecond),
        field(fields.nanosecond),
        overflow,
    )?;

    // 3. Return CombineISODateAndTimeRecord(isoDate, time).
    Ok(combine_iso_date_and_time_record(iso_date, time))
}

// 5.5.6 ToTemporalDateTime ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaldatetime
pub fn to_temporal_date_time(vm: &Vm, item: Value, options: Value) -> ThrowCompletionOr<Gc<PlainDateTime>> {
    // 1. If options is not present, set options to undefined.

    // 2. If item is an Object, then
    if item.is_object() {
        let object = item.as_object();

        // a. If item has an [[InitializedTemporalDateTime]] internal slot, then
        if let Some(plain_date_time) = object.downcast::<PlainDateTime>() {
            // i. Let resolvedOptions be ? GetOptionsObject(options).
            let resolved_options = get_options_object(vm, options)?;

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            get_temporal_overflow_option(vm, &resolved_options)?;

            // iii. Return ! CreateTemporalDateTime(item.[[ISODateTime]], item.[[Calendar]]).
            return Ok(create_temporal_date_time(
                vm,
                &plain_date_time.iso_date_time(),
                plain_date_time.calendar(),
                None,
            )
            .must());
        }

        // b. If item has an [[InitializedTemporalZonedDateTime]] internal slot, then
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

            // iv. Return ! CreateTemporalDateTime(isoDateTime, item.[[Calendar]]).
            return Ok(create_temporal_date_time(vm, &iso_date_time, zoned_date_time.calendar(), None).must());
        }

        // c. If item has an [[InitializedTemporalDate]] internal slot, then
        if let Some(plain_date) = object.downcast::<PlainDate>() {
            // i. Let resolvedOptions be ? GetOptionsObject(options).
            let resolved_options = get_options_object(vm, options)?;

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            get_temporal_overflow_option(vm, &resolved_options)?;

            // iii. Let isoDateTime be CombineISODateAndTimeRecord(item.[[ISODate]], MidnightTimeRecord()).
            let iso_date_time = combine_iso_date_and_time_record(plain_date.iso_date(), midnight_time_record());

            // iv. Return ? CreateTemporalDateTime(isoDateTime, item.[[Calendar]]).
            return create_temporal_date_time(vm, &iso_date_time, plain_date.calendar(), None);
        }

        // d. Let calendar be ? GetTemporalCalendarIdentifierWithISODefault(item).
        let calendar = get_temporal_calendar_identifier_with_iso_default(vm, &object)?;

        // e. Let fields be ? PrepareCalendarFields(calendar, item, « YEAR, MONTH, MONTH-CODE, DAY », « HOUR, MINUTE, SECOND, MILLISECOND, MICROSECOND, NANOSECOND », «»).
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
        let mut fields = prepare_calendar_fields(
            vm,
            Utf16View::of_string(&calendar),
            &object,
            &CALENDAR_FIELD_NAMES,
            &NON_CALENDAR_FIELD_NAMES,
            CalendarFieldListOrPartial::List(&[]),
        )?;

        // f. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, options)?;

        // g. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

        // h. Let result be ? InterpretTemporalDateTimeFields(calendar, fields, overflow).
        let result = interpret_temporal_date_time_fields(vm, Utf16View::of_string(&calendar), &mut fields, overflow)?;

        // i. Return ? CreateTemporalDateTime(result, calendar).
        return create_temporal_date_time(vm, &result, calendar, None);
    }

    // 3. If item is not a String, throw a TypeError exception.
    if !item.is_string() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::TemporalInvalidPlainDateTime, &[]);
    }

    // 4. Let result be ? ParseISODateTime(item, « TemporalDateTimeString[~Zoned] »).
    let item_string = item.as_string().utf16_string();
    let result = parse_iso_date_time(
        vm,
        Utf16View::of_string(&item_string),
        &[Production::TemporalDateTimeString],
    )?;

    // 5. If result.[[Time]] is START-OF-DAY, let time be MidnightTimeRecord(); else let time be result.[[Time]].
    let time = match result.time {
        TimeOrStartOfDay::StartOfDay => midnight_time_record(),
        TimeOrStartOfDay::Time(time) => time,
    };

    // 6. Let calendar be result.[[Calendar]].
    // 7. If calendar is empty, set calendar to "iso8601".
    let calendar = match &result.calendar {
        Some(calendar) => canonicalize_calendar(vm, Utf16View::of_string(calendar))?,
        None => canonicalize_calendar(vm, ascii_view(ISO8601_CALENDAR))?,
    };

    // 9. Let resolvedOptions be ? GetOptionsObject(options).
    let resolved_options = get_options_object(vm, options)?;

    // 10. Perform ? GetTemporalOverflowOption(resolvedOptions).
    get_temporal_overflow_option(vm, &resolved_options)?;

    // 11. Let isoDate be CreateISODateRecord(result.[[Year]], result.[[Month]], result.[[Day]]).
    let iso_date = create_iso_date_record(
        f64::from(result.year.expect("a date-time string has a year")),
        f64::from(result.month),
        f64::from(result.day),
    );

    // 12. Let isoDateTime be CombineISODateAndTimeRecord(isoDate, time).
    let iso_date_time = combine_iso_date_and_time_record(iso_date, time);

    // 13. Return ? CreateTemporalDateTime(isoDateTime, calendar).
    create_temporal_date_time(vm, &iso_date_time, calendar, None)
}

// 5.5.7 BalanceISODateTime ( year, month, day, hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/proposal-temporal/#sec-temporal-balanceisodatetime
#[allow(clippy::too_many_arguments)]
pub fn balance_iso_date_time(
    year: f64,
    month: f64,
    day: f64,
    hour: f64,
    minute: f64,
    second: f64,
    millisecond: f64,
    microsecond: f64,
    nanosecond: f64,
) -> ISODateTime {
    // 1. Let balancedTime be BalanceTime(hour, minute, second, millisecond, microsecond, nanosecond).
    let balanced_time = balance_time(hour, minute, second, millisecond, microsecond, nanosecond);

    // 2. Let balancedDate be AddDaysToISODate(CreateISODateRecord(year, month, day), balancedTime.[[Days]]).
    let balanced_date = add_days_to_iso_date(create_iso_date_record(year, month, day), balanced_time.days);

    // 3. Return CombineISODateAndTimeRecord(balancedDate, balancedTime).
    combine_iso_date_and_time_record(balanced_date, balanced_time)
}

// 5.5.8 CreateTemporalDateTime ( isoDateTime, calendar [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporaldatetime
pub fn create_temporal_date_time(
    vm: &Vm,
    iso_date_time: &ISODateTime,
    calendar: Utf16String,
    new_target: Option<Gc<FunctionObject>>,
) -> ThrowCompletionOr<Gc<PlainDateTime>> {
    let realm = vm.current_realm().expect("CreateTemporalDateTime runs in a realm");

    // 1. If ISODateTimeWithinLimits(isoDateTime) is false, throw a RangeError exception.
    if !iso_date_time_within_limits(iso_date_time) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainDateTime, &[]);
    }

    // 2. If newTarget is not present, set newTarget to %Temporal.PlainDateTime%.
    let new_target = new_target.unwrap_or_else(|| realm.intrinsics().temporal_plain_date_time_constructor(vm));

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainDateTime.prototype%", « [[InitializedTemporalDateTime]], [[ISODateTime]], [[Calendar]] »).
    // 4. Set object.[[ISODateTime]] to isoDateTime.
    // 5. Set object.[[Calendar]] to calendar.
    let iso_date_time = *iso_date_time;
    let object = ordinary_create_from_constructor_of(
        vm,
        realm,
        new_target,
        Intrinsics::temporal_plain_date_time_prototype,
        |prototype| PlainDateTime::new(vm, iso_date_time, calendar, prototype),
    )?;

    // 6. Return object.
    Ok(object)
}

// 5.5.9 ISODateTimeToString ( isoDateTime, calendar, precision, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-isodatetimetostring
pub fn iso_date_time_to_string(
    iso_date_time: &ISODateTime,
    calendar: Utf16View<'_>,
    precision: SecondsPrecision,
    show_calendar: ShowCalendar,
) -> String {
    // 1. Let yearString be PadISOYear(isoDateTime.[[ISODate]].[[Year]]).
    let year_string = pad_iso_year(iso_date_time.iso_date.year);

    // 2. Let monthString be ToZeroPaddedDecimalString(isoDateTime.[[ISODate]].[[Month]], 2).
    let month = iso_date_time.iso_date.month;

    // 3. Let dayString be ToZeroPaddedDecimalString(isoDateTime.[[ISODate]].[[Day]], 2).
    let day = iso_date_time.iso_date.day;

    // 4. Let subSecondNanoseconds be isoDateTime.[[Time]].[[Millisecond]] × 10**6 + isoDateTime.[[Time]].[[Microsecond]] × 10**3 + isoDateTime.[[Time]].[[Nanosecond]].
    let sub_second_nanoseconds = u64::from(iso_date_time.time.millisecond) * 1_000_000
        + u64::from(iso_date_time.time.microsecond) * 1000
        + u64::from(iso_date_time.time.nanosecond);

    // 5. Let timeString be FormatTimeString(isoDateTime.[[Time]].[[Hour]], isoDateTime.[[Time]].[[Minute]], isoDateTime.[[Time]].[[Second]], subSecondNanoseconds, precision).
    let time_string = format_time_string(
        iso_date_time.time.hour,
        iso_date_time.time.minute,
        iso_date_time.time.second,
        sub_second_nanoseconds,
        precision,
        None,
    );

    // 6. Let calendarString be FormatCalendarAnnotation(calendar, showCalendar).
    let calendar_string = format_calendar_annotation(calendar, show_calendar);

    // 7. Return the string-concatenation of yearString, the code unit 0x002D (HYPHEN-MINUS), monthString, the code unit 0x002D (HYPHEN-MINUS),
    //    dayString, 0x0054 (LATIN CAPITAL LETTER T), timeString, and calendarString.
    format!("{year_string}-{month:02}-{day:02}T{time_string}{calendar_string}")
}

// 5.5.10 CompareISODateTime ( isoDateTime1, isoDateTime2 ), https://tc39.es/proposal-temporal/#sec-temporal-compareisodatetime
pub fn compare_iso_date_time(iso_date_time1: &ISODateTime, iso_date_time2: &ISODateTime) -> i8 {
    // 1. Let dateResult be CompareISODate(isoDateTime1.[[ISODate]], isoDateTime2.[[ISODate]]).
    let date_result = compare_iso_date(iso_date_time1.iso_date, iso_date_time2.iso_date);

    // 2. If dateResult ≠ 0, return dateResult.
    if date_result != 0 {
        return date_result;
    }

    // 3. Return CompareTimeRecord(isoDateTime1.[[Time]], isoDateTime2.[[Time]]).
    compare_time_record(&iso_date_time1.time, &iso_date_time2.time)
}

// 5.5.11 RoundISODateTime ( isoDateTime, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundisodatetime
pub fn round_iso_date_time(
    iso_date_time: &ISODateTime,
    increment: u64,
    unit: Unit,
    rounding_mode: RoundingMode,
) -> ISODateTime {
    // 1. Assert: ISODateTimeWithinLimits(isoDateTime) is true.
    assert!(iso_date_time_within_limits(iso_date_time));

    // 2. Let roundedTime be RoundTime(isoDateTime.[[Time]], increment, unit, roundingMode).
    let rounded_time = round_time(&iso_date_time.time, increment, unit, rounding_mode);

    // 3. Let balanceResult be AddDaysToISODate(isoDateTime.[[ISODate]], roundedTime.[[Days]]).
    let balance_result = add_days_to_iso_date(iso_date_time.iso_date, rounded_time.days);

    // 4. Return CombineISODateAndTimeRecord(balanceResult, roundedTime).
    combine_iso_date_and_time_record(balance_result, rounded_time)
}

// 5.5.12 DifferenceISODateTime ( isoDateTime1, isoDateTime2, calendar, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-differenceisodatetime
pub fn difference_iso_date_time(
    vm: &Vm,
    iso_date_time1: &ISODateTime,
    iso_date_time2: &ISODateTime,
    calendar: Utf16View<'_>,
    largest_unit: Unit,
) -> InternalDuration {
    // 1. Assert: ISODateTimeWithinLimits(isoDateTime1) is true.
    assert!(iso_date_time_within_limits(iso_date_time1));

    // 2. Assert: ISODateTimeWithinLimits(isoDateTime2) is true.
    assert!(iso_date_time_within_limits(iso_date_time2));

    // 3. Let timeDuration be DifferenceTime(isoDateTime1.[[Time]], isoDateTime2.[[Time]]).
    let mut time_duration = difference_time(&iso_date_time1.time, &iso_date_time2.time);

    // 4. Let timeSign be TimeDurationSign(timeDuration).
    let time_sign = time_duration_sign(&time_duration);

    // 5. Let dateSign be CompareISODate(isoDateTime1.[[ISODate]], isoDateTime2.[[ISODate]]).
    let date_sign = compare_iso_date(iso_date_time1.iso_date, iso_date_time2.iso_date);

    // 6. Let adjustedDate be isoDateTime2.[[ISODate]].
    let mut adjusted_date = iso_date_time2.iso_date;

    // 7. If timeSign = dateSign, then
    if time_sign == date_sign {
        // a. Set adjustedDate to AddDaysToISODate(adjustedDate, timeSign).
        adjusted_date = add_days_to_iso_date(adjusted_date, f64::from(time_sign));

        // b. Set timeDuration to ! Add24HourDaysToTimeDuration(timeDuration, -timeSign).
        time_duration = add_24_hour_days_to_time_duration(vm, &time_duration, -f64::from(time_sign)).must();
    }

    // 8. Let dateLargestUnit be LargerOfTwoTemporalUnits(DAY, largestUnit).
    let date_largest_unit = larger_of_two_temporal_units(Unit::Day, largest_unit);

    // 9. Let dateDifference be CalendarDateUntil(calendar, isoDateTime1.[[ISODate]], adjustedDate, dateLargestUnit).
    let mut date_difference =
        calendar_date_until(vm, calendar, iso_date_time1.iso_date, adjusted_date, date_largest_unit);

    // 10. If largestUnit is not dateLargestUnit, then
    if largest_unit != date_largest_unit {
        // a. Set timeDuration to ! Add24HourDaysToTimeDuration(timeDuration, dateDifference.[[Days]]).
        time_duration = add_24_hour_days_to_time_duration(vm, &time_duration, date_difference.days).must();

        // b. Set dateDifference.[[Days]] to 0.
        date_difference.days = 0.0;
    }

    // 11. Return CombineDateAndTimeDuration(dateDifference, timeDuration).
    combine_date_and_time_duration(date_difference, time_duration)
}

// 5.5.13 DifferencePlainDateTimeWithRounding ( isoDateTime1, isoDateTime2, calendar, largestUnit, roundingIncrement, smallestUnit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-differenceplaindatetimewithrounding
#[allow(clippy::too_many_arguments)]
pub fn difference_plain_date_time_with_rounding(
    vm: &Vm,
    iso_date_time1: &ISODateTime,
    iso_date_time2: &ISODateTime,
    calendar: Utf16View<'_>,
    largest_unit: Unit,
    rounding_increment: u64,
    smallest_unit: Unit,
    rounding_mode: RoundingMode,
) -> ThrowCompletionOr<InternalDuration> {
    // 1. If CompareISODateTime(isoDateTime1, isoDateTime2) = 0, return CombineDateAndTimeDuration(ZeroDateDuration(), 0).
    if compare_iso_date_time(iso_date_time1, iso_date_time2) == 0 {
        return Ok(combine_date_and_time_duration(
            zero_date_duration(vm),
            TimeDuration::default(),
        ));
    }

    // 2. If ISODateTimeWithinLimits(isoDateTime1) is false or ISODateTimeWithinLimits(isoDateTime2) is false, throw a
    //    RangeError exception.
    if !iso_date_time_within_limits(iso_date_time1) || !iso_date_time_within_limits(iso_date_time2) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidISODateTime, &[]);
    }

    // 3. Let diff be DifferenceISODateTime(isoDateTime1, isoDateTime2, calendar, largestUnit).
    let diff = difference_iso_date_time(vm, iso_date_time1, iso_date_time2, calendar, largest_unit);

    // 4. If smallestUnit is NANOSECOND and roundingIncrement = 1, return diff.
    if smallest_unit == Unit::Nanosecond && rounding_increment == 1 {
        return Ok(diff);
    }

    // 5. Let originEpochNs be GetUTCEpochNanoseconds(isoDateTime1).
    let origin_epoch_ns = get_utc_epoch_nanoseconds(iso_date_time1);

    // 6. Let destEpochNs be GetUTCEpochNanoseconds(isoDateTime2).
    let dest_epoch_ns = get_utc_epoch_nanoseconds(iso_date_time2);

    // 7. Return ? RoundRelativeDuration(diff, originEpochNs, destEpochNs, isoDateTime1, UNSET, calendar, largestUnit, roundingIncrement, smallestUnit, roundingMode).
    round_relative_duration(
        vm,
        diff,
        &origin_epoch_ns,
        &dest_epoch_ns,
        iso_date_time1,
        None,
        calendar,
        largest_unit,
        rounding_increment,
        smallest_unit,
        rounding_mode,
    )
}

// 5.5.14 DifferencePlainDateTimeWithTotal ( isoDateTime1, isoDateTime2, calendar, unit ), https://tc39.es/proposal-temporal/#sec-temporal-differenceplaindatetimewithtotal
pub fn difference_plain_date_time_with_total(
    vm: &Vm,
    iso_date_time1: &ISODateTime,
    iso_date_time2: &ISODateTime,
    calendar: Utf16View<'_>,
    unit: Unit,
) -> ThrowCompletionOr<BigFraction> {
    // 1. If CompareISODateTime(isoDateTime1, isoDateTime2) = 0, return 0.
    if compare_iso_date_time(iso_date_time1, iso_date_time2) == 0 {
        return Ok(BigFraction::default());
    }

    // 2. If ISODateTimeWithinLimits(isoDateTime1) is false or ISODateTimeWithinLimits(isoDateTime2) is false, throw a
    //    RangeError exception.
    if !iso_date_time_within_limits(iso_date_time1) || !iso_date_time_within_limits(iso_date_time2) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidISODateTime, &[]);
    }

    // 3. Let diff be DifferenceISODateTime(isoDateTime1, isoDateTime2, calendar, unit).
    let diff = difference_iso_date_time(vm, iso_date_time1, iso_date_time2, calendar, unit);

    // 4. If unit is NANOSECOND, return diff.[[Time]].
    if unit == Unit::Nanosecond {
        return Ok(BigFraction::from_integer(diff.time));
    }

    // 5. Let originEpochNs be GetUTCEpochNanoseconds(isoDateTime1).
    let origin_epoch_ns = get_utc_epoch_nanoseconds(iso_date_time1);

    // 6. Let destEpochNs be GetUTCEpochNanoseconds(isoDateTime2).
    let dest_epoch_ns = get_utc_epoch_nanoseconds(iso_date_time2);

    // 7. Return ? TotalRelativeDuration(diff, originEpochNs, destEpochNs, isoDateTime1, UNSET, calendar, unit).
    total_relative_duration(
        vm,
        &diff,
        &origin_epoch_ns,
        &dest_epoch_ns,
        iso_date_time1,
        None,
        calendar,
        unit,
    )
}

// 5.5.15 DifferenceTemporalPlainDateTime ( operation, dateTime, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalplaindatetime
pub fn difference_temporal_plain_date_time(
    vm: &Vm,
    operation: DurationOperation,
    date_time: &PlainDateTime,
    other_value: Value,
    options: Value,
) -> ThrowCompletionOr<Gc<Duration>> {
    let calendar = date_time.calendar();

    // 1. Set other to ? ToTemporalDateTime(other).
    let other = to_temporal_date_time(vm, other_value, Value::UNDEFINED)?;

    // 2. If CalendarEquals(dateTime.[[Calendar]], other.[[Calendar]]) is false, throw a RangeError exception.
    if !calendar_equals(Utf16View::of_string(&calendar), Utf16View::of_string(&other.calendar())) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalDifferentCalendars, &[]);
    }

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    let resolved_options = get_options_object(vm, options)?;

    // 4. Let settings be ? GetDifferenceSettings(operation, resolvedOptions, DATETIME, « », NANOSECOND, DAY).
    let settings = get_difference_settings(
        vm,
        operation,
        &resolved_options,
        UnitGroup::DateTime,
        &[],
        Unit::Nanosecond,
        Unit::Day,
    )?;

    // 5. If CompareISODateTime(dateTime.[[ISODateTime]], other.[[ISODateTime]]) = 0, return ! CreateTemporalDuration(0, 0, 0, 0, 0, 0, 0, 0, 0, 0).
    if compare_iso_date_time(&date_time.iso_date_time(), &other.iso_date_time()) == 0 {
        return Ok(create_temporal_duration(vm, DurationFields::default(), None).must());
    }

    // 6. Let internalDuration be ? DifferencePlainDateTimeWithRounding(dateTime.[[ISODateTime]], other.[[ISODateTime]], dateTime.[[Calendar]], settings.[[LargestUnit]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]]).
    let internal_duration = difference_plain_date_time_with_rounding(
        vm,
        &date_time.iso_date_time(),
        &other.iso_date_time(),
        Utf16View::of_string(&calendar),
        settings.largest_unit,
        settings.rounding_increment,
        settings.smallest_unit,
        settings.rounding_mode,
    )?;

    // 7. Let result be ! TemporalDurationFromInternal(internalDuration, settings.[[LargestUnit]]).
    let mut result = temporal_duration_from_internal(vm, &internal_duration, settings.largest_unit).must();

    // 8. If operation is SINCE, set result to CreateNegatedTemporalDuration(result).
    if operation == DurationOperation::Since {
        result = create_negated_temporal_duration(vm, &result);
    }

    // 9. Return result.
    Ok(result)
}

// 5.5.16 AddDurationToDateTime ( operation, dateTime, temporalDurationLike, options ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtodatetime
pub fn add_duration_to_date_time(
    vm: &Vm,
    operation: ArithmeticOperation,
    date_time: &PlainDateTime,
    temporal_duration_like: Value,
    options: Value,
) -> ThrowCompletionOr<Gc<PlainDateTime>> {
    // 1. Let duration be ? ToTemporalDuration(temporalDurationLike).
    let mut duration = to_temporal_duration(vm, temporal_duration_like)?;

    // 2. If operation is SUBTRACT, set duration to CreateNegatedTemporalDuration(duration).
    if operation == ArithmeticOperation::Subtract {
        duration = create_negated_temporal_duration(vm, &duration);
    }

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    let resolved_options = get_options_object(vm, options)?;

    // 4. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
    let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

    // 5. Let internalDuration be ToInternalDurationRecordWith24HourDays(duration).
    let internal_duration = to_internal_duration_record_with_24_hour_days(vm, &duration);

    // 6. Let timeResult be AddTime(dateTime.[[ISODateTime]].[[Time]], internalDuration.[[Time]]).
    let time_result = add_time(&date_time.iso_date_time().time, &internal_duration.time);

    // 7. Let dateDuration be ? AdjustDateDurationRecord(internalDuration.[[Date]], timeResult.[[Days]]).
    let date_duration = adjust_date_duration_record(vm, &internal_duration.date, time_result.days, None, None)?;

    // 8. Let addedDate be ? CalendarDateAdd(dateTime.[[Calendar]], dateTime.[[ISODateTime]].[[ISODate]], dateDuration, overflow).
    let calendar = date_time.calendar();
    let added_date = calendar_date_add(
        vm,
        Utf16View::of_string(&calendar),
        date_time.iso_date_time().iso_date,
        &date_duration,
        overflow,
    )?;

    // 9. Let result be CombineISODateAndTimeRecord(addedDate, timeResult).
    let result = combine_iso_date_and_time_record(added_date, time_result);

    // 10. Return ? CreateTemporalDateTime(result, dateTime.[[Calendar]]).
    create_temporal_date_time(vm, &result, calendar, None)
}
