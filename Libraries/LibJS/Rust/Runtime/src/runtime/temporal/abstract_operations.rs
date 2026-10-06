/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The options, units, rounding and parsing operations every Temporal type shares.

use core::fmt;

use ak::Utf16String;
use num_bigint::Sign as BigIntSign;
use num_integer::Integer;
use num_traits::{One, Signed, Zero};

use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    OptionDefault, OptionType, RoundingMode, big_modulo, get_option, get_rounding_increment_option,
    get_rounding_mode_option, modulo,
};
use crate::runtime::big_fraction::BigFraction;
use crate::runtime::big_int::{BigInt, SignedBigInteger};
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::date::{parse_date_time_utc_offset, parse_date_time_utc_offset_or_throw};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::temporal::calendar::{
    CalendarField, CalendarFieldListOrPartial, CalendarFields, ISO8601_CALENDAR, calendar_iso_to_date,
    canonicalize_calendar, get_temporal_calendar_identifier_with_iso_default, prepare_calendar_fields,
};
use crate::runtime::temporal::duration::{Duration, DurationFields, create_temporal_duration};
use crate::runtime::temporal::instant::{
    NANOSECONDS_PER_DAY, NANOSECONDS_PER_HOUR, NANOSECONDS_PER_MICROSECOND, NANOSECONDS_PER_MILLISECOND,
    NANOSECONDS_PER_MINUTE, NANOSECONDS_PER_NANOSECOND, NANOSECONDS_PER_SECOND,
};
use crate::runtime::temporal::iso_records::{
    ISODate, ParsedISODateTime, ParsedISOTimeZone, ParsedTimeZoneIdentifier, Time, TimeOrStartOfDay,
};
use crate::runtime::temporal::iso8601::{Production, SubMinutePrecision, parse_iso8601, parse_utc_offset};
use crate::runtime::temporal::plain_date::{
    PlainDate, create_iso_date_record, create_temporal_date, is_valid_iso_date,
};
use crate::runtime::temporal::plain_date_time::{PlainDateTime, interpret_temporal_date_time_fields};
use crate::runtime::temporal::plain_month_day::PlainMonthDay;
use crate::runtime::temporal::plain_time::{PlainTime, create_time_record};
use crate::runtime::temporal::plain_year_month::PlainYearMonth;
use crate::runtime::temporal::time_zone::{
    UTC_TIME_ZONE, parse_time_zone_identifier_from_parse_result, parse_time_zone_identifier_or_throw,
    to_temporal_time_zone_identifier_from_string,
};
use crate::runtime::temporal::zoned_date_time::{
    MatchBehavior, OffsetBehavior, ZonedDateTime, create_temporal_zoned_date_time, interpret_iso_date_time_offset,
};
use crate::runtime::value_conversions::string_to_number;
use crate::utf16::{TrimMode, Utf16Display, Utf16View};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ArithmeticOperation {
    Add,
    Subtract,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DateType {
    Date,
    MonthDay,
    YearMonth,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Direction {
    Next,
    Previous,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Disambiguation {
    Compatible,
    Earlier,
    Later,
    Reject,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DurationOperation {
    Since,
    Until,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OffsetOption {
    Prefer,
    Use,
    Ignore,
    Reject,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Overflow {
    Constrain,
    Reject,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShowCalendar {
    Auto,
    Always,
    Never,
    Critical,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShowOffset {
    Auto,
    Never,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShowTimeZoneName {
    Auto,
    Never,
    Critical,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TimeStyle {
    Separated,
    Unseparated,
}

// https://tc39.es/proposal-temporal/#sec-temporal-units
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Unit {
    Year,
    Month,
    Week,
    Day,
    Hour,
    Minute,
    Second,
    Millisecond,
    Microsecond,
    Nanosecond,
}

impl Unit {
    /// The unit whose ordinal index in Table 21 is `index`.
    pub fn from_index(index: usize) -> Unit {
        TEMPORAL_UNITS[index].value
    }
}

// https://tc39.es/proposal-temporal/#sec-temporal-units
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnitCategory {
    Date,
    Time,
}

// https://tc39.es/proposal-temporal/#sec-temporal-units
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnitGroup {
    Date,
    Time,
    DateTime,
}

// https://tc39.es/proposal-temporal/#table-unsigned-rounding-modes
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnsignedRoundingMode {
    HalfEven,
    HalfInfinity,
    HalfZero,
    Infinity,
    Zero,
}

// https://tc39.es/proposal-temporal/#table-unsigned-rounding-modes
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Sign {
    Negative,
    Positive,
}

/// Precision: AUTO, or a number of fractional second digits.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Precision {
    Auto,
    Digits(u8),
}

/// RoundingIncrement: UNSET, or an increment.
pub type RoundingIncrement = Option<u64>;

/// UnitDefault: REQUIRED, UNSET, AUTO, or a unit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnitDefault {
    Required,
    Unset,
    Auto,
    Unit(Unit),
}

/// UnitValue: UNSET, AUTO, or a unit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnitValue {
    Unset,
    Auto,
    Unit(Unit),
}

/// SecondsStringPrecision::Precision: MINUTE, AUTO, or a number of fractional second digits.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SecondsPrecision {
    Minute,
    Auto,
    Digits(u8),
}

impl SecondsPrecision {
    /// The fractional second digits of a precision that is not MINUTE.
    pub fn to_precision(self) -> Precision {
        match self {
            SecondsPrecision::Auto => Precision::Auto,
            SecondsPrecision::Digits(digits) => Precision::Digits(digits),
            SecondsPrecision::Minute => panic!("a MINUTE precision has no fractional second digits"),
        }
    }
}

impl From<Precision> for SecondsPrecision {
    fn from(precision: Precision) -> Self {
        match precision {
            Precision::Auto => SecondsPrecision::Auto,
            Precision::Digits(digits) => SecondsPrecision::Digits(digits),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SecondsStringPrecision {
    pub precision: SecondsPrecision,
    pub unit: Unit,
    pub increment: u8,
}

/// The Record GetTemporalRelativeToOption returns. It holds cells, so it only lives in locals.
#[derive(Clone, Copy, Default)]
pub struct RelativeTo {
    pub plain_relative_to: Option<Gc<PlainDate>>,     // [[PlainRelativeTo]]
    pub zoned_relative_to: Option<Gc<ZonedDateTime>>, // [[ZonedRelativeTo]]
}

#[derive(Clone, Copy, Debug)]
pub struct DifferenceSettings {
    pub smallest_unit: Unit,
    pub largest_unit: Unit,
    pub rounding_mode: RoundingMode,
    pub rounding_increment: u64,
}

// https://tc39.es/proposal-temporal/#table-temporal-units
struct TemporalUnit {
    value: Unit,
    singular_property_name: &'static str,
    plural_property_name: &'static str,
    category: UnitCategory,
    maximum_duration_rounding_increment: RoundingIncrement,
}

const TEMPORAL_UNITS: [TemporalUnit; 10] = [
    TemporalUnit {
        value: Unit::Year,
        singular_property_name: "year",
        plural_property_name: "years",
        category: UnitCategory::Date,
        maximum_duration_rounding_increment: None,
    },
    TemporalUnit {
        value: Unit::Month,
        singular_property_name: "month",
        plural_property_name: "months",
        category: UnitCategory::Date,
        maximum_duration_rounding_increment: None,
    },
    TemporalUnit {
        value: Unit::Week,
        singular_property_name: "week",
        plural_property_name: "weeks",
        category: UnitCategory::Date,
        maximum_duration_rounding_increment: None,
    },
    TemporalUnit {
        value: Unit::Day,
        singular_property_name: "day",
        plural_property_name: "days",
        category: UnitCategory::Date,
        maximum_duration_rounding_increment: None,
    },
    TemporalUnit {
        value: Unit::Hour,
        singular_property_name: "hour",
        plural_property_name: "hours",
        category: UnitCategory::Time,
        maximum_duration_rounding_increment: Some(24),
    },
    TemporalUnit {
        value: Unit::Minute,
        singular_property_name: "minute",
        plural_property_name: "minutes",
        category: UnitCategory::Time,
        maximum_duration_rounding_increment: Some(60),
    },
    TemporalUnit {
        value: Unit::Second,
        singular_property_name: "second",
        plural_property_name: "seconds",
        category: UnitCategory::Time,
        maximum_duration_rounding_increment: Some(60),
    },
    TemporalUnit {
        value: Unit::Millisecond,
        singular_property_name: "millisecond",
        plural_property_name: "milliseconds",
        category: UnitCategory::Time,
        maximum_duration_rounding_increment: Some(1000),
    },
    TemporalUnit {
        value: Unit::Microsecond,
        singular_property_name: "microsecond",
        plural_property_name: "microseconds",
        category: UnitCategory::Time,
        maximum_duration_rounding_increment: Some(1000),
    },
    TemporalUnit {
        value: Unit::Nanosecond,
        singular_property_name: "nanosecond",
        plural_property_name: "nanoseconds",
        category: UnitCategory::Time,
        maximum_duration_rounding_increment: Some(1000),
    },
];

pub fn temporal_unit_to_string(unit: Unit) -> &'static str {
    match unit {
        Unit::Year => "year",
        Unit::Month => "month",
        Unit::Week => "week",
        Unit::Day => "day",
        Unit::Hour => "hour",
        Unit::Minute => "minute",
        Unit::Second => "second",
        Unit::Millisecond => "millisecond",
        Unit::Microsecond => "microsecond",
        Unit::Nanosecond => "nanosecond",
    }
}

/// A view of an ASCII string, for passing string constants as Utf16View literals.
pub fn ascii_view(string: &str) -> Utf16View<'_> {
    Utf16View::Ascii(string.as_bytes())
}

// 13.1 ISODateToEpochDays ( year, month, date ), https://tc39.es/proposal-temporal/#sec-isodatetoepochdays
pub fn iso_date_to_epoch_days(year: f64, month: f64, date: f64) -> f64 {
    // 1. Let resolvedYear be year + floor(month / 12).
    // 2. Let resolvedMonth be month modulo 12.
    // 3. Find a time t such that EpochTimeToEpochYear(t) = resolvedYear, EpochTimeToMonthInYear(t) = resolvedMonth, and EpochTimeToDate(t) = 1.
    // 4. Return EpochTimeToDayNumber(t) + date - 1.

    // NB: Since we don't have a real MV type to work with, let's defer to MakeDay.
    crate::runtime::date::make_day(year, month, date)
}

// 13.2 EpochDaysToEpochMs ( day, time ), https://tc39.es/proposal-temporal/#sec-epochdaystoepochms
pub fn epoch_days_to_epoch_ms(day: f64, time: f64) -> f64 {
    // 1. Return day × ℝ(msPerDay) + time.
    day * crate::runtime::date::MS_PER_DAY + time
}

// 13.4 CheckISODaysRange ( isoDate ), https://tc39.es/proposal-temporal/#sec-checkisodaysrange
pub fn check_iso_days_range(vm: &Vm, iso_date: ISODate) -> ThrowCompletionOr<()> {
    // 1. If abs(ISODateToEpochDays(isoDate.[[Year]], isoDate.[[Month]] - 1, isoDate.[[Day]])) > 10**8, throw a RangeError exception.
    if iso_date_to_epoch_days(
        f64::from(iso_date.year),
        f64::from(iso_date.month) - 1.0,
        f64::from(iso_date.day),
    )
    .abs()
        > 100_000_000.0
    {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidISODate, &[]);
    }

    // 2. Return unused.
    Ok(())
}

fn string_option(
    vm: &Vm,
    options: &Object,
    property: &PropertyKey,
    values: &[&str],
    default: OptionDefault<'_>,
) -> ThrowCompletionOr<Value> {
    get_option(vm, options, property, OptionType::String, values, default)
}

fn string_value_is(value: Value, string: &str) -> bool {
    value.as_string().utf16_string_view() == string
}

// 13.6 GetTemporalOverflowOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporaloverflowoption
pub fn get_temporal_overflow_option(vm: &Vm, options: &Object) -> ThrowCompletionOr<Overflow> {
    // 1. Let stringValue be ? GetOption(options, "overflow", STRING, « "constrain", "reject" », "constrain").
    let string_value = string_option(
        vm,
        options,
        &vm.names.overflow,
        &["constrain", "reject"],
        OptionDefault::String(ascii_view("constrain")),
    )?;

    // 2. If stringValue is "constrain", return CONSTRAIN.
    if string_value_is(string_value, "constrain") {
        return Ok(Overflow::Constrain);
    }

    // 3. Return REJECT.
    Ok(Overflow::Reject)
}

// 13.7 GetTemporalDisambiguationOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporaldisambiguationoption
pub fn get_temporal_disambiguation_option(vm: &Vm, options: &Object) -> ThrowCompletionOr<Disambiguation> {
    // 1. Let stringValue be ? GetOption(options, "disambiguation", STRING, « "compatible", "earlier", "later", "reject" », "compatible").
    let string_value = string_option(
        vm,
        options,
        &vm.names.disambiguation,
        &["compatible", "earlier", "later", "reject"],
        OptionDefault::String(ascii_view("compatible")),
    )?;

    // 2. If stringValue is "compatible", return COMPATIBLE.
    if string_value_is(string_value, "compatible") {
        return Ok(Disambiguation::Compatible);
    }

    // 3. If stringValue is "earlier", return EARLIER.
    if string_value_is(string_value, "earlier") {
        return Ok(Disambiguation::Earlier);
    }

    // 4. If stringValue is "later", return LATER.
    if string_value_is(string_value, "later") {
        return Ok(Disambiguation::Later);
    }

    // 5. Return REJECT.
    Ok(Disambiguation::Reject)
}

// 13.8 NegateRoundingMode ( roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-negateroundingmode
pub fn negate_rounding_mode(rounding_mode: RoundingMode) -> RoundingMode {
    match rounding_mode {
        // 1. If roundingMode is CEIL, return FLOOR.
        RoundingMode::Ceil => RoundingMode::Floor,
        // 2. If roundingMode is FLOOR, return CEIL.
        RoundingMode::Floor => RoundingMode::Ceil,
        // 3. If roundingMode is HALF-CEIL, return HALF-FLOOR.
        RoundingMode::HalfCeil => RoundingMode::HalfFloor,
        // 4. If roundingMode is HALF-FLOOR, return HALF-CEIL.
        RoundingMode::HalfFloor => RoundingMode::HalfCeil,
        // 5. Return roundingMode.
        _ => rounding_mode,
    }
}

// 13.9 GetTemporalOffsetOption ( options, fallback ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporaloffsetoption
pub fn get_temporal_offset_option(
    vm: &Vm,
    options: &Object,
    fallback: OffsetOption,
) -> ThrowCompletionOr<OffsetOption> {
    let string_fallback = match fallback {
        // 1. If fallback is PREFER, let stringFallback be "prefer".
        OffsetOption::Prefer => "prefer",
        // 2. Else if fallback is USE, let stringFallback be "use".
        OffsetOption::Use => "use",
        // 3. Else if fallback is IGNORE, let stringFallback be "ignore".
        OffsetOption::Ignore => "ignore",
        // 4. Else, let stringFallback be "reject".
        OffsetOption::Reject => "reject",
    };

    // 5. Let stringValue be ? GetOption(options, "offset", STRING, « "prefer", "use", "ignore", "reject" », stringFallback).
    let string_value = string_option(
        vm,
        options,
        &vm.names.offset,
        &["prefer", "use", "ignore", "reject"],
        OptionDefault::String(ascii_view(string_fallback)),
    )?;

    // 6. If stringValue is "prefer", return PREFER.
    if string_value_is(string_value, "prefer") {
        return Ok(OffsetOption::Prefer);
    }

    // 7. If stringValue is "use", return USE.
    if string_value_is(string_value, "use") {
        return Ok(OffsetOption::Use);
    }

    // 8. If stringValue is "ignore", return IGNORE.
    if string_value_is(string_value, "ignore") {
        return Ok(OffsetOption::Ignore);
    }

    // 9. Return REJECT.
    Ok(OffsetOption::Reject)
}

// 13.10 GetTemporalShowCalendarNameOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalshowcalendarnameoption
pub fn get_temporal_show_calendar_name_option(vm: &Vm, options: &Object) -> ThrowCompletionOr<ShowCalendar> {
    // 1. Let stringValue be ? GetOption(options, "calendarName", STRING, « "auto", "always", "never", "critical" », "auto").
    let string_value = string_option(
        vm,
        options,
        &vm.names.calendarName,
        &["auto", "always", "never", "critical"],
        OptionDefault::String(ascii_view("auto")),
    )?;

    // 2. If stringValue is "always", return ALWAYS.
    if string_value_is(string_value, "always") {
        return Ok(ShowCalendar::Always);
    }

    // 3. If stringValue is "never", return NEVER.
    if string_value_is(string_value, "never") {
        return Ok(ShowCalendar::Never);
    }

    // 4. If stringValue is "critical", return CRITICAL.
    if string_value_is(string_value, "critical") {
        return Ok(ShowCalendar::Critical);
    }

    // 5. Return AUTO.
    Ok(ShowCalendar::Auto)
}

// 13.11 GetTemporalShowTimeZoneNameOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalshowtimezonenameoption
pub fn get_temporal_show_time_zone_name_option(vm: &Vm, options: &Object) -> ThrowCompletionOr<ShowTimeZoneName> {
    // 1. Let stringValue be ? GetOption(options, "timeZoneName", STRING, « "auto", "never", "critical" », "auto").
    let string_value = string_option(
        vm,
        options,
        &vm.names.timeZoneName,
        &["auto", "never", "critical"],
        OptionDefault::String(ascii_view("auto")),
    )?;

    // 2. If stringValue is "never", return NEVER.
    if string_value_is(string_value, "never") {
        return Ok(ShowTimeZoneName::Never);
    }

    // 3. If stringValue is "critical", return CRITICAL.
    if string_value_is(string_value, "critical") {
        return Ok(ShowTimeZoneName::Critical);
    }

    // 4. Return AUTO.
    Ok(ShowTimeZoneName::Auto)
}

// 13.12 GetTemporalShowOffsetOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalshowoffsetoption
pub fn get_temporal_show_offset_option(vm: &Vm, options: &Object) -> ThrowCompletionOr<ShowOffset> {
    // 1. Let stringValue be ? GetOption(options, "offset", STRING, « "auto", "never" », "auto").
    let string_value = string_option(
        vm,
        options,
        &vm.names.offset,
        &["auto", "never"],
        OptionDefault::String(ascii_view("auto")),
    )?;

    // 2. If stringValue is "never", return never.
    if string_value_is(string_value, "never") {
        return Ok(ShowOffset::Never);
    }

    // 3. Return auto.
    Ok(ShowOffset::Auto)
}

// 13.13 GetDirectionOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-getdirectionoption
pub fn get_direction_option(vm: &Vm, options: &Object) -> ThrowCompletionOr<Direction> {
    // 1. Let stringValue be ? GetOption(options, "direction", STRING, « "next", "previous" », REQUIRED).
    let string_value = string_option(
        vm,
        options,
        &vm.names.direction,
        &["next", "previous"],
        OptionDefault::Required,
    )?;

    // 2. If stringValue is "next", return NEXT.
    if string_value_is(string_value, "next") {
        return Ok(Direction::Next);
    }

    // 3. Return PREVIOUS.
    Ok(Direction::Previous)
}

// 13.14 ValidateTemporalRoundingIncrement ( increment, dividend, inclusive ), https://tc39.es/proposal-temporal/#sec-validatetemporalroundingincrement
pub fn validate_temporal_rounding_increment(
    vm: &Vm,
    increment: u64,
    dividend: u64,
    inclusive: bool,
) -> ThrowCompletionOr<()> {
    // 1. If inclusive is true, then
    let maximum = if inclusive {
        // a. Let maximum be dividend.
        dividend
    }
    // 2. Else,
    else {
        // a. Assert: dividend > 1.
        assert!(dividend > 1);

        // b. Let maximum be dividend - 1.
        dividend - 1
    };

    // 3. If increment > maximum, throw a RangeError exception.
    if increment > maximum {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::OptionIsNotValidValue,
            &[&increment, &"roundingIncrement"],
        );
    }

    // 5. If dividend modulo increment ≠ 0, throw a RangeError exception.
    if !dividend.is_multiple_of(increment) {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::OptionIsNotValidValue,
            &[&increment, &"roundingIncrement"],
        );
    }

    // 6. Return UNUSED.
    Ok(())
}

// 13.15 GetTemporalFractionalSecondDigitsOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalfractionalseconddigitsoption
pub fn get_temporal_fractional_second_digits_option(vm: &Vm, options: &Object) -> ThrowCompletionOr<Precision> {
    // 1. Let digitsValue be ? Get(options, "fractionalSecondDigits").
    let digits_value = options.get(vm, &vm.names.fractionalSecondDigits)?;

    // 2. If digitsValue is undefined, return AUTO.
    if digits_value.is_undefined() {
        return Ok(Precision::Auto);
    }

    // 3. If digitsValue is not a Number, then
    if !digits_value.is_number() {
        // a. If ? ToString(digitsValue) is not "auto", throw a RangeError exception.
        let digits_value_string = digits_value.to_utf16_string(vm)?;

        if Utf16View::of_string(&digits_value_string) != "auto" {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::OptionIsNotValidValue,
                &[&digits_value, &vm.names.fractionalSecondDigits],
            );
        }

        // b. Return AUTO.
        return Ok(Precision::Auto);
    }

    // 4. If digitsValue is one of NaN, +∞𝔽, or -∞𝔽, throw a RangeError exception.
    if digits_value.is_nan() || digits_value.is_infinity() {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::OptionIsNotValidValue,
            &[&digits_value, &vm.names.fractionalSecondDigits],
        );
    }

    // 5. Let digitCount be floor(ℝ(digitsValue)).
    let digit_count = digits_value.as_f64().floor();

    // 6. If digitCount < 0 or digitCount > 9, throw a RangeError exception.
    if !(0.0..=9.0).contains(&digit_count) {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::OptionIsNotValidValue,
            &[&digits_value, &vm.names.fractionalSecondDigits],
        );
    }

    // 7. Return digitCount.
    Ok(Precision::Digits(digit_count as u8))
}

// 13.16 ToSecondsStringPrecisionRecord ( smallestUnit, fractionalDigitCount ), https://tc39.es/proposal-temporal/#sec-temporal-tosecondsstringprecisionrecord
pub fn to_seconds_string_precision_record(
    smallest_unit: UnitValue,
    fractional_digit_count: Precision,
) -> SecondsStringPrecision {
    if let UnitValue::Unit(unit) = smallest_unit {
        // 1. If smallestUnit is MINUTE, return the Record { [[Precision]]: MINUTE, [[Unit]]: MINUTE, [[Increment]]: 1  }.
        if unit == Unit::Minute {
            return SecondsStringPrecision {
                precision: SecondsPrecision::Minute,
                unit: Unit::Minute,
                increment: 1,
            };
        }

        // 2. If smallestUnit is SECOND, return the Record { [[Precision]]: 0, [[Unit]]: SECOND, [[Increment]]: 1  }.
        if unit == Unit::Second {
            return SecondsStringPrecision {
                precision: SecondsPrecision::Digits(0),
                unit: Unit::Second,
                increment: 1,
            };
        }

        // 3. If smallestUnit is MILLISECOND, return the Record { [[Precision]]: 3, [[Unit]]: MILLISECOND, [[Increment]]: 1  }.
        if unit == Unit::Millisecond {
            return SecondsStringPrecision {
                precision: SecondsPrecision::Digits(3),
                unit: Unit::Millisecond,
                increment: 1,
            };
        }

        // 4. If smallestUnit is MICROSECOND, return the Record { [[Precision]]: 6, [[Unit]]: MICROSECOND, [[Increment]]: 1  }.
        if unit == Unit::Microsecond {
            return SecondsStringPrecision {
                precision: SecondsPrecision::Digits(6),
                unit: Unit::Microsecond,
                increment: 1,
            };
        }

        // 5. If smallestUnit is NANOSECOND, return the Record { [[Precision]]: 9, [[Unit]]: NANOSECOND, [[Increment]]: 1  }.
        if unit == Unit::Nanosecond {
            return SecondsStringPrecision {
                precision: SecondsPrecision::Digits(9),
                unit: Unit::Nanosecond,
                increment: 1,
            };
        }
    }

    // 6. Assert: smallestUnit is UNSET.
    assert!(smallest_unit == UnitValue::Unset);

    // 7. If fractionalDigitCount is auto, return the Record { [[Precision]]: AUTO, [[Unit]]: NANOSECOND, [[Increment]]: 1  }.
    let Precision::Digits(fractional_digits) = fractional_digit_count else {
        return SecondsStringPrecision {
            precision: SecondsPrecision::Auto,
            unit: Unit::Nanosecond,
            increment: 1,
        };
    };

    // 8. If fractionalDigitCount = 0, return the Record { [[Precision]]: 0, [[Unit]]: SECOND, [[Increment]]: 1  }.
    if fractional_digits == 0 {
        return SecondsStringPrecision {
            precision: SecondsPrecision::Digits(0),
            unit: Unit::Second,
            increment: 1,
        };
    }

    // 9. If fractionalDigitCount is in the inclusive interval from 1 to 3, return the Record { [[Precision]]: fractionalDigitCount, [[Unit]]: MILLISECOND, [[Increment]]: 10**(3 - fractionalDigitCount)  }.
    if (1..=3).contains(&fractional_digits) {
        return SecondsStringPrecision {
            precision: SecondsPrecision::Digits(fractional_digits),
            unit: Unit::Millisecond,
            increment: 10u8.pow(u32::from(3 - fractional_digits)),
        };
    }

    // 10. If fractionalDigitCount is in the inclusive interval from 4 to 6, return the Record { [[Precision]]: fractionalDigitCount, [[Unit]]: MICROSECOND, [[Increment]]: 10**(6 - fractionalDigitCount)  }.
    if (4..=6).contains(&fractional_digits) {
        return SecondsStringPrecision {
            precision: SecondsPrecision::Digits(fractional_digits),
            unit: Unit::Microsecond,
            increment: 10u8.pow(u32::from(6 - fractional_digits)),
        };
    }

    // 11. Assert: fractionalDigitCount is in the inclusive interval from 7 to 9.
    assert!((7..=9).contains(&fractional_digits));

    // 12. Return the Record { [[Precision]]: fractionalDigitCount, [[Unit]]: NANOSECOND, [[Increment]]: 10**(9 - fractionalDigitCount)  }.
    SecondsStringPrecision {
        precision: SecondsPrecision::Digits(fractional_digits),
        unit: Unit::Nanosecond,
        increment: 10u8.pow(u32::from(9 - fractional_digits)),
    }
}

// 13.17 GetTemporalUnitValuedOption ( options, key, default ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalunitvaluedoption
pub fn get_temporal_unit_valued_option(
    vm: &Vm,
    options: &Object,
    key: &PropertyKey,
    default: UnitDefault,
) -> ThrowCompletionOr<UnitValue> {
    // 1. Let allowedStrings be a List containing all values in the "Singular property name" and "Plural property name"
    //    columns of Table 21, except the header row.
    // 2. Append "auto" to allowedStrings.
    // 3. NOTE: For each singular Temporal unit name that is contained within allowedStrings, the corresponding plural
    //    name is also contained within it.
    let mut allowed_strings = Vec::with_capacity(TEMPORAL_UNITS.len() * 2 + 1);
    for temporal_unit in &TEMPORAL_UNITS {
        allowed_strings.push(temporal_unit.singular_property_name);
        allowed_strings.push(temporal_unit.plural_property_name);
    }
    allowed_strings.push("auto");

    // 4. If default is UNSET, then
    //     a. Let defaultValue be undefined.
    // 5. Else,
    //     a. Let defaultValue be default.
    let default_value = match default {
        UnitDefault::Unset => OptionDefault::Empty,
        UnitDefault::Required => OptionDefault::Required,
        UnitDefault::Auto => OptionDefault::String(ascii_view("auto")),
        UnitDefault::Unit(unit) => OptionDefault::String(ascii_view(temporal_unit_to_string(unit))),
    };

    // 6. Let value be ? GetOption(options, key, STRING, allowedStrings, defaultValue).
    let value = get_option(vm, options, key, OptionType::String, &allowed_strings, default_value)?;

    // 7. If value is undefined, return UNSET.
    if value.is_undefined() {
        return Ok(UnitValue::Unset);
    }

    let value_string = value.as_string();
    let value_string = value_string.utf16_string_view();

    // 8. If value is "auto", return AUTO.
    if value_string == "auto" {
        return Ok(UnitValue::Auto);
    }

    // 9. Return the value in the "Value" column of Table 21 corresponding to the row with value in its "Singular
    //    property name" or "Plural property name" column.
    let temporal_unit = TEMPORAL_UNITS
        .iter()
        .find(|temporal_unit| {
            value_string == temporal_unit.singular_property_name || value_string == temporal_unit.plural_property_name
        })
        .expect("GetOption only returns allowed strings");

    Ok(UnitValue::Unit(temporal_unit.value))
}

// 13.18 ValidateTemporalUnitValue ( value, unitGroup [ , extraValues ] ), https://tc39.es/proposal-temporal/#sec-temporal-validatetemporalunitvaluedoption
// AD-HOC: We require a PropertyKey parameter as well to form sensible exception messages.
pub fn validate_temporal_unit_value(
    vm: &Vm,
    key: &PropertyKey,
    value: UnitValue,
    unit_group: UnitGroup,
    extra_values: &[UnitValue],
) -> ThrowCompletionOr<()> {
    // 1. If value is UNSET, return UNUSED.
    if value == UnitValue::Unset {
        return Ok(());
    }

    // 2. If extraValues is present and extraValues contains value, return UNUSED.
    if extra_values.contains(&value) {
        return Ok(());
    }

    // 3. Let category be the value in the “Category” column of the row of Table 21 whose “Value” column contains value.
    //    If there is no such row, throw a RangeError exception.
    let UnitValue::Unit(unit_value) = value else {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::OptionIsNotValidValue, &[&"auto", key]);
    };

    let category = temporal_unit_category(unit_value);

    // 4. If category is DATE and unitGroup is either DATE or DATETIME, return unused.
    if category == UnitCategory::Date && (unit_group == UnitGroup::Date || unit_group == UnitGroup::DateTime) {
        return Ok(());
    }

    // 5. If category is TIME and unitGroup is either TIME or DATETIME, return unused.
    if category == UnitCategory::Time && (unit_group == UnitGroup::Time || unit_group == UnitGroup::DateTime) {
        return Ok(());
    }

    // 6. Throw a RangeError exception.
    vm.throw_completion(
        ErrorKind::RangeError,
        ErrorType::OptionIsNotValidValue,
        &[&temporal_unit_to_string(unit_value), key],
    )
}

// 13.19 GetTemporalRelativeToOption ( options ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalrelativetooption
pub fn get_temporal_relative_to_option(vm: &Vm, options: &Object) -> ThrowCompletionOr<RelativeTo> {
    // 1. Let value be ? Get(options, "relativeTo").
    let value = options.get(vm, &vm.names.relativeTo)?;

    // 2. If value is undefined, return the Record { [[PlainRelativeTo]]: undefined, [[ZonedRelativeTo]]: undefined }.
    if value.is_undefined() {
        return Ok(RelativeTo::default());
    }

    // 3. Let offsetBehaviour be OPTION.
    let mut offset_behavior = OffsetBehavior::Option;

    // 4. Let matchBehaviour be MATCH-EXACTLY.
    let mut match_behavior = MatchBehavior::MatchExactly;

    let calendar: Utf16String;
    let time_zone: Option<Utf16String>;
    let offset_string: Option<Utf16String>;

    let iso_date: ISODate;
    let time: TimeOrStartOfDay;

    // 5. If value is an Object, then
    if value.is_object() {
        let object = value.as_object();

        // a. If value has an [[InitializedTemporalZonedDateTime]] internal slot, then
        if let Some(zoned_date_time) = object.downcast::<ZonedDateTime>() {
            // i. Return the Record { [[PlainRelativeTo]]: undefined, [[ZonedRelativeTo]]: value }.
            return Ok(RelativeTo {
                plain_relative_to: None,
                zoned_relative_to: Some(zoned_date_time),
            });
        }

        // b. If value has an [[InitializedTemporalDate]] internal slot, then
        if let Some(plain_date) = object.downcast::<PlainDate>() {
            // i. Return the Record { [[PlainRelativeTo]]: value, [[ZonedRelativeTo]]: undefined }.
            return Ok(RelativeTo {
                plain_relative_to: Some(plain_date),
                zoned_relative_to: None,
            });
        }

        // c. If value has an [[InitializedTemporalDateTime]] internal slot, then
        if let Some(plain_date_time) = object.downcast::<PlainDateTime>() {
            // i. Let plainDate be ! CreateTemporalDate(value.[[ISODateTime]].[[ISODate]], value.[[Calendar]]).
            let plain_date = create_temporal_date(
                vm,
                plain_date_time.iso_date_time().iso_date,
                plain_date_time.calendar(),
                None,
            )
            .must();

            // ii. Return the Record { [[PlainRelativeTo]]: plainDate, [[ZonedRelativeTo]]: undefined }.
            return Ok(RelativeTo {
                plain_relative_to: Some(plain_date),
                zoned_relative_to: None,
            });
        }

        // d. Let calendar be ? GetTemporalCalendarIdentifierWithISODefault(value).
        calendar = get_temporal_calendar_identifier_with_iso_default(vm, &object)?;

        // e. Let fields be ? PrepareCalendarFields(calendar, value, « YEAR, MONTH, MONTH-CODE, DAY », « HOUR, MINUTE, SECOND, MILLISECOND, MICROSECOND, NANOSECOND, OFFSET, TIME-ZONE », «»).
        let mut fields = prepare_calendar_fields(
            vm,
            Utf16View::of_string(&calendar),
            &object,
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
                CalendarField::TimeZone,
            ],
            CalendarFieldListOrPartial::List(&[]),
        )?;

        // f. Let result be ? InterpretTemporalDateTimeFields(calendar, fields, CONSTRAIN).
        let result =
            interpret_temporal_date_time_fields(vm, Utf16View::of_string(&calendar), &mut fields, Overflow::Constrain)?;

        // g. Let timeZone be fields.[[TimeZone]].
        time_zone = fields.time_zone.take();

        // h. Let offsetString be fields.[[OffsetString]].
        offset_string = fields.offset_string.take();

        // i. If offsetString is UNSET, then
        if offset_string.is_none() {
            // i. Set offsetBehaviour to WALL.
            offset_behavior = OffsetBehavior::Wall;
        }

        // j. Let isoDate be result.[[ISODate]].
        iso_date = result.iso_date;

        // k. Let time be result.[[Time]].
        time = TimeOrStartOfDay::Time(result.time);
    }
    // 6. Else,
    else {
        // a. If value is not a String, throw a TypeError exception.
        if !value.is_string() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAString, &[&vm.names.relativeTo]);
        }

        // b. Let result be ? ParseISODateTime(value, « TemporalDateTimeString[+Zoned], TemporalDateTimeString[~Zoned] »).
        let value_string = value.as_string().utf16_string();
        let result = parse_iso_date_time(
            vm,
            Utf16View::of_string(&value_string),
            &[
                Production::TemporalZonedDateTimeString,
                Production::TemporalDateTimeString,
            ],
        )?;

        // c. Let offsetString be result.[[TimeZone]].[[OffsetString]].
        offset_string = result.time_zone.offset_string.clone();

        // d. Let annotation be result.[[TimeZone]].[[TimeZoneAnnotation]].
        let annotation = result.time_zone.time_zone_annotation.clone();

        // e. If annotation is EMPTY, then
        match annotation {
            None => {
                // i. Let timeZone be UNSET.
                time_zone = None;
            }
            // f. Else,
            Some(annotation) => {
                // i. Let timeZone be ? ToTemporalTimeZoneIdentifier(annotation).
                time_zone = Some(to_temporal_time_zone_identifier_from_string(
                    vm,
                    Utf16View::of_string(&annotation),
                )?);

                // ii. If result.[[TimeZone]].[[Z]] is true, then
                if result.time_zone.z_designator {
                    // 1. Set offsetBehaviour to EXACT.
                    offset_behavior = OffsetBehavior::Exact;
                }
                // iii. Else if offsetString is EMPTY, then
                else if offset_string.is_none() {
                    // 1. Set offsetBehaviour to WALL.
                    offset_behavior = OffsetBehavior::Wall;
                }

                // iv. Set matchBehaviour to MATCH-MINUTES.
                match_behavior = MatchBehavior::MatchMinutes;

                // v. If offsetString is not EMPTY, then
                if let Some(offset_string) = &offset_string {
                    // 1. Let offsetParseResult be ParseText(StringToCodePoints(offsetString), UTCOffset[+SubMinutePrecision]).
                    let offset_parse_result =
                        parse_utc_offset(Utf16View::of_string(offset_string), SubMinutePrecision::Yes);

                    // 2. Assert: offsetParseResult is a Parse Node.
                    let offset_parse_result = offset_parse_result.expect("the offset string was parsed before");

                    // 3. If offsetParseResult contains more than one MinuteSecond Parse Node, set matchBehaviour to MATCH-EXACTLY.
                    if offset_parse_result.seconds.is_some() {
                        match_behavior = MatchBehavior::MatchExactly;
                    }
                }
            }
        }

        // g. Let calendar be result.[[Calendar]].
        // h. If calendar is EMPTY, set calendar to "iso8601".
        calendar = match &result.calendar {
            Some(calendar) => canonicalize_calendar(vm, Utf16View::of_string(calendar))?,
            None => canonicalize_calendar(vm, ascii_view(ISO8601_CALENDAR))?,
        };

        // j. Let isoDate be CreateISODateRecord(result.[[Year]], result.[[Month]], result.[[Day]]).
        iso_date = create_iso_date_record(
            f64::from(result.year.expect("a date-time string has a year")),
            f64::from(result.month),
            f64::from(result.day),
        );

        // k. Let time be result.[[Time]].
        time = result.time;
    }

    // 7. If timeZone is UNSET, then
    let Some(time_zone) = time_zone else {
        // a. Let plainDate be ? CreateTemporalDate(isoDate, calendar).
        let plain_date = create_temporal_date(vm, iso_date, calendar, None)?;

        // b. Return the Record { [[PlainRelativeTo]]: plainDate, [[ZonedRelativeTo]]: undefined }.
        return Ok(RelativeTo {
            plain_relative_to: Some(plain_date),
            zoned_relative_to: None,
        });
    };

    // 8. If offsetBehaviour is OPTION, then
    let offset_nanoseconds = if offset_behavior == OffsetBehavior::Option {
        // a. Let offsetNs be ! ParseDateTimeUTCOffset(offsetString).
        parse_date_time_utc_offset(Utf16View::of_string(
            offset_string
                .as_ref()
                .expect("an OPTION offset behaviour has an offset string"),
        ))
    }
    // 9. Else,
    else {
        // a. Let offsetNs be 0.
        0.0
    };

    // 10. Let epochNanoseconds be ? InterpretISODateTimeOffset(isoDate, time, offsetBehaviour, offsetNs, timeZone, COMPATIBLE, REJECT, matchBehaviour).
    let epoch_nanoseconds = interpret_iso_date_time_offset(
        vm,
        iso_date,
        time,
        offset_behavior,
        offset_nanoseconds,
        Utf16View::of_string(&time_zone),
        Disambiguation::Compatible,
        OffsetOption::Reject,
        match_behavior,
    )?;

    // 11. Let zonedRelativeTo be ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
    let zoned_relative_to =
        create_temporal_zoned_date_time(vm, BigInt::create(vm, epoch_nanoseconds), time_zone, calendar, None).must();

    // 12. Return the Record { [[PlainRelativeTo]]: undefined, [[ZonedRelativeTo]]: zonedRelativeTo }.
    Ok(RelativeTo {
        plain_relative_to: None,
        zoned_relative_to: Some(zoned_relative_to),
    })
}

// 13.20 LargerOfTwoTemporalUnits ( u1, u2 ), https://tc39.es/proposal-temporal/#sec-temporal-largeroftwotemporalunits
pub fn larger_of_two_temporal_units(unit1: Unit, unit2: Unit) -> Unit {
    // 1. For each row of Table 21, except the header row, in table order, do
    for row in &TEMPORAL_UNITS {
        // a. Let unit be the value in the "Value" column of the row.
        let unit = row.value;

        // b. If u1 is unit, return unit.
        if unit1 == unit {
            return unit;
        }

        // c. If u2 is unit, return unit.
        if unit2 == unit {
            return unit;
        }
    }

    unreachable!()
}

// 13.21 IsCalendarUnit ( unit ), https://tc39.es/proposal-temporal/#sec-temporal-iscalendarunit
pub fn is_calendar_unit(unit: Unit) -> bool {
    // 1. If unit is year, return true.
    // 2. If unit is month, return true.
    // 3. If unit is week, return true.
    // 4. Return false.
    matches!(unit, Unit::Year | Unit::Month | Unit::Week)
}

// 13.22 TemporalUnitCategory ( unit ), https://tc39.es/proposal-temporal/#sec-temporal-temporalunitcategory
pub fn temporal_unit_category(unit: Unit) -> UnitCategory {
    // 1. Return the value from the "Category" column of the row of Table 21 in which unit is in the "Value" column.
    TEMPORAL_UNITS[unit as usize].category
}

// 13.23 MaximumTemporalDurationRoundingIncrement ( unit ), https://tc39.es/proposal-temporal/#sec-temporal-maximumtemporaldurationroundingincrement
pub fn maximum_temporal_duration_rounding_increment(unit: Unit) -> RoundingIncrement {
    // 1. Return the value from the "Maximum duration rounding increment" column of the row of Table 21 in which unit is
    //    in the "Value" column.
    TEMPORAL_UNITS[unit as usize].maximum_duration_rounding_increment
}

// https://tc39.es/proposal-temporal/#table-temporal-units
pub fn temporal_unit_length_in_nanoseconds(unit: Unit) -> &'static SignedBigInteger {
    match unit {
        Unit::Day => &NANOSECONDS_PER_DAY,
        Unit::Hour => &NANOSECONDS_PER_HOUR,
        Unit::Minute => &NANOSECONDS_PER_MINUTE,
        Unit::Second => &NANOSECONDS_PER_SECOND,
        Unit::Millisecond => &NANOSECONDS_PER_MILLISECOND,
        Unit::Microsecond => &NANOSECONDS_PER_MICROSECOND,
        Unit::Nanosecond => &NANOSECONDS_PER_NANOSECOND,
        _ => unreachable!("calendar units have no fixed length"),
    }
}

// 13.24 IsPartialTemporalObject ( value ), https://tc39.es/proposal-temporal/#sec-temporal-ispartialtemporalobject
pub fn is_partial_temporal_object(vm: &Vm, value: Value) -> ThrowCompletionOr<bool> {
    // 1. If value is not an Object, return false.
    if !value.is_object() {
        return Ok(false);
    }

    let object = value.as_object();

    // 2. If value has an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], [[InitializedTemporalMonthDay]],
    //    [[InitializedTemporalTime]], [[InitializedTemporalYearMonth]], or [[InitializedTemporalZonedDateTime]] internal
    //    slot, return false.
    if object.is::<PlainDate>()
        || object.is::<PlainDateTime>()
        || object.is::<PlainMonthDay>()
        || object.is::<PlainTime>()
        || object.is::<PlainYearMonth>()
        || object.is::<ZonedDateTime>()
    {
        return Ok(false);
    }

    // 3. Let calendarProperty be ? Get(value, "calendar").
    let calendar_property = object.get(vm, &vm.names.calendar)?;

    // 4. If calendarProperty is not undefined, return false.
    if !calendar_property.is_undefined() {
        return Ok(false);
    }

    // 5. Let timeZoneProperty be ? Get(value, "timeZone").
    let time_zone_property = object.get(vm, &vm.names.timeZone)?;

    // 6. If timeZoneProperty is not undefined, return false.
    if !time_zone_property.is_undefined() {
        return Ok(false);
    }

    // 7. Return true.
    Ok(true)
}

// 13.25 FormatFractionalSeconds ( subSecondNanoseconds, precision ), https://tc39.es/proposal-temporal/#sec-temporal-formatfractionalseconds
pub fn format_fractional_seconds(sub_second_nanoseconds: u64, precision: Precision) -> String {
    let fraction_string = match precision {
        // 1. If precision is auto, then
        Precision::Auto => {
            // a. If subSecondNanoseconds = 0, return the empty String.
            if sub_second_nanoseconds == 0 {
                return String::new();
            }

            // b. Let fractionString be ToZeroPaddedDecimalString(subSecondNanoseconds, 9).
            let fraction_string = format!("{sub_second_nanoseconds:09}");

            // c. Set fractionString to the longest prefix of fractionString ending with a code unit other than 0x0030 (DIGIT ZERO).
            fraction_string.trim_end_matches('0').to_string()
        }
        // 2. Else,
        Precision::Digits(precision) => {
            // a. If precision = 0, return the empty String.
            if precision == 0 {
                return String::new();
            }

            // b. Let fractionString be ToZeroPaddedDecimalString(subSecondNanoseconds, 9).
            let fraction_string = format!("{sub_second_nanoseconds:09}");

            // c. Set fractionString to the substring of fractionString from 0 to precision.
            fraction_string[..usize::from(precision)].to_string()
        }
    };

    // 3. Return the string-concatenation of the code unit 0x002E (FULL STOP) and fractionString.
    format!(".{fraction_string}")
}

// 13.26 FormatTimeString ( hour, minute, second, subSecondNanoseconds, precision [ , style ] ), https://tc39.es/proposal-temporal/#sec-temporal-formattimestring
pub fn format_time_string(
    hour: u8,
    minute: u8,
    second: u8,
    sub_second_nanoseconds: u64,
    precision: SecondsPrecision,
    style: Option<TimeStyle>,
) -> String {
    // 1. If style is present and style is UNSEPARATED, let separator be the empty String; else, let separator be ":".
    let separator = if style == Some(TimeStyle::Unseparated) { "" } else { ":" };

    // 2. Let hh be ToZeroPaddedDecimalString(hour, 2).
    // 3. Let mm be ToZeroPaddedDecimalString(minute, 2).

    // 4. If precision is minute, return the string-concatenation of hh, separator, and mm.
    if precision == SecondsPrecision::Minute {
        return format!("{hour:02}{separator}{minute:02}");
    }

    // 5. Let ss be ToZeroPaddedDecimalString(second, 2).
    // 6. Let subSecondsPart be FormatFractionalSeconds(subSecondNanoseconds, precision).
    let sub_seconds_part = format_fractional_seconds(sub_second_nanoseconds, precision.to_precision());

    // 7. Return the string-concatenation of hh, separator, mm, separator, ss, and subSecondsPart.
    format!("{hour:02}{separator}{minute:02}{separator}{second:02}{sub_seconds_part}")
}

// 13.27 GetUnsignedRoundingMode ( roundingMode, sign ), https://tc39.es/proposal-temporal/#sec-getunsignedroundingmode
pub fn get_unsigned_rounding_mode(rounding_mode: RoundingMode, sign: Sign) -> UnsignedRoundingMode {
    // 1. Return the specification type in the "Unsigned Rounding Mode" column of Table 22 for the row where the value
    //    in the "Rounding Mode" column is roundingMode and the value in the "Sign" column is sign.
    let is_positive = sign == Sign::Positive;
    match rounding_mode {
        RoundingMode::Ceil if is_positive => UnsignedRoundingMode::Infinity,
        RoundingMode::Ceil => UnsignedRoundingMode::Zero,
        RoundingMode::Floor if is_positive => UnsignedRoundingMode::Zero,
        RoundingMode::Floor => UnsignedRoundingMode::Infinity,
        RoundingMode::Expand => UnsignedRoundingMode::Infinity,
        RoundingMode::Trunc => UnsignedRoundingMode::Zero,
        RoundingMode::HalfCeil if is_positive => UnsignedRoundingMode::HalfInfinity,
        RoundingMode::HalfCeil => UnsignedRoundingMode::HalfZero,
        RoundingMode::HalfFloor if is_positive => UnsignedRoundingMode::HalfZero,
        RoundingMode::HalfFloor => UnsignedRoundingMode::HalfInfinity,
        RoundingMode::HalfExpand => UnsignedRoundingMode::HalfInfinity,
        RoundingMode::HalfTrunc => UnsignedRoundingMode::HalfZero,
        RoundingMode::HalfEven => UnsignedRoundingMode::HalfEven,
    }
}

// 13.28 ApplyUnsignedRoundingMode ( x, r1, r2, unsignedRoundingMode ), https://tc39.es/proposal-temporal/#sec-applyunsignedroundingmode
pub fn apply_unsigned_rounding_mode(x: f64, r1: f64, r2: f64, unsigned_rounding_mode: UnsignedRoundingMode) -> f64 {
    // 1. If x = r1, return r1.
    if x == r1 {
        return r1;
    }

    // 2. Assert: r1 < x < r2.
    assert!(r1 < x && x < r2);

    // 3. Assert: unsignedRoundingMode is not undefined.

    // 4. If unsignedRoundingMode is ZERO, return r1.
    if unsigned_rounding_mode == UnsignedRoundingMode::Zero {
        return r1;
    }

    // 5. If unsignedRoundingMode is INFINITY, return r2.
    if unsigned_rounding_mode == UnsignedRoundingMode::Infinity {
        return r2;
    }

    // 6. Let d1 be x – r1.
    let d1 = x - r1;

    // 7. Let d2 be r2 – x.
    let d2 = r2 - x;

    // 8. If d1 < d2, return r1.
    if d1 < d2 {
        return r1;
    }

    // 9. If d2 < d1, return r2.
    if d2 < d1 {
        return r2;
    }

    // 10. Assert: d1 is equal to d2.
    assert!(d1 == d2);

    // 11. If unsignedRoundingMode is HALF-ZERO, return r1.
    if unsigned_rounding_mode == UnsignedRoundingMode::HalfZero {
        return r1;
    }

    // 12. If unsignedRoundingMode is HALF-INFINITY, return r2.
    if unsigned_rounding_mode == UnsignedRoundingMode::HalfInfinity {
        return r2;
    }

    // 13. Assert: unsignedRoundingMode is HALF-EVEN.
    assert!(unsigned_rounding_mode == UnsignedRoundingMode::HalfEven);

    // 14. Let cardinality be (r1 / (r2 – r1)) modulo 2.
    let cardinality = modulo(r1 / (r2 - r1), 2.0);

    // 15. If cardinality = 0, return r1.
    if cardinality == 0.0 {
        return r1;
    }

    // 16. Return r2.
    r2
}

/// The quotient and remainder of a BigInt division, Crypto::SignedDivisionResult.
pub struct SignedDivisionResult {
    pub quotient: SignedBigInteger,
    pub remainder: SignedBigInteger,
}

// 13.28 ApplyUnsignedRoundingMode ( x, r1, r2, unsignedRoundingMode ), https://tc39.es/proposal-temporal/#sec-applyunsignedroundingmode
pub fn apply_unsigned_rounding_mode_to_division(
    x: &SignedDivisionResult,
    r1: SignedBigInteger,
    r2: SignedBigInteger,
    unsigned_rounding_mode: UnsignedRoundingMode,
    increment: &SignedBigInteger,
) -> SignedBigInteger {
    // 1. If x = r1, return r1.
    if x.quotient == r1 && x.remainder.is_zero() {
        return r1;
    }

    // 2. Assert: r1 < x < r2.
    // NOTE: Skipped for the sake of performance.

    // 3. Assert: unsignedRoundingMode is not undefined.

    // 4. If unsignedRoundingMode is ZERO, return r1.
    if unsigned_rounding_mode == UnsignedRoundingMode::Zero {
        return r1;
    }

    // 5. If unsignedRoundingMode is INFINITY, return r2.
    if unsigned_rounding_mode == UnsignedRoundingMode::Infinity {
        return r2;
    }

    // 6. Let d1 be x – r1.
    let d1 = &x.remainder;

    // 7. Let d2 be r2 – x.
    let d2 = if x.remainder.is_negative() {
        &x.remainder + increment
    } else {
        &x.remainder - increment
    };
    let d2 = -d2;

    // 8. If d1 < d2, return r1.
    if *d1 < d2 {
        return r1;
    }

    // 9. If d2 < d1, return r2.
    if d2 < *d1 {
        return r2;
    }

    // 10. Assert: d1 is equal to d2.
    // NOTE: Skipped for the sake of performance.

    // 11. If unsignedRoundingMode is HALF-ZERO, return r1.
    if unsigned_rounding_mode == UnsignedRoundingMode::HalfZero {
        return r1;
    }

    // 12. If unsignedRoundingMode is HALF-INFINITY, return r2.
    if unsigned_rounding_mode == UnsignedRoundingMode::HalfInfinity {
        return r2;
    }

    // 13. Assert: unsignedRoundingMode is HALF-EVEN.
    assert!(unsigned_rounding_mode == UnsignedRoundingMode::HalfEven);

    // 14. Let cardinality be (r1 / (r2 – r1)) modulo 2.
    let cardinality = big_modulo(&(&r1 / &(&r2 - &r1)), &SignedBigInteger::from(2));

    // 15. If cardinality = 0, return r1.
    if cardinality.is_zero() {
        return r1;
    }

    // 16. Return r2.
    r2
}

// 13.29 RoundNumberToIncrement ( x, increment, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundnumbertoincrement
pub fn round_number_to_increment(x: f64, increment: u64, rounding_mode: RoundingMode) -> f64 {
    // 1. Let quotient be x / increment.
    let mut quotient = x / increment as f64;

    // 2. If quotient < 0, then
    let is_negative = if quotient < 0.0 {
        // b. Set quotient to -quotient.
        quotient = -quotient;

        // a. Let isNegative be NEGATIVE.
        Sign::Negative
    }
    // 3. Else,
    else {
        // a. Let isNegative be POSITIVE.
        Sign::Positive
    };

    // 4. Let unsignedRoundingMode be GetUnsignedRoundingMode(roundingMode, isNegative).
    let unsigned_rounding_mode = get_unsigned_rounding_mode(rounding_mode, is_negative);

    // 5. Let r1 be the largest integer such that r1 ≤ quotient.
    let r1 = quotient.floor();

    // 6. Let r2 be the smallest integer such that r2 > quotient.
    let mut r2 = quotient.ceil();
    if quotient == r2 {
        r2 += 1.0;
    }

    // 7. Let rounded be ApplyUnsignedRoundingMode(quotient, r1, r2, unsignedRoundingMode).
    let mut rounded = apply_unsigned_rounding_mode(quotient, r1, r2, unsigned_rounding_mode);

    // 8. If isNegative is NEGATIVE, set rounded to -rounded.
    if is_negative == Sign::Negative {
        rounded = -rounded;
    }

    // 9. Return rounded × increment.
    rounded * increment as f64
}

// 13.29 RoundNumberToIncrement ( x, increment, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundnumbertoincrement
pub fn round_big_number_to_increment(
    x: &SignedBigInteger,
    increment: &SignedBigInteger,
    rounding_mode: RoundingMode,
) -> SignedBigInteger {
    // OPTIMIZATION: If the increment is 1 the number is always rounded.
    if increment.is_one() {
        return x.clone();
    }

    // 1. Let quotient be x / increment.
    let (quotient, remainder) = x.div_rem(increment);
    let mut division_result = SignedDivisionResult { quotient, remainder };

    // OPTIMIZATION: If there's no remainder the number is already rounded.
    if division_result.remainder.is_zero() {
        return x.clone();
    }

    // 2. If quotient < 0, then
    let is_negative = if division_result.quotient.sign() == BigIntSign::Minus
        || division_result.remainder.sign() == BigIntSign::Minus
    {
        // b. Set quotient to -quotient.
        division_result.quotient = -division_result.quotient;
        division_result.remainder = -division_result.remainder;

        // a. Let isNegative be NEGATIVE.
        Sign::Negative
    }
    // 3. Else,
    else {
        // a. Let isNegative be POSITIVE.
        Sign::Positive
    };

    // 4. Let unsignedRoundingMode be GetUnsignedRoundingMode(roundingMode, isNegative).
    let unsigned_rounding_mode = get_unsigned_rounding_mode(rounding_mode, is_negative);

    // 5. Let r1 be the largest integer such that r1 ≤ quotient.
    let r1 = division_result.quotient.clone();

    // 6. Let r2 be the smallest integer such that r2 > quotient.
    let r2 = &division_result.quotient + 1;

    // 7. Let rounded be ApplyUnsignedRoundingMode(quotient, r1, r2, unsignedRoundingMode).
    let mut rounded =
        apply_unsigned_rounding_mode_to_division(&division_result, r1, r2, unsigned_rounding_mode, increment);

    // 8. If isNegative is NEGATIVE, set rounded to -rounded.
    if is_negative == Sign::Negative {
        rounded = -rounded;
    }

    // 9. Return rounded × increment.
    rounded * increment
}

// 13.30 RoundNumberToIncrementAsIfPositive ( x, increment, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundnumbertoincrementasifpositive
pub fn round_number_to_increment_as_if_positive(
    x: &SignedBigInteger,
    increment: &SignedBigInteger,
    rounding_mode: RoundingMode,
) -> SignedBigInteger {
    // OPTIMIZATION: If the increment is 1 the number is always rounded.
    if increment.is_one() {
        return x.clone();
    }

    // 1. Let quotient be x / increment.
    let (quotient, remainder) = x.div_rem(increment);
    let division_result = SignedDivisionResult { quotient, remainder };

    // OPTIMIZATION: If there's no remainder the number is already rounded.
    if division_result.remainder.is_zero() {
        return x.clone();
    }

    // 2. Let unsignedRoundingMode be GetUnsignedRoundingMode(roundingMode, POSITIVE).
    let unsigned_rounding_mode = get_unsigned_rounding_mode(rounding_mode, Sign::Positive);

    // 3. Let r1 be the largest integer such that r1 ≤ quotient.
    // 4. Let r2 be the smallest integer such that r2 > quotient.
    let (r1, r2) = if x.is_negative() {
        (&division_result.quotient - 1, division_result.quotient.clone())
    } else {
        (division_result.quotient.clone(), &division_result.quotient + 1)
    };

    // 5. Let rounded be ApplyUnsignedRoundingMode(quotient, r1, r2, unsignedRoundingMode).
    let rounded = apply_unsigned_rounding_mode_to_division(&division_result, r1, r2, unsigned_rounding_mode, increment);

    // 6. Return rounded × increment.
    rounded * increment
}

fn view_to_number(view: Utf16View<'_>) -> f64 {
    let code_units: Vec<u16> = view.code_units().collect();
    string_to_number(&code_units)
}

// 13.35 ParseISODateTime ( isoString, allowedFormats ), https://tc39.es/proposal-temporal/#sec-temporal-parseisodatetime
pub fn parse_iso_date_time(
    vm: &Vm,
    iso_string: Utf16View<'_>,
    allowed_formats: &[Production],
) -> ThrowCompletionOr<ParsedISODateTime> {
    // 1. Let parseResult be EMPTY.
    let mut parse_result = None;

    // 2. Let calendar be EMPTY.
    let mut calendar: Option<Utf16String> = None;

    // 3. Let yearAbsent be false.
    let mut year_absent = false;

    // 4. For each nonterminal goal of allowedFormats, do
    for &goal in allowed_formats {
        // a. If parseResult is not a Parse Node, then
        if parse_result.is_some() {
            break;
        }

        // i. Set parseResult to ParseText(StringToCodePoints(isoString), goal).
        parse_result = parse_iso8601(goal, iso_string);

        // ii. If parseResult is a Parse Node, then
        if let Some(result) = &parse_result {
            // 1. Let calendarWasCritical be false.
            let mut calendar_was_critical = false;

            // 2. For each Annotation Parse Node annotation contained within parseResult, do
            for annotation in &result.annotations {
                // a. Let key be the source text matched by the AnnotationKey Parse Node contained within annotation.
                let key = annotation.key;

                // b. Let value be the source text matched by the AnnotationValue Parse Node contained within annotation.
                let value = annotation.value;

                // c. If CodePointsToString(key) is "u-ca", then
                if key == "u-ca" {
                    // i. If calendar is EMPTY, then
                    if calendar.is_none() {
                        // i. Set calendar to CodePointsToString(value).
                        calendar = Some(value.to_utf16_string());

                        // ii. If annotation contains an AnnotationCriticalFlag Parse Node, set calendarWasCritical to true.
                        if annotation.critical {
                            calendar_was_critical = true;
                        }
                    }
                    // ii. Else,
                    else {
                        // i. If annotation contains an AnnotationCriticalFlag Parse Node, or calendarWasCritical is true,
                        //    throw a RangeError exception.
                        if annotation.critical || calendar_was_critical {
                            return vm.throw_completion(
                                ErrorKind::RangeError,
                                ErrorType::TemporalInvalidCriticalAnnotation,
                                &[&key, &value],
                            );
                        }
                    }
                }
                // d. Else,
                else {
                    // i. If annotation contains an AnnotationCriticalFlag Parse Node, throw a RangeError exception.
                    if annotation.critical {
                        return vm.throw_completion(
                            ErrorKind::RangeError,
                            ErrorType::TemporalInvalidCriticalAnnotation,
                            &[&key, &value],
                        );
                    }
                }
            }

            // 3. If goal is TemporalYearMonthString and parseResult does not contain a DateDay Parse Node, then
            // 4. If goal is TemporalMonthDayString and parseResult does not contain a DateYear Parse Node, then
            if (goal == Production::TemporalYearMonthString && result.date_day.is_none())
                || (goal == Production::TemporalMonthDayString && result.date_year.is_none())
            {
                // a. If calendar is not EMPTY and the ASCII-lowercase of calendar is not "iso8601", throw a RangeError exception.
                if let Some(calendar) = &calendar
                    && !Utf16View::of_string(calendar).equals_ignoring_ascii_case(ascii_view("iso8601"))
                {
                    return vm.throw_completion(
                        ErrorKind::RangeError,
                        ErrorType::TemporalInvalidCalendarIdentifier,
                        &[calendar],
                    );
                }

                // b. Set yearAbsent to true.
                if goal == Production::TemporalMonthDayString {
                    year_absent = true;
                }
            }
        }
    }

    // 5. If parseResult is not a Parse Node, throw a RangeError exception.
    let Some(parse_result) = parse_result else {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidISODateTime, &[]);
    };

    // 6. NOTE: Applications of StringToNumber below do not lose precision, since each of the parsed values is guaranteed
    //    to be a sufficiently short string of decimal digits.

    // 7. Let each of year, month, day, hour, minute, second, and fSeconds be the source text matched by the respective
    //    DateYear, DateMonth, DateDay, the first Hour, the first MinuteSecond, TimeSecond, and the first
    //    TemporalDecimalFraction Parse Node contained within parseResult, or an empty sequence of code points if not present.
    let year = parse_result.date_year.unwrap_or(Utf16View::EMPTY);
    let month = parse_result.date_month.unwrap_or(Utf16View::EMPTY);
    let day = parse_result.date_day.unwrap_or(Utf16View::EMPTY);
    let hour = parse_result.time_hour.unwrap_or(Utf16View::EMPTY);
    let minute = parse_result.time_minute.unwrap_or(Utf16View::EMPTY);
    let second = parse_result.time_second.unwrap_or(Utf16View::EMPTY);
    let fractional_seconds = parse_result.time_fraction.unwrap_or(Utf16View::EMPTY);

    // 8. Let yearMV be ℝ(StringToNumber(CodePointsToString(year))).
    let year_value = view_to_number(year);

    // 9. If month is empty, then
    //        a. Let monthMV be 1.
    // 10. Else,
    //         a. Let monthMV be ℝ(StringToNumber(CodePointsToString(month))).
    let month_value = if month.is_empty() { 1.0 } else { view_to_number(month) };

    // 11. If day is empty, then
    //         a. Let dayMV be 1.
    // 12. Else,
    //         a. Let dayMV be ℝ(StringToNumber(CodePointsToString(day))).
    let day_value = if day.is_empty() { 1.0 } else { view_to_number(day) };

    // 13. If hour is empty, then
    //         a. Let hourMV be 0.
    // 14. Else,
    //         a. Let hourMV be ℝ(StringToNumber(CodePointsToString(hour))).
    let hour_value = if hour.is_empty() { 0.0 } else { view_to_number(hour) };

    // 15. If minute is empty, then
    //         a. Let minuteMV be 0.
    // 16. Else,
    //         a. Let minuteMV be ℝ(StringToNumber(CodePointsToString(minute))).
    let minute_value = if minute.is_empty() { 0.0 } else { view_to_number(minute) };

    // 17. If second is empty, then
    //         a. Let secondMV be 0.
    // 18. Else,
    //         a. Let secondMV be ℝ(StringToNumber(CodePointsToString(second))).
    //         b. If secondMV = 60, then
    //                i. Set secondMV to 59.
    let second_value = if second.is_empty() {
        0.0
    } else {
        view_to_number(second).min(59.0)
    };

    let mut millisecond_value = 0.0;
    let mut microsecond_value = 0.0;
    let mut nanosecond_value = 0.0;

    // 19. If fSeconds is not empty, then
    if !fractional_seconds.is_empty() {
        // a. Let fSecondsDigits be the substring of CodePointsToString(fSeconds) from 1.
        let parse_fractional_digits = |digits: Utf16View<'_>, offset: usize| {
            let mut value = 0.0;
            for i in 0..3 {
                value *= 10.0;
                let index = offset + i;
                if index < digits.length_in_code_units() {
                    value += f64::from(digits.code_unit_at(index) - u16::from(b'0'));
                }
            }
            value
        };

        let fractional_seconds_digits =
            fractional_seconds.substring_view(1, fractional_seconds.length_in_code_units() - 1);

        // b. Let fSecondsDigitsExtended be the string-concatenation of fSecondsDigits and "000000000".
        // c. Let millisecond be the substring of fSecondsDigitsExtended from 0 to 3.
        // f. Let millisecondMV be ℝ(StringToNumber(millisecond)).
        millisecond_value = parse_fractional_digits(fractional_seconds_digits, 0);

        // d. Let microsecond be the substring of fSecondsDigitsExtended from 3 to 6.
        // g. Let microsecondMV be ℝ(StringToNumber(microsecond)).
        microsecond_value = parse_fractional_digits(fractional_seconds_digits, 3);

        // e. Let nanosecond be the substring of fSecondsDigitsExtended from 6 to 9.
        // h. Let nanosecondMV be ℝ(StringToNumber(nanosecond)).
        nanosecond_value = parse_fractional_digits(fractional_seconds_digits, 6);
    }
    // 20. Else,
    //     a. Let millisecondMV be 0.
    //     b. Let microsecondMV be 0.
    //     c. Let nanosecondMV be 0.

    // 21. Assert: IsValidISODate(yearMV, monthMV, dayMV) is true.
    assert!(is_valid_iso_date(year_value, month_value, day_value));

    // 22. If hour is empty, then
    let time = if hour.is_empty() {
        // a. Let time be START-OF-DAY.
        TimeOrStartOfDay::StartOfDay
    }
    // 23. Else,
    else {
        // a. Let time be CreateTimeRecord(hourMV, minuteMV, secondMV, millisecondMV, microsecondMV, nanosecondMV).
        TimeOrStartOfDay::Time(create_time_record(
            hour_value,
            minute_value,
            second_value,
            millisecond_value,
            microsecond_value,
            nanosecond_value,
            0.0,
        ))
    };

    // 24. Let timeZoneResult be ISO String Time Zone Parse Record { [[Z]]: false, [[OffsetString]]: EMPTY, [[TimeZoneAnnotation]]: EMPTY }.
    let mut time_zone_result = ParsedISOTimeZone::default();

    // 25. If parseResult contains a TimeZoneIdentifier Parse Node, then
    if let Some(identifier) = parse_result.time_zone_identifier {
        // a. Let identifier be the source text matched by the TimeZoneIdentifier Parse Node contained within parseResult.
        // b. Set timeZoneResult.[[TimeZoneAnnotation]] to CodePointsToString(identifier).
        time_zone_result.time_zone_annotation = Some(identifier.to_utf16_string());
    }

    // 26. If parseResult contains a UTCDesignator Parse Node, then
    if parse_result.utc_designator.is_some() {
        // a. Set timeZoneResult.[[Z]] to true.
        time_zone_result.z_designator = true;
    }
    // 27. Else if parseResult contains a UTCOffset[+SubMinutePrecision] Parse Node, then
    else if let Some(date_time_offset) = &parse_result.date_time_offset {
        // a. Let offset be the source text matched by the UTCOffset[+SubMinutePrecision] Parse Node contained within parseResult.
        // b. Set timeZoneResult.[[OffsetString]] to CodePointsToString(offset).
        time_zone_result.offset_string = Some(date_time_offset.source_text.to_utf16_string());
    }

    // 28. If yearAbsent is true, let yearReturn be EMPTY; else let yearReturn be yearMV.
    let year_return = (!year_absent).then_some(year_value as i32);

    // 29. Return ISO Date-Time Parse Record { [[Year]]: yearReturn, [[Month]]: monthMV, [[Day]]: dayMV, [[Time]]: time, [[TimeZone]]: timeZoneResult, [[Calendar]]: calendar  }.
    Ok(ParsedISODateTime {
        year: year_return,
        month: month_value as u8,
        day: day_value as u8,
        time,
        time_zone: time_zone_result,
        calendar,
    })
}

const ALL_DATE_TIME_PRODUCTIONS: [Production; 6] = [
    Production::TemporalZonedDateTimeString,
    Production::TemporalDateTimeString,
    Production::TemporalInstantString,
    Production::TemporalTimeString,
    Production::TemporalMonthDayString,
    Production::TemporalYearMonthString,
];

// 13.36 ParseTemporalCalendarString ( string ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporalcalendarstring
pub fn parse_temporal_calendar_string(vm: &Vm, string: Utf16View<'_>) -> ThrowCompletionOr<Utf16String> {
    // 1. Let parseResult be Completion(ParseISODateTime(string, « TemporalDateTimeString[+Zoned], TemporalDateTimeString[~Zoned],
    //    TemporalInstantString, TemporalTimeString, TemporalMonthDayString, TemporalYearMonthString »)).
    let parse_result = parse_iso_date_time(vm, string, &ALL_DATE_TIME_PRODUCTIONS);

    // 2. If parseResult is a normal completion, then
    if let Ok(parse_result) = parse_result {
        // a. Let calendar be parseResult.[[Value]].[[Calendar]].
        // b. If calendar is EMPTY, return "iso8601".
        // c. Else, return calendar.
        return Ok(parse_result
            .calendar
            .unwrap_or_else(|| Utf16String::from_utf8(ISO8601_CALENDAR)));
    }

    // 3. Set parseResult to ParseText(StringToCodePoints(string), AnnotationValue).
    let annotation_parse_result = parse_iso8601(Production::AnnotationValue, string);

    // 4. If parseResult is a List of errors, throw a RangeError exception.
    if annotation_parse_result.is_none() {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TemporalInvalidCalendarString,
            &[&string],
        );
    }

    // 5. Return string.
    Ok(string.to_utf16_string())
}

// 13.37 ParseTemporalDurationString ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporaldurationstring
pub fn parse_temporal_duration_string(vm: &Vm, iso_string: Utf16View<'_>) -> ThrowCompletionOr<Gc<Duration>> {
    // 1. Let duration be ParseText(StringToCodePoints(isoString), TemporalDurationString).
    let parse_result = parse_iso8601(Production::TemporalDurationString, iso_string);

    // 2. If duration is a List of errors, throw a RangeError exception.
    let Some(parse_result) = parse_result else {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TemporalInvalidDurationString,
            &[&iso_string],
        );
    };

    let to_integer = |digits: Utf16View<'_>| {
        to_integer_with_truncation_of_string(vm, digits, ErrorType::TemporalInvalidDurationString, &[&iso_string])
    };

    // 3. Let sign be the source text matched by the ASCIISign Parse Node contained within duration, or an empty sequence
    //    of code points if not present.
    let sign = parse_result.sign;

    // 4. If duration contains a DurationYearsPart Parse Node, then
    //        a. Let yearsNode be that DurationYearsPart Parse Node contained within duration.
    //        b. Let years be the source text matched by the DecimalDigits Parse Node contained within yearsNode.
    // 5. Else,
    //        a. Let years be an empty sequence of code points.
    let years = parse_result.duration_years.unwrap_or(Utf16View::EMPTY);

    // 6. If duration contains a DurationMonthsPart Parse Node, then
    //        a. Let monthsNode be the DurationMonthsPart Parse Node contained within duration.
    //        b. Let months be the source text matched by the DecimalDigits Parse Node contained within monthsNode.
    // 7. Else,
    //        a. Let months be an empty sequence of code points.
    let months = parse_result.duration_months.unwrap_or(Utf16View::EMPTY);

    // 8. If duration contains a DurationWeeksPart Parse Node, then
    //        a. Let weeksNode be the DurationWeeksPart Parse Node contained within duration.
    //        b. Let weeks be the source text matched by the DecimalDigits Parse Node contained within weeksNode.
    // 9. Else,
    //        a. Let weeks be an empty sequence of code points.
    let weeks = parse_result.duration_weeks.unwrap_or(Utf16View::EMPTY);

    // 10. If duration contains a DurationDaysPart Parse Node, then
    //         a. Let daysNode be the DurationDaysPart Parse Node contained within duration.
    //         b. Let days be the source text matched by the DecimalDigits Parse Node contained within daysNode.
    // 11. Else,
    //         a. Let days be an empty sequence of code points.
    let days = parse_result.duration_days.unwrap_or(Utf16View::EMPTY);

    // 12. If duration contains a DurationHoursPart Parse Node, then
    //         a. Let hoursNode be the DurationHoursPart Parse Node contained within duration.
    //         b. Let hours be the source text matched by the DecimalDigits Parse Node contained within hoursNode.
    //         c. Let fHours be the source text matched by the TemporalDecimalFraction Parse Node contained within
    //            hoursNode, or an empty sequence of code points if not present.
    // 13. Else,
    //         a. Let hours be an empty sequence of code points.
    //         b. Let fHours be an empty sequence of code points.
    let hours = parse_result.duration_hours.unwrap_or(Utf16View::EMPTY);
    let fractional_hours = parse_result.duration_hours_fraction.unwrap_or(Utf16View::EMPTY);

    // 14. If duration contains a DurationMinutesPart Parse Node, then
    //         a. Let minutesNode be the DurationMinutesPart Parse Node contained within duration.
    //         b. Let minutes be the source text matched by the DecimalDigits Parse Node contained within minutesNode.
    //         c. Let fMinutes be the source text matched by the TemporalDecimalFraction Parse Node contained within
    //            minutesNode, or an empty sequence of code points if not present.
    // 15. Else,
    //         a. Let minutes be an empty sequence of code points.
    //         b. Let fMinutes be an empty sequence of code points.
    let minutes = parse_result.duration_minutes.unwrap_or(Utf16View::EMPTY);
    let fractional_minutes = parse_result.duration_minutes_fraction.unwrap_or(Utf16View::EMPTY);

    // 16. If duration contains a DurationSecondsPart Parse Node, then
    //         a. Let secondsNode be the DurationSecondsPart Parse Node contained within duration.
    //         b. Let seconds be the source text matched by the DecimalDigits Parse Node contained within secondsNode.
    //         c. Let fSeconds be the source text matched by the TemporalDecimalFraction Parse Node contained within
    //            secondsNode, or an empty sequence of code points if not present.
    // 17. Else,
    //         a. Let seconds be an empty sequence of code points.
    //         b. Let fSeconds be an empty sequence of code points.
    let seconds = parse_result.duration_seconds.unwrap_or(Utf16View::EMPTY);
    let fractional_seconds = parse_result.duration_seconds_fraction.unwrap_or(Utf16View::EMPTY);

    // 18. Let yearsMV be ? ToIntegerWithTruncation(CodePointsToString(years)).
    let mut years_value = to_integer(years)?;

    // 19. Let monthsMV be ? ToIntegerWithTruncation(CodePointsToString(months)).
    let mut months_value = to_integer(months)?;

    // 20. Let weeksMV be ? ToIntegerWithTruncation(CodePointsToString(weeks)).
    let mut weeks_value = to_integer(weeks)?;

    // 21. Let daysMV be ? ToIntegerWithTruncation(CodePointsToString(days)).
    let mut days_value = to_integer(days)?;

    // 22. Let hoursMV be ? ToIntegerWithTruncation(CodePointsToString(hours)).
    let mut hours_value = to_integer(hours)?;

    let remainder_one = |value: &BigFraction| {
        // FIXME: We should add a generic remainder() method to BigFraction, or a method equivalent to modf(). But for
        //        now, since we know we are only dividing by powers of 10, we can implement a very situationally specific
        //        method to extract the fractional part of the BigFraction.
        let remainder = value.numerator() % value.denominator();
        BigFraction::new(remainder, value.denominator().clone())
    };
    fn without_separator(fraction: Utf16View<'_>) -> Utf16View<'_> {
        fraction.substring_view(1, fraction.length_in_code_units() - 1)
    }
    let power_of_ten = |scale: usize| BigFraction::from_double(10f64.powi(scale as i32));

    // 23. If fHours is not empty, then
    let minutes_value = if !fractional_hours.is_empty() {
        // a. Assert: minutes, fMinutes, seconds, and fSeconds are empty.
        assert!(minutes.is_empty());
        assert!(fractional_minutes.is_empty());
        assert!(seconds.is_empty());
        assert!(fractional_seconds.is_empty());

        // b. Let fHoursDigits be the substring of CodePointsToString(fHours) from 1.
        let fractional_hours_digits = without_separator(fractional_hours);

        // c. Let fHoursScale be the length of fHoursDigits.
        let fractional_hours_scale = fractional_hours_digits.length_in_code_units();

        // d. Let minutesMV be ? ToIntegerWithTruncation(fHoursDigits) / 10**fHoursScale × 60.
        let minutes_integer = to_integer(fractional_hours_digits)?;
        &(&BigFraction::from_double(minutes_integer) / &power_of_ten(fractional_hours_scale))
            * &BigFraction::from_double(60.0)
    }
    // 24. Else,
    else {
        // a. Let minutesMV be ? ToIntegerWithTruncation(CodePointsToString(minutes)).
        let minutes_integer = to_integer(minutes)?;
        BigFraction::from_double(minutes_integer)
    };

    // 25. If fMinutes is not empty, then
    let seconds_value = if !fractional_minutes.is_empty() {
        // a. Assert: seconds and fSeconds are empty.
        assert!(seconds.is_empty());
        assert!(fractional_seconds.is_empty());

        // b. Let fMinutesDigits be the substring of CodePointsToString(fMinutes) from 1.
        let fractional_minutes_digits = without_separator(fractional_minutes);

        // c. Let fMinutesScale be the length of fMinutesDigits.
        let fractional_minutes_scale = fractional_minutes_digits.length_in_code_units();

        // d. Let secondsMV be ? ToIntegerWithTruncation(fMinutesDigits) / 10**fMinutesScale × 60.
        let seconds_integer = to_integer(fractional_minutes_digits)?;
        &(&BigFraction::from_double(seconds_integer) / &power_of_ten(fractional_minutes_scale))
            * &BigFraction::from_double(60.0)
    }
    // 26. Else if seconds is not empty, then
    else if !seconds.is_empty() {
        // a. Let secondsMV be ? ToIntegerWithTruncation(CodePointsToString(seconds)).
        let seconds_integer = to_integer(seconds)?;
        BigFraction::from_double(seconds_integer)
    }
    // 27. Else,
    else {
        // a. Let secondsMV be remainder(minutesMV, 1) × 60.
        &remainder_one(&minutes_value) * &BigFraction::from_double(60.0)
    };

    // 28. If fSeconds is not empty, then
    let milliseconds_value = if !fractional_seconds.is_empty() {
        // a. Let fSecondsDigits be the substring of CodePointsToString(fSeconds) from 1.
        let fractional_seconds_digits = without_separator(fractional_seconds);

        // b. Let fSecondsScale be the length of fSecondsDigits.
        let fractional_seconds_scale = fractional_seconds_digits.length_in_code_units();

        // c. Let millisecondsMV be ? ToIntegerWithTruncation(fSecondsDigits) / 10**fSecondsScale × 1000.
        let milliseconds_integer = to_integer(fractional_seconds_digits)?;
        &(&BigFraction::from_double(milliseconds_integer) / &power_of_ten(fractional_seconds_scale))
            * &BigFraction::from_double(1000.0)
    }
    // 29. Else,
    else {
        // a. Let millisecondsMV be remainder(secondsMV, 1) × 1000.
        &remainder_one(&seconds_value) * &BigFraction::from_double(1000.0)
    };

    // 30. Let microsecondsMV be remainder(millisecondsMV, 1) × 1000.
    let microseconds_value = &remainder_one(&milliseconds_value) * &BigFraction::from_double(1000.0);

    // 31. Let nanosecondsMV be remainder(microsecondsMV, 1) × 1000.
    let nanoseconds_value = &remainder_one(&microseconds_value) * &BigFraction::from_double(1000.0);

    // 32. If sign contains the code point U+002D (HYPHEN-MINUS), then
    //     a. Let factor be -1.
    // 33. Else,
    //     a. Let factor be 1.
    let factor = if sign == Some(b'-') { -1.0 } else { 1.0 };

    // 34. Set yearsMV to yearsMV × factor.
    years_value *= factor;

    // 35. Set monthsMV to monthsMV × factor.
    months_value *= factor;

    // 36. Set weeksMV to weeksMV × factor.
    weeks_value *= factor;

    // 37. Set daysMV to daysMV × factor.
    days_value *= factor;

    // 38. Set hoursMV to hoursMV × factor.
    hours_value *= factor;

    // 39. Set minutesMV to floor(minutesMV) × factor.
    let factored_minutes_value = minutes_value.to_double().floor() * factor;

    // 40. Set secondsMV to floor(secondsMV) × factor.
    let factored_seconds_value = seconds_value.to_double().floor() * factor;

    // 41. Set millisecondsMV to floor(millisecondsMV) × factor.
    let factored_milliseconds_value = milliseconds_value.to_double().floor() * factor;

    // 42. Set microsecondsMV to floor(microsecondsMV) × factor.
    let factored_microseconds_value = microseconds_value.to_double().floor() * factor;

    // 43. Set nanosecondsMV to floor(nanosecondsMV) × factor.
    let factored_nanoseconds_value = nanoseconds_value.to_double().floor() * factor;

    // 44. Return ? CreateTemporalDuration(yearsMV, monthsMV, weeksMV, daysMV, hoursMV, minutesMV, secondsMV, millisecondsMV, microsecondsMV, nanosecondsMV).
    create_temporal_duration(
        vm,
        DurationFields {
            years: years_value,
            months: months_value,
            weeks: weeks_value,
            days: days_value,
            hours: hours_value,
            minutes: factored_minutes_value,
            seconds: factored_seconds_value,
            milliseconds: factored_milliseconds_value,
            microseconds: factored_microseconds_value,
            nanoseconds: factored_nanoseconds_value,
        },
        None,
    )
}

// 13.38 ParseTemporalTimeZoneString ( timeZoneString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporaltimezonestring
pub fn parse_temporal_time_zone_string(
    vm: &Vm,
    time_zone_string: Utf16View<'_>,
) -> ThrowCompletionOr<ParsedTimeZoneIdentifier> {
    // 1. Let parseResult be ParseText(StringToCodePoints(timeZoneString), TimeZoneIdentifier).
    let parse_result = parse_iso8601(Production::TimeZoneIdentifier, time_zone_string);

    // 2. If parseResult is a Parse Node, return ! ParseTimeZoneIdentifier(timeZoneString).
    if let Some(parse_result) = parse_result {
        return Ok(parse_time_zone_identifier_from_parse_result(&parse_result));
    }

    // 3. Let result be ? ParseISODateTime(timeZoneString, « TemporalDateTimeString[+Zoned], TemporalDateTimeString[~Zoned],
    //    TemporalInstantString, TemporalTimeString, TemporalMonthDayString, TemporalYearMonthString »).
    let result = parse_iso_date_time(vm, time_zone_string, &ALL_DATE_TIME_PRODUCTIONS)?;

    // 4. Let timeZoneResult be result.[[TimeZone]].
    let time_zone_result = result.time_zone;

    // 5. If timeZoneResult.[[TimeZoneAnnotation]] is not EMPTY, return ! ParseTimeZoneIdentifier(timeZoneResult.[[TimeZoneAnnotation]]).
    if let Some(time_zone_annotation) = &time_zone_result.time_zone_annotation {
        return parse_time_zone_identifier_or_throw(vm, Utf16View::of_string(time_zone_annotation));
    }

    // 6. If timeZoneResult.[[Z]] is true, return ! ParseTimeZoneIdentifier("UTC").
    if time_zone_result.z_designator {
        return Ok(crate::runtime::temporal::time_zone::parse_time_zone_identifier(
            ascii_view(UTC_TIME_ZONE),
        ));
    }

    // 7. If timeZoneResult.[[OffsetString]] is not EMPTY, return ? ParseTimeZoneIdentifier(timeZoneResult.[[OffsetString]]).
    if let Some(offset_string) = &time_zone_result.offset_string {
        return parse_time_zone_identifier_or_throw(vm, Utf16View::of_string(offset_string));
    }

    // 8. Throw a RangeError exception.
    vm.throw_completion(
        ErrorKind::RangeError,
        ErrorType::TemporalInvalidTimeZoneString,
        &[&time_zone_string],
    )
}

// 13.41 ToOffsetString ( argument ), https://tc39.es/proposal-temporal/#sec-temporal-tooffsetstring
pub fn to_offset_string(vm: &Vm, argument: Value) -> ThrowCompletionOr<Utf16String> {
    // 1. Let offset be ? ToPrimitive(argument, STRING).
    let offset = argument.to_primitive(vm, crate::runtime::value::PreferredType::String)?;

    // 2. If offset is not a String, throw a TypeError exception.
    if !offset.is_string() {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::TemporalInvalidTimeZoneString,
            &[&offset],
        );
    }

    // 3. Perform ? ParseDateTimeUTCOffset(offset).
    let offset_string = offset.as_string().utf16_string();
    parse_date_time_utc_offset_or_throw(vm, Utf16View::of_string(&offset_string))?;

    // 4. Return offset.
    Ok(offset_string)
}

// 13.42 ISODateToFields ( calendar, isoDate, type ), https://tc39.es/proposal-temporal/#sec-temporal-isodatetofields
pub fn iso_date_to_fields(calendar: Utf16View<'_>, iso_date: ISODate, date_type: DateType) -> CalendarFields {
    // 1. Let fields be an empty Calendar Fields Record with all fields set to unset.
    let mut fields = CalendarFields::unset();

    // 2. Let calendarDate be CalendarISOToDate(calendar, isoDate).
    let calendar_date = calendar_iso_to_date(calendar, iso_date);

    // 3. Set fields.[[MonthCode]] to calendarDate.[[MonthCode]].
    fields.month_code = Some(calendar_date.month_code);

    // 4. If type is either MONTH-DAY or DATE, then
    if date_type == DateType::MonthDay || date_type == DateType::Date {
        // a. Set fields.[[Day]] to calendarDate.[[Day]].
        fields.day = Some(u32::from(calendar_date.day));
    }

    // 5. If type is either YEAR-MONTH or DATE, then
    if date_type == DateType::YearMonth || date_type == DateType::Date {
        // a. Set fields.[[Year]] to calendarDate.[[Year]].
        fields.year = Some(calendar_date.year);
    }

    // 6. Return fields.
    fields
}

// 13.43 GetDifferenceSettings ( operation, options, unitGroup, disallowedUnits, fallbackSmallestUnit, smallestLargestDefaultUnit ), https://tc39.es/proposal-temporal/#sec-temporal-getdifferencesettings
pub fn get_difference_settings(
    vm: &Vm,
    operation: DurationOperation,
    options: &Object,
    unit_group: UnitGroup,
    disallowed_units: &[Unit],
    fallback_smallest_unit: Unit,
    smallest_largest_default_unit: Unit,
) -> ThrowCompletionOr<DifferenceSettings> {
    // 1. NOTE: The following steps read options and perform independent validation in alphabetical order.

    // 2. Let largestUnit be ? GetTemporalUnitValuedOption(options, "largestUnit", UNSET).
    let mut largest_unit = get_temporal_unit_valued_option(vm, options, &vm.names.largestUnit, UnitDefault::Unset)?;

    // 3. Let roundingIncrement be ? GetRoundingIncrementOption(options).
    let rounding_increment = get_rounding_increment_option(vm, options)?;

    // 4. Let roundingMode be ? GetRoundingModeOption(options, TRUNC).
    let mut rounding_mode = get_rounding_mode_option(vm, options, RoundingMode::Trunc)?;

    // 5. Let smallestUnit be ? GetTemporalUnitValuedOption(options, "smallestUnit", UNSET).
    let smallest_unit = get_temporal_unit_valued_option(vm, options, &vm.names.smallestUnit, UnitDefault::Unset)?;

    // 6. Perform ? ValidateTemporalUnitValue(largestUnit, unitGroup, « AUTO »).
    validate_temporal_unit_value(vm, &vm.names.largestUnit, largest_unit, unit_group, &[UnitValue::Auto])?;

    // 7. If largestUnit is UNSET, then
    if largest_unit == UnitValue::Unset {
        // a. Set largestUnit to AUTO.
        largest_unit = UnitValue::Auto;
    }

    // 8. If disallowedUnits contains largestUnit, throw a RangeError exception.
    if let UnitValue::Unit(unit) = largest_unit
        && disallowed_units.contains(&unit)
    {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::OptionIsNotValidValue,
            &[&temporal_unit_to_string(unit), &vm.names.largestUnit],
        );
    }

    // 9. Perform ? ValidateTemporalUnitValue(smallestUnit, unitGroup).
    validate_temporal_unit_value(vm, &vm.names.smallestUnit, smallest_unit, unit_group, &[])?;

    // 10. If smallestUnit is UNSET, then
    //     a. Set smallestUnit to fallbackSmallestUnit.
    let smallest_unit_value = match smallest_unit {
        UnitValue::Unit(unit) => unit,
        _ => fallback_smallest_unit,
    };

    // 11. If disallowedUnits contains smallestUnit, throw a RangeError exception.
    if disallowed_units.contains(&smallest_unit_value) {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::OptionIsNotValidValue,
            &[&temporal_unit_to_string(smallest_unit_value), &vm.names.smallestUnit],
        );
    }

    // 12. Let defaultLargestUnit be LargerOfTwoTemporalUnits(smallestLargestDefaultUnit, smallestUnit).
    let default_largest_unit = larger_of_two_temporal_units(smallest_largest_default_unit, smallest_unit_value);

    // 13. If largestUnit is AUTO, set largestUnit to defaultLargestUnit.
    let largest_unit_value = match largest_unit {
        UnitValue::Unit(unit) => unit,
        _ => default_largest_unit,
    };

    // 14. If LargerOfTwoTemporalUnits(largestUnit, smallestUnit) is not largestUnit, throw a RangeError exception.
    if larger_of_two_temporal_units(largest_unit_value, smallest_unit_value) != largest_unit_value {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TemporalInvalidUnitRange,
            &[
                &temporal_unit_to_string(smallest_unit_value),
                &temporal_unit_to_string(largest_unit_value),
            ],
        );
    }

    // 15. Let maximum be MaximumTemporalDurationRoundingIncrement(smallestUnit).
    let maximum = maximum_temporal_duration_rounding_increment(smallest_unit_value);

    // 16. If maximum is not UNSET, perform ? ValidateTemporalRoundingIncrement(roundingIncrement, maximum, false).
    if let Some(maximum) = maximum {
        validate_temporal_rounding_increment(vm, rounding_increment, maximum, false)?;
    }

    // 17. If operation is SINCE, then
    if operation == DurationOperation::Since {
        // a. Set roundingMode to NegateRoundingMode(roundingMode).
        rounding_mode = negate_rounding_mode(rounding_mode);
    }

    // 18. Return the Record { [[SmallestUnit]]: smallestUnit, [[LargestUnit]]: largestUnit, [[RoundingMode]]: roundingMode, [[RoundingIncrement]]: roundingIncrement,  }.
    Ok(DifferenceSettings {
        smallest_unit: smallest_unit_value,
        largest_unit: largest_unit_value,
        rounding_mode,
        rounding_increment,
    })
}

// 13.40 ToIntegerWithTruncation ( argument ), https://tc39.es/proposal-temporal/#sec-tointegerwithtruncation
pub fn to_integer_with_truncation(
    vm: &Vm,
    argument: Value,
    error_type: ErrorType,
    arguments: &[&dyn Utf16Display],
) -> ThrowCompletionOr<f64> {
    // 1. Let number be ? ToNumber(argument).
    let number = argument.to_number(vm)?;

    // 2. If number is one of NaN, +∞𝔽, or -∞𝔽, throw a RangeError exception.
    if number.is_nan() || number.is_infinity() {
        return vm.throw_completion(ErrorKind::RangeError, error_type, arguments);
    }

    // 3. Return truncate(ℝ(number)).
    Ok(number.as_f64().trunc())
}

// 13.40 ToIntegerWithTruncation ( argument ), https://tc39.es/proposal-temporal/#sec-tointegerwithtruncation
// AD-HOC: We often need to use this AO when we have a parsed Utf16View. This overload allows callers to avoid creating
//         a PrimitiveString for the primary definition.
pub fn to_integer_with_truncation_of_string(
    vm: &Vm,
    argument: Utf16View<'_>,
    error_type: ErrorType,
    arguments: &[&dyn Utf16Display],
) -> ThrowCompletionOr<f64> {
    // 1. Let number be ? ToNumber(argument).
    let number = view_to_number(argument);

    // 2. If number is one of NaN, +∞𝔽, or -∞𝔽, throw a RangeError exception.
    if number.is_nan() || number.is_infinite() {
        return vm.throw_completion(ErrorKind::RangeError, error_type, arguments);
    }

    // 3. Return truncate(ℝ(number)).
    Ok(number.trunc())
}

// 13.39 ToPositiveIntegerWithTruncation ( argument ), https://tc39.es/proposal-temporal/#sec-topositiveintegerwithtruncation
pub fn to_positive_integer_with_truncation(
    vm: &Vm,
    argument: Value,
    error_type: ErrorType,
    arguments: &[&dyn Utf16Display],
) -> ThrowCompletionOr<f64> {
    // 1. Let integer be ? ToIntegerWithTruncation(argument).
    let integer = to_integer_with_truncation(vm, argument, error_type, arguments)?;

    // 2. If integer ≤ 0, throw a RangeError exception.
    if integer <= 0.0 {
        return vm.throw_completion(ErrorKind::RangeError, error_type, arguments);
    }

    // 3. Return integer.
    Ok(integer)
}

/// Trims the trailing code units of a view that are `code_unit`, as AK's Utf16String::trim(..., TrimMode::Right).
pub fn trim_trailing(view: Utf16View<'_>, code_unit: u8) -> Utf16View<'_> {
    view.trim(&[u16::from(code_unit)], TrimMode::Right)
}

/// Whether a BigInt is negative, for the BigInt arithmetic of Temporal.
pub fn is_negative(value: &SignedBigInteger) -> bool {
    value.is_negative()
}

/// The BigInt `value` divided by `divisor`, truncating, with the remainder, as Crypto's divided_by().
pub fn divided_by(value: &SignedBigInteger, divisor: &SignedBigInteger) -> SignedDivisionResult {
    let (quotient, remainder) = value.div_rem(divisor);
    SignedDivisionResult { quotient, remainder }
}

impl fmt::Debug for RelativeTo {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RelativeTo")
            .field("plain_relative_to", &self.plain_relative_to.is_some())
            .field("zoned_relative_to", &self.zoned_relative_to.is_some())
            .finish()
    }
}

impl Time {
    /// The Time Record of a parsed time that is not START-OF-DAY.
    pub fn from_parsed(time: TimeOrStartOfDay) -> Time {
        match time {
            TimeOrStartOfDay::Time(time) => time,
            TimeOrStartOfDay::StartOfDay => panic!("the parsed time is START-OF-DAY"),
        }
    }
}
