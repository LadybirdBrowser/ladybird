/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/Duration.cpp: Temporal.Duration objects, the duration records, and the rounding
//! of durations relative to a date.

use std::sync::LazyLock;

use libjs_runtime_macros::Trace;
use num_bigint::Sign as BigIntSign;
use num_traits::{FromPrimitive, Signed, ToPrimitive, Zero};

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{RoundingMode, ordinary_create_from_constructor_of, to_integer_if_integral};
use crate::runtime::big_fraction::BigFraction;
use crate::runtime::big_int::SignedBigInteger;
use crate::runtime::big_int_algorithms;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::date::{HOURS_PER_DAY, get_utc_epoch_nanoseconds};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::{AkDouble, ErrorType};
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::temporal::abstract_operations::{
    ArithmeticOperation, Disambiguation, MessageArgument, Overflow, Precision, Sign, Unit, UnitCategory,
    apply_unsigned_rounding_mode, format_fractional_seconds, get_unsigned_rounding_mode, is_calendar_unit,
    larger_of_two_temporal_units, parse_temporal_duration_string, round_big_number_to_increment,
    round_number_to_increment, temporal_unit_category, temporal_unit_length_in_nanoseconds,
};
use crate::runtime::temporal::calendar::{calendar_date_add, calendar_date_until};
use crate::runtime::temporal::instant::{
    MICROSECONDS_PER_MILLISECOND, MILLISECONDS_PER_SECOND, MINUTES_PER_HOUR, NANOSECONDS_PER_DAY, NANOSECONDS_PER_HOUR,
    NANOSECONDS_PER_MICROSECOND, NANOSECONDS_PER_MILLISECOND, NANOSECONDS_PER_MINUTE, NANOSECONDS_PER_SECOND,
    SECONDS_PER_MINUTE,
};
use crate::runtime::temporal::iso_records::{ISODateTime, TimeDuration};
use crate::runtime::temporal::plain_date::{PlainDate, add_days_to_iso_date};
use crate::runtime::temporal::plain_date_time::combine_iso_date_and_time_record;
use crate::runtime::temporal::time_zone::get_epoch_nanoseconds_for;
use crate::utf16::Utf16View;

// 7 Temporal.Duration Objects, https://tc39.es/proposal-temporal/#sec-temporal-duration-objects
#[repr(C)]
#[derive(Trace)]
pub struct Duration {
    base: Object,
    years: f64,        // [[Years]]
    months: f64,       // [[Months]]
    weeks: f64,        // [[Weeks]]
    days: f64,         // [[Days]]
    hours: f64,        // [[Hours]]
    minutes: f64,      // [[Minutes]]
    seconds: f64,      // [[Seconds]]
    milliseconds: f64, // [[Milliseconds]]
    microseconds: f64, // [[Microseconds]]
    nanoseconds: f64,  // [[Nanoseconds]]
}

define_cell!(Duration, Object, extends: [Object]);

impl core::ops::Deref for Duration {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

/// The ten fields of a duration, in the order Temporal lists them.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DurationFields {
    pub years: f64,
    pub months: f64,
    pub weeks: f64,
    pub days: f64,
    pub hours: f64,
    pub minutes: f64,
    pub seconds: f64,
    pub milliseconds: f64,
    pub microseconds: f64,
    pub nanoseconds: f64,
}

impl DurationFields {
    fn values(&self) -> [f64; 10] {
        [
            self.years,
            self.months,
            self.weeks,
            self.days,
            self.hours,
            self.minutes,
            self.seconds,
            self.milliseconds,
            self.microseconds,
            self.nanoseconds,
        ]
    }

    fn map(self, function: impl Fn(f64) -> f64) -> DurationFields {
        DurationFields {
            years: function(self.years),
            months: function(self.months),
            weeks: function(self.weeks),
            days: function(self.days),
            hours: function(self.hours),
            minutes: function(self.minutes),
            seconds: function(self.seconds),
            milliseconds: function(self.milliseconds),
            microseconds: function(self.microseconds),
            nanoseconds: function(self.nanoseconds),
        }
    }
}

/// The NOTE of the C++ constructor: the fields are finite and integral, and negative zero is normalized.
fn normalize_field(value: f64) -> f64 {
    assert!(value.is_finite());
    // FIXME: test-js contains a small number of cases where a Temporal.Duration is constructed with a non-integral
    //        double. Eliminate these and VERIFY(trunc(value) == value) instead.
    if value.trunc() != value {
        return value.trunc();
    }
    if value == 0.0 {
        return 0.0;
    }
    value
}

impl Duration {
    fn new(vm: &Vm, fields: DurationFields, prototype: Gc<Object>) -> Duration {
        // NOTE: The spec stores these fields as mathematical values. VERIFY() that we have finite, integral values in them,
        //       and normalize any negative zeros caused by floating point math. This is usually done using ℝ(𝔽(value)) at
        //       the call site.
        let fields = fields.map(normalize_field);
        Duration {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            years: fields.years,
            months: fields.months,
            weeks: fields.weeks,
            days: fields.days,
            hours: fields.hours,
            minutes: fields.minutes,
            seconds: fields.seconds,
            milliseconds: fields.milliseconds,
            microseconds: fields.microseconds,
            nanoseconds: fields.nanoseconds,
        }
    }

    pub fn years(&self) -> f64 {
        self.years
    }

    pub fn months(&self) -> f64 {
        self.months
    }

    pub fn weeks(&self) -> f64 {
        self.weeks
    }

    pub fn days(&self) -> f64 {
        self.days
    }

    pub fn hours(&self) -> f64 {
        self.hours
    }

    pub fn minutes(&self) -> f64 {
        self.minutes
    }

    pub fn seconds(&self) -> f64 {
        self.seconds
    }

    pub fn milliseconds(&self) -> f64 {
        self.milliseconds
    }

    pub fn microseconds(&self) -> f64 {
        self.microseconds
    }

    pub fn nanoseconds(&self) -> f64 {
        self.nanoseconds
    }

    pub fn fields(&self) -> DurationFields {
        DurationFields {
            years: self.years,
            months: self.months,
            weeks: self.weeks,
            days: self.days,
            hours: self.hours,
            minutes: self.minutes,
            seconds: self.seconds,
            milliseconds: self.milliseconds,
            microseconds: self.microseconds,
            nanoseconds: self.nanoseconds,
        }
    }
}

// 7.5.1 Date Duration Records, https://tc39.es/proposal-temporal/#sec-temporal-date-duration-records
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct DateDuration {
    pub years: f64,
    pub months: f64,
    pub weeks: f64,
    pub days: f64,
}

// 7.5.2 Partial Duration Records, https://tc39.es/proposal-temporal/#sec-temporal-partial-duration-records
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PartialDuration {
    pub years: Option<f64>,
    pub months: Option<f64>,
    pub weeks: Option<f64>,
    pub days: Option<f64>,
    pub hours: Option<f64>,
    pub minutes: Option<f64>,
    pub seconds: Option<f64>,
    pub milliseconds: Option<f64>,
    pub microseconds: Option<f64>,
    pub nanoseconds: Option<f64>,
}

impl PartialDuration {
    pub fn zero() -> PartialDuration {
        PartialDuration {
            years: Some(0.0),
            months: Some(0.0),
            weeks: Some(0.0),
            days: Some(0.0),
            hours: Some(0.0),
            minutes: Some(0.0),
            seconds: Some(0.0),
            milliseconds: Some(0.0),
            microseconds: Some(0.0),
            nanoseconds: Some(0.0),
        }
    }

    pub fn any_field_defined(&self) -> bool {
        self.years.is_some()
            || self.months.is_some()
            || self.weeks.is_some()
            || self.days.is_some()
            || self.hours.is_some()
            || self.minutes.is_some()
            || self.seconds.is_some()
            || self.milliseconds.is_some()
            || self.microseconds.is_some()
            || self.nanoseconds.is_some()
    }
}

// maxTimeDuration = 2**53 × 10**9 - 1 = 9,007,199,254,740,991,999,999,999
pub static MAX_TIME_DURATION: LazyLock<TimeDuration> =
    LazyLock::new(|| "9007199254740991999999999".parse().expect("a valid integer"));

// 7.5.3 Internal Duration Records, https://tc39.es/proposal-temporal/#sec-temporal-internal-duration-records
#[derive(Clone, Debug, Default, PartialEq)]
pub struct InternalDuration {
    pub date: DateDuration,
    pub time: TimeDuration,
}

// 7.5.32 Duration Nudge Result Records, https://tc39.es/proposal-temporal/#sec-temporal-duration-nudge-result-records
#[derive(Clone, Debug, Default)]
pub struct DurationNudgeResult {
    pub duration: InternalDuration,
    pub nudged_epoch_ns: SignedBigInteger,
    pub did_expand_calendar_unit: bool,
}

#[derive(Clone, Debug, Default)]
pub struct NudgeWindow {
    pub r1: f64,
    pub r2: f64,
    pub start_epoch_ns: SignedBigInteger,
    pub end_epoch_ns: SignedBigInteger,
    pub start_duration: InternalDuration,
    pub end_duration: InternalDuration,
}

#[derive(Clone, Debug)]
pub struct CalendarNudgeResult {
    pub nudge_result: DurationNudgeResult,
    pub total: BigFraction,
}

/// The magnitude of a time duration, which is what C++ compares through unsigned_value().
fn magnitude_exceeds_max_time_duration(value: &TimeDuration) -> bool {
    value.magnitude() > MAX_TIME_DURATION.magnitude()
}

/// SignedBigInteger(double), which truncates the double towards zero.
pub fn big_integer_from_double(value: f64) -> SignedBigInteger {
    SignedBigInteger::from_f64(value).expect("the double is finite")
}

// 7.5.4 ZeroDateDuration ( ), https://tc39.es/proposal-temporal/#sec-temporal-zerodateduration
pub fn zero_date_duration(vm: &Vm) -> DateDuration {
    // 1. Return ! CreateDateDurationRecord(0, 0, 0, 0).
    create_date_duration_record(vm, 0.0, 0.0, 0.0, 0.0).must()
}

// 7.5.5 ToInternalDurationRecord ( duration ), https://tc39.es/proposal-temporal/#sec-temporal-tointernaldurationrecord
pub fn to_internal_duration_record(vm: &Vm, duration: &Duration) -> InternalDuration {
    // 1. Let dateDuration be ! CreateDateDurationRecord(duration.[[Years]], duration.[[Months]], duration.[[Weeks]], duration.[[Days]]).
    let date_duration = create_date_duration_record(
        vm,
        duration.years(),
        duration.months(),
        duration.weeks(),
        duration.days(),
    )
    .must();

    // 2. Let timeDuration be TimeDurationFromComponents(duration.[[Hours]], duration.[[Minutes]], duration.[[Seconds]], duration.[[Milliseconds]], duration.[[Microseconds]], duration.[[Nanoseconds]]).
    let time_duration = time_duration_from_components(
        duration.hours(),
        duration.minutes(),
        duration.seconds(),
        duration.milliseconds(),
        duration.microseconds(),
        duration.nanoseconds(),
    );

    // 3. Return CombineDateAndTimeDuration(dateDuration, timeDuration).
    combine_date_and_time_duration(date_duration, time_duration)
}

// 7.5.6 ToInternalDurationRecordWith24HourDays ( duration ), https://tc39.es/proposal-temporal/#sec-temporal-tointernaldurationrecordwith24hourdays
pub fn to_internal_duration_record_with_24_hour_days(vm: &Vm, duration: &Duration) -> InternalDuration {
    // 1. Let timeDuration be TimeDurationFromComponents(duration.[[Hours]], duration.[[Minutes]], duration.[[Seconds]], duration.[[Milliseconds]], duration.[[Microseconds]], duration.[[Nanoseconds]]).
    let time_duration = time_duration_from_components(
        duration.hours(),
        duration.minutes(),
        duration.seconds(),
        duration.milliseconds(),
        duration.microseconds(),
        duration.nanoseconds(),
    );

    // 2. Set timeDuration to ! Add24HourDaysToTimeDuration(timeDuration, duration.[[Days]]).
    let time_duration = add_24_hour_days_to_time_duration(vm, &time_duration, duration.days()).must();

    // 3. Let dateDuration be ! CreateDateDurationRecord(duration.[[Years]], duration.[[Months]], duration.[[Weeks]], 0).
    let date_duration =
        create_date_duration_record(vm, duration.years(), duration.months(), duration.weeks(), 0.0).must();

    // 4. Return CombineDateAndTimeDuration(dateDuration, timeDuration).
    combine_date_and_time_duration(date_duration, time_duration)
}

// 7.5.7 ToDateDurationRecordWithoutTime ( duration ), https://tc39.es/proposal-temporal/#sec-temporal-todatedurationrecordwithouttime
pub fn to_date_duration_record_without_time(vm: &Vm, duration: &Duration) -> DateDuration {
    // 1. Let internalDuration be ToInternalDurationRecordWith24HourDays(duration).
    let internal_duration = to_internal_duration_record_with_24_hour_days(vm, duration);

    // 2. Let days be truncate(internalDuration.[[Time]] / nsPerDay).
    let days = &internal_duration.time / &*NANOSECONDS_PER_DAY;

    // 3. Return ! CreateDateDurationRecord(internalDuration.[[Date]].[[Years]], internalDuration.[[Date]].[[Months]], internalDuration.[[Date]].[[Weeks]], days).
    create_date_duration_record(
        vm,
        duration.years(),
        duration.months(),
        duration.weeks(),
        big_int_algorithms::to_double(&days),
    )
    .must()
}

/// Crypto::UnsignedBigInteger::to_double() of a non-negative BigInt.
fn unsigned_to_double(value: &SignedBigInteger) -> f64 {
    big_int_algorithms::unsigned_to_double(value.magnitude())
}

// 7.5.8 TemporalDurationFromInternal ( internalDuration, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-temporaldurationfrominternal
pub fn temporal_duration_from_internal(
    vm: &Vm,
    internal_duration: &InternalDuration,
    largest_unit: Unit,
) -> ThrowCompletionOr<Gc<Duration>> {
    // 1. Let days, hours, minutes, seconds, milliseconds, and microseconds be 0.
    let mut days = 0.0;
    let mut hours = 0.0;
    let mut minutes = 0.0;
    let mut seconds = 0.0;
    let mut milliseconds = 0.0;
    let mut microseconds = 0.0;

    // 2. Let sign be TimeDurationSign(internalDuration.[[Time]]).
    let sign = f64::from(time_duration_sign(&internal_duration.time));

    // 3. Let nanoseconds be abs(internalDuration.[[Time]]).
    let absolute_nanoseconds = internal_duration.time.abs();
    let nanoseconds;

    let divide = |dividend: &SignedBigInteger, divisor: &SignedBigInteger| (dividend / divisor, dividend % divisor);

    // 4. If TemporalUnitCategory(largestUnit) is date, then
    if temporal_unit_category(largest_unit) == UnitCategory::Date {
        // a. Set microseconds to floor(nanoseconds / 1000).
        let (microseconds_value, nanoseconds_remainder) = divide(&absolute_nanoseconds, &NANOSECONDS_PER_MICROSECOND);

        // b. Set nanoseconds to nanoseconds modulo 1000.
        nanoseconds = unsigned_to_double(&nanoseconds_remainder);

        // c. Set milliseconds to floor(microseconds / 1000).
        let (milliseconds_value, microseconds_remainder) = divide(&microseconds_value, &MICROSECONDS_PER_MILLISECOND);

        // d. Set microseconds to microseconds modulo 1000.
        microseconds = unsigned_to_double(&microseconds_remainder);

        // e. Set seconds to floor(milliseconds / 1000).
        let (seconds_value, milliseconds_remainder) = divide(&milliseconds_value, &MILLISECONDS_PER_SECOND);

        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = unsigned_to_double(&milliseconds_remainder);

        // g. Set minutes to floor(seconds / 60).
        let (minutes_value, seconds_remainder) = divide(&seconds_value, &SECONDS_PER_MINUTE);

        // h. Set seconds to seconds modulo 60.
        seconds = unsigned_to_double(&seconds_remainder);

        // i. Set hours to floor(minutes / 60).
        let (hours_value, minutes_remainder) = divide(&minutes_value, &MINUTES_PER_HOUR);

        // j. Set minutes to minutes modulo 60.
        minutes = unsigned_to_double(&minutes_remainder);

        // k. Set days to floor(hours / 24).
        let (days_value, hours_remainder) = divide(&hours_value, &crate::runtime::temporal::instant::HOURS_PER_DAY);
        days = unsigned_to_double(&days_value);

        // l. Set hours to hours modulo 24.
        hours = unsigned_to_double(&hours_remainder);
    }
    // 5. Else if largestUnit is hour, then
    else if largest_unit == Unit::Hour {
        // a. Set microseconds to floor(nanoseconds / 1000).
        let (microseconds_value, nanoseconds_remainder) = divide(&absolute_nanoseconds, &NANOSECONDS_PER_MICROSECOND);

        // b. Set nanoseconds to nanoseconds modulo 1000.
        nanoseconds = unsigned_to_double(&nanoseconds_remainder);

        // c. Set milliseconds to floor(microseconds / 1000).
        let (milliseconds_value, microseconds_remainder) = divide(&microseconds_value, &MICROSECONDS_PER_MILLISECOND);

        // d. Set microseconds to microseconds modulo 1000.
        microseconds = unsigned_to_double(&microseconds_remainder);

        // e. Set seconds to floor(milliseconds / 1000).
        let (seconds_value, milliseconds_remainder) = divide(&milliseconds_value, &MILLISECONDS_PER_SECOND);

        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = unsigned_to_double(&milliseconds_remainder);

        // g. Set minutes to floor(seconds / 60).
        let (minutes_value, seconds_remainder) = divide(&seconds_value, &SECONDS_PER_MINUTE);

        // h. Set seconds to seconds modulo 60.
        seconds = unsigned_to_double(&seconds_remainder);

        // i. Set hours to floor(minutes / 60).
        let (hours_value, minutes_remainder) = divide(&minutes_value, &MINUTES_PER_HOUR);
        hours = unsigned_to_double(&hours_value);

        // j. Set minutes to minutes modulo 60.
        minutes = unsigned_to_double(&minutes_remainder);
    }
    // 6. Else if largestUnit is minute, then
    else if largest_unit == Unit::Minute {
        // a. Set microseconds to floor(nanoseconds / 1000).
        let (microseconds_value, nanoseconds_remainder) = divide(&absolute_nanoseconds, &NANOSECONDS_PER_MICROSECOND);

        // b. Set nanoseconds to nanoseconds modulo 1000.
        nanoseconds = unsigned_to_double(&nanoseconds_remainder);

        // c. Set milliseconds to floor(microseconds / 1000).
        let (milliseconds_value, microseconds_remainder) = divide(&microseconds_value, &MICROSECONDS_PER_MILLISECOND);

        // d. Set microseconds to microseconds modulo 1000.
        microseconds = unsigned_to_double(&microseconds_remainder);

        // e. Set seconds to floor(milliseconds / 1000).
        let (seconds_value, milliseconds_remainder) = divide(&milliseconds_value, &MILLISECONDS_PER_SECOND);

        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = unsigned_to_double(&milliseconds_remainder);

        // g. Set minutes to floor(seconds / 60).
        let (minutes_value, seconds_remainder) = divide(&seconds_value, &SECONDS_PER_MINUTE);
        minutes = unsigned_to_double(&minutes_value);

        // h. Set seconds to seconds modulo 60.
        seconds = unsigned_to_double(&seconds_remainder);
    }
    // 7. Else if largestUnit is second, then
    else if largest_unit == Unit::Second {
        // a. Set microseconds to floor(nanoseconds / 1000).
        let (microseconds_value, nanoseconds_remainder) = divide(&absolute_nanoseconds, &NANOSECONDS_PER_MICROSECOND);

        // b. Set nanoseconds to nanoseconds modulo 1000.
        nanoseconds = unsigned_to_double(&nanoseconds_remainder);

        // c. Set milliseconds to floor(microseconds / 1000).
        let (milliseconds_value, microseconds_remainder) = divide(&microseconds_value, &MICROSECONDS_PER_MILLISECOND);

        // d. Set microseconds to microseconds modulo 1000.
        microseconds = unsigned_to_double(&microseconds_remainder);

        // e. Set seconds to floor(milliseconds / 1000).
        let (seconds_value, milliseconds_remainder) = divide(&milliseconds_value, &MILLISECONDS_PER_SECOND);
        seconds = unsigned_to_double(&seconds_value);

        // f. Set milliseconds to milliseconds modulo 1000.
        milliseconds = unsigned_to_double(&milliseconds_remainder);
    }
    // 8. Else if largestUnit is millisecond, then
    else if largest_unit == Unit::Millisecond {
        // a. Set microseconds to floor(nanoseconds / 1000).
        let (microseconds_value, nanoseconds_remainder) = divide(&absolute_nanoseconds, &NANOSECONDS_PER_MICROSECOND);

        // b. Set nanoseconds to nanoseconds modulo 1000.
        nanoseconds = unsigned_to_double(&nanoseconds_remainder);

        // c. Set milliseconds to floor(microseconds / 1000).
        let (milliseconds_value, microseconds_remainder) = divide(&microseconds_value, &MICROSECONDS_PER_MILLISECOND);
        milliseconds = unsigned_to_double(&milliseconds_value);

        // d. Set microseconds to microseconds modulo 1000.
        microseconds = unsigned_to_double(&microseconds_remainder);
    }
    // 9. Else if largestUnit is microsecond, then
    else if largest_unit == Unit::Microsecond {
        // a. Set microseconds to floor(nanoseconds / 1000).
        let (microseconds_value, nanoseconds_remainder) = divide(&absolute_nanoseconds, &NANOSECONDS_PER_MICROSECOND);
        microseconds = unsigned_to_double(&microseconds_value);

        // b. Set nanoseconds to nanoseconds modulo 1000.
        nanoseconds = unsigned_to_double(&nanoseconds_remainder);
    }
    // 10. Else,
    else {
        // a. Assert: largestUnit is nanosecond.
        assert!(largest_unit == Unit::Nanosecond);
        nanoseconds = unsigned_to_double(&absolute_nanoseconds);
    }

    // 11. NOTE: When largestUnit is millisecond, microsecond, or nanosecond, milliseconds, microseconds, or nanoseconds
    //     may be an unsafe integer. In this case, care must be taken when implementing the calculation using floating
    //     point arithmetic. It can be implemented in C++ using std::fma(). String manipulation will also give an exact
    //     result, since the multiplication is by a power of 10.

    // 12. Return ? CreateTemporalDuration(internalDuration.[[Date]].[[Years]], internalDuration.[[Date]].[[Months]], internalDuration.[[Date]].[[Weeks]], internalDuration.[[Date]].[[Days]] + days × sign, hours × sign, minutes × sign, seconds × sign, milliseconds × sign, microseconds × sign, nanoseconds × sign).
    create_temporal_duration(
        vm,
        DurationFields {
            years: internal_duration.date.years,
            months: internal_duration.date.months,
            weeks: internal_duration.date.weeks,
            days: internal_duration.date.days + (days * sign),
            hours: hours * sign,
            minutes: minutes * sign,
            seconds: seconds * sign,
            milliseconds: milliseconds * sign,
            microseconds: microseconds * sign,
            nanoseconds: nanoseconds * sign,
        },
        None,
    )
}

// 7.5.9 CreateDateDurationRecord ( years, months, weeks, days ), https://tc39.es/proposal-temporal/#sec-temporal-createdatedurationrecord
pub fn create_date_duration_record(
    vm: &Vm,
    years: f64,
    months: f64,
    weeks: f64,
    days: f64,
) -> ThrowCompletionOr<DateDuration> {
    // 1. If IsValidDuration(years, months, weeks, days, 0, 0, 0, 0, 0, 0) is false, throw a RangeError exception.
    if !is_valid_duration(&DurationFields {
        years,
        months,
        weeks,
        days,
        ..DurationFields::default()
    }) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidDuration, &[]);
    }

    // 2. Return Date Duration Record { [[Years]]: ℝ(𝔽(years)), [[Months]]: ℝ(𝔽(months)), [[Weeks]]: ℝ(𝔽(weeks)), [[Days]]: ℝ(𝔽(days))  }.
    Ok(DateDuration {
        years,
        months,
        weeks,
        days,
    })
}

// 7.5.10 AdjustDateDurationRecord ( dateDuration, days [ , weeks [ , months ] ] ), https://tc39.es/proposal-temporal/#sec-temporal-adjustdatedurationrecord
pub fn adjust_date_duration_record(
    vm: &Vm,
    date_duration: &DateDuration,
    days: f64,
    weeks: Option<f64>,
    months: Option<f64>,
) -> ThrowCompletionOr<DateDuration> {
    // 1. If weeks is not present, set weeks to dateDuration.[[Weeks]].
    let weeks = weeks.unwrap_or(date_duration.weeks);

    // 2. If months is not present, set months to dateDuration.[[Months]].
    let months = months.unwrap_or(date_duration.months);

    // 3. Return ? CreateDateDurationRecord(dateDuration.[[Years]], months, weeks, days).
    create_date_duration_record(vm, date_duration.years, months, weeks, days)
}

// 7.5.11 CombineDateAndTimeDuration ( dateDuration, timeDuration ), https://tc39.es/proposal-temporal/#sec-temporal-combinedateandtimeduration
pub fn combine_date_and_time_duration(date_duration: DateDuration, time_duration: TimeDuration) -> InternalDuration {
    // 1. Let dateSign be DateDurationSign(dateDuration).
    let date_sign = date_duration_sign(&date_duration);

    // 2. Let timeSign be TimeDurationSign(timeDuration).
    let time_sign = time_duration_sign(&time_duration);

    // 3. Assert: If dateSign ≠ 0 and timeSign ≠ 0, dateSign = timeSign.
    if date_sign != 0 && time_sign != 0 {
        assert!(date_sign == time_sign);
    }

    // 4. Return Internal Duration Record { [[Date]]: dateDuration, [[Time]]: timeDuration  }.
    InternalDuration {
        date: date_duration,
        time: time_duration,
    }
}

// 7.5.12 ToTemporalDuration ( item ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalduration
pub fn to_temporal_duration(vm: &Vm, item: Value) -> ThrowCompletionOr<Gc<Duration>> {
    // 1. If item is an Object and item has an [[InitializedTemporalDuration]] internal slot, then
    if item.is_object()
        && let Some(duration) = item.as_object().downcast::<Duration>()
    {
        // a. Return ! CreateTemporalDuration(item.[[Years]], item.[[Months]], item.[[Weeks]], item.[[Days]], item.[[Hours]], item.[[Minutes]], item.[[Seconds]], item.[[Milliseconds]], item.[[Microseconds]], item.[[Nanoseconds]]).
        return Ok(create_temporal_duration(vm, duration.fields(), None).must());
    }

    // 2. If item is not an Object, then
    if !item.is_object() {
        // a. If item is not a String, throw a TypeError exception.
        if !item.is_string() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAString, &[&item]);
        }

        // b. Return ? ParseTemporalDurationString(item).
        let item = item.as_string().utf16_string();
        return parse_temporal_duration_string(vm, Utf16View::of_string(&item));
    }

    // 3. Let result be a new Partial Duration Record with each field set to 0.
    let mut result = PartialDuration::zero();

    // 4. Let partial be ? ToTemporalPartialDurationRecord(item).
    let partial = to_temporal_partial_duration_record(vm, item)?;

    // 5. If partial.[[Years]] is not undefined, set result.[[Years]] to partial.[[Years]].
    if partial.years.is_some() {
        result.years = partial.years;
    }

    // 6. If partial.[[Months]] is not undefined, set result.[[Months]] to partial.[[Months]].
    if partial.months.is_some() {
        result.months = partial.months;
    }

    // 7. If partial.[[Weeks]] is not undefined, set result.[[Weeks]] to partial.[[Weeks]].
    if partial.weeks.is_some() {
        result.weeks = partial.weeks;
    }

    // 8. If partial.[[Days]] is not undefined, set result.[[Days]] to partial.[[Days]].
    if partial.days.is_some() {
        result.days = partial.days;
    }

    // 9. If partial.[[Hours]] is not undefined, set result.[[Hours]] to partial.[[Hours]].
    if partial.hours.is_some() {
        result.hours = partial.hours;
    }

    // 10. If partial.[[Minutes]] is not undefined, set result.[[Minutes]] to partial.[[Minutes]].
    if partial.minutes.is_some() {
        result.minutes = partial.minutes;
    }

    // 11. If partial.[[Seconds]] is not undefined, set result.[[Seconds]] to partial.[[Seconds]].
    if partial.seconds.is_some() {
        result.seconds = partial.seconds;
    }

    // 12. If partial.[[Milliseconds]] is not undefined, set result.[[Milliseconds]] to partial.[[Milliseconds]].
    if partial.milliseconds.is_some() {
        result.milliseconds = partial.milliseconds;
    }

    // 13. If partial.[[Microseconds]] is not undefined, set result.[[Microseconds]] to partial.[[Microseconds]].
    if partial.microseconds.is_some() {
        result.microseconds = partial.microseconds;
    }

    // 14. If partial.[[Nanoseconds]] is not undefined, set result.[[Nanoseconds]] to partial.[[Nanoseconds]].
    if partial.nanoseconds.is_some() {
        result.nanoseconds = partial.nanoseconds;
    }

    // 15. Return ? CreateTemporalDuration(result.[[Years]], result.[[Months]], result.[[Weeks]], result.[[Days]], result.[[Hours]], result.[[Minutes]], result.[[Seconds]], result.[[Milliseconds]], result.[[Microseconds]], result.[[Nanoseconds]]).
    let field = |value: Option<f64>| value.expect("every field of the result is set");
    create_temporal_duration(
        vm,
        DurationFields {
            years: field(result.years),
            months: field(result.months),
            weeks: field(result.weeks),
            days: field(result.days),
            hours: field(result.hours),
            minutes: field(result.minutes),
            seconds: field(result.seconds),
            milliseconds: field(result.milliseconds),
            microseconds: field(result.microseconds),
            nanoseconds: field(result.nanoseconds),
        },
        None,
    )
}

fn sign_of_values(values: &[f64]) -> i8 {
    // 1. For each value v of « ... », do
    for &value in values {
        // a. If v < 0, return -1.
        if value < 0.0 {
            return -1;
        }

        // b. If v > 0, return 1.
        if value > 0.0 {
            return 1;
        }
    }

    // 2. Return 0.
    0
}

// 7.5.13 DurationSign ( duration ), https://tc39.es/proposal-temporal/#sec-durationsign
pub fn duration_sign(duration: &Duration) -> i8 {
    // 1. For each value v of « duration.[[Years]], duration.[[Months]], duration.[[Weeks]], duration.[[Days]], duration.[[Hours]], duration.[[Minutes]], duration.[[Seconds]], duration.[[Milliseconds]], duration.[[Microseconds]], duration.[[Nanoseconds]] », do
    //     a. If v < 0, return -1.
    //     b. If v > 0, return 1.
    // 2. Return 0.
    sign_of_values(&duration.fields().values())
}

// 7.5.14 DateDurationSign ( dateDuration ), https://tc39.es/proposal-temporal/#sec-temporal-datedurationsign
pub fn date_duration_sign(date_duration: &DateDuration) -> i8 {
    // 1. For each value v of « dateDuration.[[Years]], dateDuration.[[Months]], dateDuration.[[Weeks]], dateDuration.[[Days]] », do
    //     a. If v < 0, return -1.
    //     b. If v > 0, return 1.
    // 2. Return 0.
    sign_of_values(&[
        date_duration.years,
        date_duration.months,
        date_duration.weeks,
        date_duration.days,
    ])
}

// 7.5.15 InternalDurationSign ( internalDuration ), https://tc39.es/proposal-temporal/#sec-temporal-internaldurationsign
pub fn internal_duration_sign(internal_duration: &InternalDuration) -> i8 {
    // 1. Let dateSign be DateDurationSign(internalDuration.[[Date]]).
    let date_sign = date_duration_sign(&internal_duration.date);

    // 2. If dateSign ≠ 0, return dateSign.
    if date_sign != 0 {
        return date_sign;
    }

    // 3. Return TimeDurationSign(internalDuration.[[Time]]).
    time_duration_sign(&internal_duration.time)
}

// 7.5.16 IsValidDuration ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds ), https://tc39.es/proposal-temporal/#sec-isvalidduration
pub fn is_valid_duration(fields: &DurationFields) -> bool {
    // 1. Let sign be 0.
    let mut sign = 0;

    // 2. For each value v of « years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds », do
    for value in fields.values() {
        // a. Assert: 𝔽(v) is finite.
        assert!(value.is_finite());

        // b. If v < 0, then
        if value < 0.0 {
            // i. If sign > 0, return false.
            if sign > 0 {
                return false;
            }

            // ii. Set sign to -1.
            sign = -1;
        }
        // c. Else if v > 0, then
        else if value > 0.0 {
            // i. If sign < 0, return false.
            if sign < 0 {
                return false;
            }

            // ii. Set sign to 1.
            sign = 1;
        }
    }

    // 3. If abs(years) ≥ 2**32, return false.
    if fields.years.abs() > f64::from(u32::MAX) {
        return false;
    }

    // 4. If abs(months) ≥ 2**32, return false.
    if fields.months.abs() > f64::from(u32::MAX) {
        return false;
    }

    // 5. If abs(weeks) ≥ 2**32, return false.
    if fields.weeks.abs() > f64::from(u32::MAX) {
        return false;
    }

    // 6. Let normalizedNanoseconds be days × 86,400 × 10**9 + hours × 3600 × 10**9 + minutes × 60 × 10**9 + seconds × 10**9 + ℝ(𝔽(milliseconds)) × 10**6 + ℝ(𝔽(microseconds)) × 10**3 + ℝ(𝔽(nanoseconds)).
    // 7. NOTE: The above step cannot be implemented directly using 64-bit floating-point arithmetic. Multiplying by
    //          10**-3, 10**-6, and 10**-9 respectively may be imprecise when milliseconds, microseconds, or nanoseconds
    //          is an unsafe integer. The step can be implemented by using 128-bit integers and performing all arithmetic
    //          on nanosecond values. It could also be implemented in C++ with an implementation of std::remquo() with
    //          sufficient bits in the quotient. String manipulation will also give an exact result, since the
    //          multiplication is by a power of 10.
    let normalized_nanoseconds = big_integer_from_double(fields.days) * &*NANOSECONDS_PER_DAY
        + big_integer_from_double(fields.hours) * &*NANOSECONDS_PER_HOUR
        + big_integer_from_double(fields.minutes) * &*NANOSECONDS_PER_MINUTE
        + big_integer_from_double(fields.seconds) * &*NANOSECONDS_PER_SECOND
        + big_integer_from_double(fields.milliseconds) * &*NANOSECONDS_PER_MILLISECOND
        + big_integer_from_double(fields.microseconds) * &*NANOSECONDS_PER_MICROSECOND
        + big_integer_from_double(fields.nanoseconds);

    // 8. If abs(normalizedNanoseconds) ≥ 10**9 × 2**53, return false.
    if magnitude_exceeds_max_time_duration(&normalized_nanoseconds) {
        return false;
    }

    // 9. Return true.
    true
}

// 7.5.17 DefaultTemporalLargestUnit ( duration ), https://tc39.es/proposal-temporal/#sec-temporal-defaulttemporallargestunit
pub fn default_temporal_largest_unit(duration: &Duration) -> Unit {
    // 1. If duration.[[Years]] ≠ 0, return YEAR.
    if duration.years() != 0.0 {
        return Unit::Year;
    }

    // 2. If duration.[[Months]] ≠ 0, return MONTH.
    if duration.months() != 0.0 {
        return Unit::Month;
    }

    // 3. If duration.[[Weeks]] ≠ 0, return WEEK.
    if duration.weeks() != 0.0 {
        return Unit::Week;
    }

    // 4. If duration.[[Days]] ≠ 0, return DAY.
    if duration.days() != 0.0 {
        return Unit::Day;
    }

    // 5. If duration.[[Hours]] ≠ 0, return HOUR.
    if duration.hours() != 0.0 {
        return Unit::Hour;
    }

    // 6. If duration.[[Minutes]] ≠ 0, return MINUTE.
    if duration.minutes() != 0.0 {
        return Unit::Minute;
    }

    // 7. If duration.[[Seconds]] ≠ 0, return SECOND.
    if duration.seconds() != 0.0 {
        return Unit::Second;
    }

    // 8. If duration.[[Milliseconds]] ≠ 0, return MILLISECOND.
    if duration.milliseconds() != 0.0 {
        return Unit::Millisecond;
    }

    // 9. If duration.[[Microseconds]] ≠ 0, return MICROSECOND.
    if duration.microseconds() != 0.0 {
        return Unit::Microsecond;
    }

    // 10. Return NANOSECOND.
    Unit::Nanosecond
}

// 7.5.18 ToTemporalPartialDurationRecord ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalpartialdurationrecord
pub fn to_temporal_partial_duration_record(
    vm: &Vm,
    temporal_duration_like: Value,
) -> ThrowCompletionOr<PartialDuration> {
    // 1. If temporalDurationLike is not an Object, throw a TypeError exception.
    if !temporal_duration_like.is_object() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObject, &[&temporal_duration_like]);
    }

    // 2. Let result be a new partial Duration Record with each field set to undefined.
    let mut result = PartialDuration::default();

    // 3. NOTE: The following steps read properties and perform independent validation in alphabetical order.

    let temporal_duration = temporal_duration_like.as_object();
    let to_integral_if_defined =
        |property: &crate::runtime::property_key::PropertyKey| -> ThrowCompletionOr<Option<f64>> {
            let value = temporal_duration.get(vm, property)?;
            if value.is_undefined() {
                return Ok(None);
            }
            Ok(Some(to_integer_if_integral(
                vm,
                value,
                ErrorType::TemporalInvalidDurationPropertyValueNonIntegral,
                &[MessageArgument::PropertyKey(property), MessageArgument::Value(value)],
            )?))
        };
    let names = &vm.names;

    // 4. Let days be ? Get(temporalDurationLike, "days").
    // 5. If days is not undefined, set result.[[Days]] to ? ToIntegerIfIntegral(days).
    result.days = to_integral_if_defined(&names.days)?;

    // 6. Let hours be ? Get(temporalDurationLike, "hours").
    // 7. If hours is not undefined, set result.[[Hours]] to ? ToIntegerIfIntegral(hours).
    result.hours = to_integral_if_defined(&names.hours)?;

    // 8. Let microseconds be ? Get(temporalDurationLike, "microseconds").
    // 9. If microseconds is not undefined, set result.[[Microseconds]] to ? ToIntegerIfIntegral(microseconds).
    result.microseconds = to_integral_if_defined(&names.microseconds)?;

    // 10. Let milliseconds be ? Get(temporalDurationLike, "milliseconds").
    // 11. If milliseconds is not undefined, set result.[[Milliseconds]] to ? ToIntegerIfIntegral(milliseconds).
    result.milliseconds = to_integral_if_defined(&names.milliseconds)?;

    // 12. Let minutes be ? Get(temporalDurationLike, "minutes").
    // 13. If minutes is not undefined, set result.[[Minutes]] to ? ToIntegerIfIntegral(minutes).
    result.minutes = to_integral_if_defined(&names.minutes)?;

    // 14. Let months be ? Get(temporalDurationLike, "months").
    // 15. If months is not undefined, set result.[[Months]] to ? ToIntegerIfIntegral(months).
    result.months = to_integral_if_defined(&names.months)?;

    // 16. Let nanoseconds be ? Get(temporalDurationLike, "nanoseconds").
    // 17. If nanoseconds is not undefined, set result.[[Nanoseconds]] to ? ToIntegerIfIntegral(nanoseconds).
    result.nanoseconds = to_integral_if_defined(&names.nanoseconds)?;

    // 18. Let seconds be ? Get(temporalDurationLike, "seconds").
    // 19. If seconds is not undefined, set result.[[Seconds]] to ? ToIntegerIfIntegral(seconds).
    result.seconds = to_integral_if_defined(&names.seconds)?;

    // 20. Let weeks be ? Get(temporalDurationLike, "weeks").
    // 21. If weeks is not undefined, set result.[[Weeks]] to ? ToIntegerIfIntegral(weeks).
    result.weeks = to_integral_if_defined(&names.weeks)?;

    // 22. Let years be ? Get(temporalDurationLike, "years").
    // 23. If years is not undefined, set result.[[Years]] to ? ToIntegerIfIntegral(years).
    result.years = to_integral_if_defined(&names.years)?;

    // 24. If years is undefined, and months is undefined, and weeks is undefined, and days is undefined, and hours is
    //     undefined, and minutes is undefined, and seconds is undefined, and milliseconds is undefined, and microseconds
    //     is undefined, and nanoseconds is undefined, throw a TypeError exception.
    if !result.any_field_defined() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::TemporalInvalidDurationLikeObject, &[]);
    }

    // 25. Return result.
    Ok(result)
}

// 7.5.19 CreateTemporalDuration ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporalduration
pub fn create_temporal_duration(
    vm: &Vm,
    fields: DurationFields,
    new_target: Option<Gc<FunctionObject>>,
) -> ThrowCompletionOr<Gc<Duration>> {
    let realm = vm.current_realm().expect("CreateTemporalDuration runs in a realm");

    // 1. If IsValidDuration(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds) is false, throw a RangeError exception.
    if !is_valid_duration(&fields) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidDuration, &[]);
    }

    // 2. If newTarget is not present, set newTarget to %Temporal.Duration%.
    let new_target = new_target.unwrap_or_else(|| realm.intrinsics().temporal_duration_constructor(vm));

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.Duration.prototype%", « [[InitializedTemporalDuration]], [[Years]], [[Months]], [[Weeks]], [[Days]], [[Hours]], [[Minutes]], [[Seconds]], [[Milliseconds]], [[Microseconds]], [[Nanoseconds]] »).
    // 4. Set object.[[Years]] to ℝ(𝔽(years)).
    // 5. Set object.[[Months]] to ℝ(𝔽(months)).
    // 6. Set object.[[Weeks]] to ℝ(𝔽(weeks)).
    // 7. Set object.[[Days]] to ℝ(𝔽(days)).
    // 8. Set object.[[Hours]] to ℝ(𝔽(hours)).
    // 9. Set object.[[Minutes]] to ℝ(𝔽(minutes)).
    // 10. Set object.[[Seconds]] to ℝ(𝔽(seconds)).
    // 11. Set object.[[Milliseconds]] to ℝ(𝔽(milliseconds)).
    // 12. Set object.[[Microseconds]] to ℝ(𝔽(microseconds)).
    // 13. Set object.[[Nanoseconds]] to ℝ(𝔽(nanoseconds)).
    let object = ordinary_create_from_constructor_of(
        vm,
        realm,
        new_target,
        Intrinsics::temporal_duration_prototype,
        |prototype| Duration::new(vm, fields, prototype),
    )?;

    // 14. Return object.
    Ok(object)
}

// 7.5.20 CreateNegatedTemporalDuration ( duration ), https://tc39.es/proposal-temporal/#sec-temporal-createnegatedtemporalduration
pub fn create_negated_temporal_duration(vm: &Vm, duration: &Duration) -> Gc<Duration> {
    // 1. Return ! CreateTemporalDuration(-duration.[[Years]], -duration.[[Months]], -duration.[[Weeks]], -duration.[[Days]], -duration.[[Hours]], -duration.[[Minutes]], -duration.[[Seconds]], -duration.[[Milliseconds]], -duration.[[Microseconds]], -duration.[[Nanoseconds]]).
    create_temporal_duration(vm, duration.fields().map(|value| -value), None).must()
}

// 7.5.21 TimeDurationFromComponents ( hours, minutes, seconds, milliseconds, microseconds, nanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-timedurationfromcomponents
pub fn time_duration_from_components(
    hours: f64,
    minutes: f64,
    seconds: f64,
    milliseconds: f64,
    microseconds: f64,
    nanoseconds: f64,
) -> TimeDuration {
    // 1. Set minutes to minutes + hours × 60.
    let total_minutes = big_integer_from_double(minutes) + big_integer_from_double(hours) * 60;

    // 2. Set seconds to seconds + minutes × 60.
    let total_seconds = big_integer_from_double(seconds) + total_minutes * 60;

    // 3. Set milliseconds to milliseconds + seconds × 1000.
    let total_milliseconds = big_integer_from_double(milliseconds) + total_seconds * 1000;

    // 4. Set microseconds to microseconds + milliseconds × 1000.
    let total_microseconds = big_integer_from_double(microseconds) + total_milliseconds * 1000;

    // 5. Set nanoseconds to nanoseconds + microseconds × 1000.
    let total_nanoseconds = big_integer_from_double(nanoseconds) + total_microseconds * 1000;

    // 6. Assert: abs(nanoseconds) ≤ maxTimeDuration.
    assert!(!magnitude_exceeds_max_time_duration(&total_nanoseconds));

    // 7. Return nanoseconds.
    total_nanoseconds
}

// 7.5.22 AddTimeDuration ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-addtimeduration
pub fn add_time_duration(vm: &Vm, one: &TimeDuration, two: &TimeDuration) -> ThrowCompletionOr<TimeDuration> {
    // 1. Let result be one + two.
    let result = one + two;

    // 2. If abs(result) > maxTimeDuration, throw a RangeError exception.
    if magnitude_exceeds_max_time_duration(&result) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidDuration, &[]);
    }

    // 3. Return result.
    Ok(result)
}

// 7.5.23 Add24HourDaysToTimeDuration ( d, days ), https://tc39.es/proposal-temporal/#sec-temporal-add24hourdaystonormalizedtimeduration
pub fn add_24_hour_days_to_time_duration(
    vm: &Vm,
    time_duration: &TimeDuration,
    days: f64,
) -> ThrowCompletionOr<TimeDuration> {
    // 1. Let result be d + days × nsPerDay.
    let result = time_duration + big_integer_from_double(days) * &*NANOSECONDS_PER_DAY;

    // 2. If abs(result) > maxTimeDuration, throw a RangeError exception.
    if magnitude_exceeds_max_time_duration(&result) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidDuration, &[]);
    }

    // 3. Return result.
    Ok(result)
}

// 7.5.24 AddTimeDurationToEpochNanoseconds ( d, epochNs ), https://tc39.es/proposal-temporal/#sec-temporal-addtimedurationtoepochnanoseconds
pub fn add_time_duration_to_epoch_nanoseconds(
    duration: &TimeDuration,
    epoch_nanoseconds: &SignedBigInteger,
) -> SignedBigInteger {
    // 1. Return epochNs + ℤ(d).
    epoch_nanoseconds + duration
}

// 7.5.25 CompareTimeDuration ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-comparetimeduration
pub fn compare_time_duration(one: &TimeDuration, two: &TimeDuration) -> i8 {
    // 1. If one > two, return 1.
    if one > two {
        return 1;
    }

    // 2. If one < two, return -1.
    if one < two {
        return -1;
    }

    // 3. Return 0.
    0
}

// 7.5.26 TimeDurationFromEpochNanosecondsDifference ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-timedurationfromepochnanosecondsdifference
pub fn time_duration_from_epoch_nanoseconds_difference(one: &SignedBigInteger, two: &SignedBigInteger) -> TimeDuration {
    // 1. Let result be ℝ(one) - ℝ(two).
    let result = one - two;

    // 2. Assert: abs(result) ≤ maxTimeDuration.
    assert!(!magnitude_exceeds_max_time_duration(&result));

    // 3. Return result.
    result
}

// 7.5.27 RoundTimeDurationToIncrement ( d, increment, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundtimedurationtoincrement
pub fn round_time_duration_to_increment(
    vm: &Vm,
    duration: &TimeDuration,
    increment: &SignedBigInteger,
    rounding_mode: RoundingMode,
) -> ThrowCompletionOr<TimeDuration> {
    // 1. Let rounded be RoundNumberToIncrement(d, increment, roundingMode).
    let rounded = round_big_number_to_increment(duration, increment, rounding_mode);

    // 2. If abs(rounded) > maxTimeDuration, throw a RangeError exception.
    if magnitude_exceeds_max_time_duration(&rounded) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidDuration, &[]);
    }

    // 3. Return rounded.
    Ok(rounded)
}

// 7.5.28 TimeDurationSign ( d ), https://tc39.es/proposal-temporal/#sec-temporal-timedurationsign
pub fn time_duration_sign(time_duration: &TimeDuration) -> i8 {
    match time_duration.sign() {
        // 1. If d < 0, return -1.
        BigIntSign::Minus => -1,
        // 2. If d > 0, return 1.
        BigIntSign::Plus => 1,
        // 3. Return 0.
        BigIntSign::NoSign => 0,
    }
}

// 7.5.29 DateDurationDays ( dateDuration, plainRelativeTo ), https://tc39.es/proposal-temporal/#sec-temporal-datedurationdays
pub fn date_duration_days(
    vm: &Vm,
    date_duration: &DateDuration,
    plain_relative_to: &PlainDate,
) -> ThrowCompletionOr<f64> {
    // 1. Let yearsMonthsWeeksDuration be ! AdjustDateDurationRecord(dateDuration, 0).
    let years_months_weeks_duration = adjust_date_duration_record(vm, date_duration, 0.0, None, None).must();

    // 2. If DateDurationSign(yearsMonthsWeeksDuration) = 0, return dateDuration.[[Days]].
    if date_duration_sign(&years_months_weeks_duration) == 0 {
        return Ok(date_duration.days);
    }

    // 3. Let later be ? CalendarDateAdd(plainRelativeTo.[[Calendar]], plainRelativeTo.[[ISODate]], yearsMonthsWeeksDuration, CONSTRAIN).
    let calendar = plain_relative_to.calendar();
    let later = calendar_date_add(
        vm,
        Utf16View::of_string(&calendar),
        plain_relative_to.iso_date(),
        &years_months_weeks_duration,
        Overflow::Constrain,
    )?;

    // 4. Let epochDays1 be ISODateToEpochDays(plainRelativeTo.[[ISODate]].[[Year]], plainRelativeTo.[[ISODate]].[[Month]] - 1, plainRelativeTo.[[ISODate]].[[Day]]).
    let relative_to_date = plain_relative_to.iso_date();
    let epoch_days1 = crate::runtime::temporal::abstract_operations::iso_date_to_epoch_days(
        f64::from(relative_to_date.year),
        f64::from(relative_to_date.month) - 1.0,
        f64::from(relative_to_date.day),
    );

    // 5. Let epochDays2 be ISODateToEpochDays(later.[[Year]], later.[[Month]] - 1, later.[[Day]]).
    let epoch_days2 = crate::runtime::temporal::abstract_operations::iso_date_to_epoch_days(
        f64::from(later.year),
        f64::from(later.month) - 1.0,
        f64::from(later.day),
    );

    // 6. Let yearsMonthsWeeksInDays be epochDays2 - epochDays1.
    let years_months_weeks_in_days = epoch_days2 - epoch_days1;

    // 7. Return dateDuration.[[Days]] + yearsMonthsWeeksInDays.
    Ok(date_duration.days + years_months_weeks_in_days)
}

// 7.5.30 RoundTimeDuration ( timeDuration, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundtimeduration
pub fn round_time_duration(
    vm: &Vm,
    time_duration: &TimeDuration,
    increment: &SignedBigInteger,
    unit: Unit,
    rounding_mode: RoundingMode,
) -> ThrowCompletionOr<TimeDuration> {
    // 1. Let divisor be the value in the "Length in Nanoseconds" column of the row of Table 21 whose "Value" column contains unit.
    let divisor = temporal_unit_length_in_nanoseconds(unit);

    // 2. Return ? RoundTimeDurationToIncrement(timeDuration, divisor × increment, roundingMode).
    round_time_duration_to_increment(vm, time_duration, &(divisor * increment), rounding_mode)
}

// 7.5.31 TotalTimeDuration ( timeDuration, unit ), https://tc39.es/proposal-temporal/#sec-temporal-totaltimeduration
pub fn total_time_duration(time_duration: &TimeDuration, unit: Unit) -> BigFraction {
    // 1. Let divisor be the value in the "Length in Nanoseconds" column of the row of Table 21 whose "Value" column contains unit.
    let divisor = temporal_unit_length_in_nanoseconds(unit);

    // 2. NOTE: The following step cannot be implemented directly using floating-point arithmetic when 𝔽(timeDuration) is
    //    not a safe integer. The division can be implemented in C++ with the __float128 type if the compiler supports it,
    //    or with software emulation such as in the SoftFP library.

    // 3. Return timeDuration / divisor.
    &BigFraction::from_integer(time_duration.clone()) / &BigFraction::from_integer(divisor.clone())
}

/// The epoch nanoseconds of a date-time in a time zone, or in UTC when it has none.
fn epoch_nanoseconds_of(
    vm: &Vm,
    date_time: &ISODateTime,
    time_zone: Option<Utf16View<'_>>,
) -> ThrowCompletionOr<SignedBigInteger> {
    match time_zone {
        None => Ok(get_utc_epoch_nanoseconds(date_time)),
        Some(time_zone) => get_epoch_nanoseconds_for(vm, time_zone, date_time, Disambiguation::Compatible),
    }
}

// 7.5.33 ComputeNudgeWindow ( sign, duration, originEpochNs, isoDateTime, timeZone, calendar, increment, unit, additionalShift ), https://tc39.es/proposal-temporal/#sec-temporal-computenudgewindow
#[allow(clippy::too_many_arguments)]
pub fn compute_nudge_window(
    vm: &Vm,
    sign: i8,
    duration: &InternalDuration,
    origin_epoch_ns: &SignedBigInteger,
    iso_date_time: &ISODateTime,
    time_zone: Option<Utf16View<'_>>,
    calendar: Utf16View<'_>,
    increment: u64,
    unit: Unit,
    additional_shift: bool,
) -> ThrowCompletionOr<NudgeWindow> {
    let signed_increment = increment as f64 * f64::from(sign);

    // 1. If unit is YEAR, then
    let (r1, r2, start_date_duration, end_date_duration) = if unit == Unit::Year {
        // a. Let years be RoundNumberToIncrement(duration.[[Date]].[[Years]], increment, TRUNC).
        let years = round_number_to_increment(duration.date.years, increment, RoundingMode::Trunc);

        // b. If additionalShift is false, then
        //     i. Let r1 be years.
        // c. Else,
        //     i. Let r1 be years + increment × sign.
        let r1 = if additional_shift {
            years + signed_increment
        } else {
            years
        };

        // d. Let r2 be r1 + increment × sign.
        let r2 = r1 + signed_increment;

        // e. Let startDuration be ? CreateDateDurationRecord(r1, 0, 0, 0).
        let start_date_duration = create_date_duration_record(vm, r1, 0.0, 0.0, 0.0)?;

        // f. Let endDuration be ? CreateDateDurationRecord(r2, 0, 0, 0).
        let end_date_duration = create_date_duration_record(vm, r2, 0.0, 0.0, 0.0)?;
        (r1, r2, start_date_duration, end_date_duration)
    }
    // 2. Else if unit is MONTH, then
    else if unit == Unit::Month {
        // a. Let months be RoundNumberToIncrement(duration.[[Date]].[[Months]], increment, TRUNC).
        let months = round_number_to_increment(duration.date.months, increment, RoundingMode::Trunc);

        // b. If additionalShift is false, then
        //     i. Let r1 be months.
        // c. Else,
        //     i. Let r1 be months + increment × sign.
        let r1 = if additional_shift {
            months + signed_increment
        } else {
            months
        };

        // d. Let r2 be r1 + increment × sign.
        let r2 = r1 + signed_increment;

        // e. Let startDuration be ? AdjustDateDurationRecord(duration.[[Date]], 0, 0, r1).
        let start_date_duration = adjust_date_duration_record(vm, &duration.date, 0.0, Some(0.0), Some(r1))?;

        // f. Let endDuration be ? AdjustDateDurationRecord(duration.[[Date]], 0, 0, r2).
        let end_date_duration = adjust_date_duration_record(vm, &duration.date, 0.0, Some(0.0), Some(r2))?;
        (r1, r2, start_date_duration, end_date_duration)
    }
    // 3. Else if unit is WEEK, then
    else if unit == Unit::Week {
        // a. Let yearsMonths be ! AdjustDateDurationRecord(duration.[[Date]], 0, 0).
        let years_months = adjust_date_duration_record(vm, &duration.date, 0.0, Some(0.0), None).must();

        // b. Let weeksStart be ? CalendarDateAdd(calendar, isoDateTime.[[ISODate]], yearsMonths, CONSTRAIN).
        let weeks_start = calendar_date_add(vm, calendar, iso_date_time.iso_date, &years_months, Overflow::Constrain)?;

        // c. Let weeksEnd be AddDaysToISODate(weeksStart, duration.[[Date]].[[Days]]).
        let weeks_end = add_days_to_iso_date(weeks_start, duration.date.days);

        // d. Let untilResult be CalendarDateUntil(calendar, weeksStart, weeksEnd, WEEK).
        let until_result = calendar_date_until(vm, calendar, weeks_start, weeks_end, Unit::Week);

        // e. Let weeks be RoundNumberToIncrement(duration.[[Date]].[[Weeks]] + untilResult.[[Weeks]], increment, TRUNC).
        let weeks = round_number_to_increment(duration.date.weeks + until_result.weeks, increment, RoundingMode::Trunc);

        // f. Let r1 be weeks.
        let r1 = weeks;

        // g. Let r2 be weeks + increment × sign.
        let r2 = weeks + signed_increment;

        // h. Let startDuration be ? AdjustDateDurationRecord(duration.[[Date]], 0, r1).
        let start_date_duration = adjust_date_duration_record(vm, &duration.date, 0.0, Some(r1), None)?;

        // i. Let endDuration be ? AdjustDateDurationRecord(duration.[[Date]], 0, r2).
        let end_date_duration = adjust_date_duration_record(vm, &duration.date, 0.0, Some(r2), None)?;
        (r1, r2, start_date_duration, end_date_duration)
    }
    // 4. Else,
    else {
        // a. Assert: unit is DAY.
        assert!(unit == Unit::Day);

        // b. Let days be RoundNumberToIncrement(duration.[[Date]].[[Days]], increment, TRUNC).
        let days = round_number_to_increment(duration.date.days, increment, RoundingMode::Trunc);

        // c. Let r1 be days.
        let r1 = days;

        // d. Let r2 be days + increment × sign.
        let r2 = days + signed_increment;

        // e. Let startDuration be ? AdjustDateDurationRecord(duration.[[Date]], r1).
        let start_date_duration = adjust_date_duration_record(vm, &duration.date, r1, None, None)?;

        // f. Let endDuration be ? AdjustDateDurationRecord(duration.[[Date]], r2).
        let end_date_duration = adjust_date_duration_record(vm, &duration.date, r2, None, None)?;
        (r1, r2, start_date_duration, end_date_duration)
    };

    // 5. Assert: If sign = 1, r1 ≥ 0 and r1 < r2.
    if sign == 1 {
        assert!(r1 >= 0.0 && r1 < r2);
    }
    // 6. Assert: If sign = -1, r1 ≤ 0 and r1 > r2.
    else if sign == -1 {
        assert!(r1 <= 0.0 && r1 > r2);
    }

    // 7. If DateDurationSign(startDateDuration) = 0, then
    let start_epoch_ns = if date_duration_sign(&start_date_duration) == 0 {
        // a. Let startEpochNs be originEpochNs.
        origin_epoch_ns.clone()
    }
    // 8. Else,
    else {
        // a. Let start be ? CalendarDateAdd(calendar, isoDateTime.[[ISODate]], startDuration, CONSTRAIN).
        let start = calendar_date_add(
            vm,
            calendar,
            iso_date_time.iso_date,
            &start_date_duration,
            Overflow::Constrain,
        )?;

        // b. Let startDateTime be CombineISODateAndTimeRecord(start, isoDateTime.[[Time]]).
        let start_date_time = combine_iso_date_and_time_record(start, iso_date_time.time);

        // c. If timeZone is UNSET, then
        //     i. Let startEpochNs be GetUTCEpochNanoseconds(startDateTime).
        // d. Else,
        //     i. Let startEpochNs be ? GetEpochNanosecondsFor(timeZone, startDateTime, COMPATIBLE).
        epoch_nanoseconds_of(vm, &start_date_time, time_zone)?
    };

    // 9. Let end be ? CalendarDateAdd(calendar, isoDateTime.[[ISODate]], endDuration, CONSTRAIN).
    let end = calendar_date_add(
        vm,
        calendar,
        iso_date_time.iso_date,
        &end_date_duration,
        Overflow::Constrain,
    )?;

    // 10. Let endDateTime be CombineISODateAndTimeRecord(end, isoDateTime.[[Time]]).
    let end_date_time = combine_iso_date_and_time_record(end, iso_date_time.time);

    // 11. If timeZone is UNSET, then
    //     a. Let endEpochNs be GetUTCEpochNanoseconds(endDateTime).
    // 12. Else,
    //     a. Let endEpochNs be ? GetEpochNanosecondsFor(timeZone, endDateTime, COMPATIBLE).
    let end_epoch_ns = epoch_nanoseconds_of(vm, &end_date_time, time_zone)?;

    // 13. Let startDuration be CombineDateAndTimeDuration(startDateDuration, 0).
    let start_duration = combine_date_and_time_duration(start_date_duration, TimeDuration::zero());

    // 14. Let endDuration be CombineDateAndTimeDuration(endDateDuration, 0).
    let end_duration = combine_date_and_time_duration(end_date_duration, TimeDuration::zero());

    // 15. Return the Record { [[R1]]: r1, [[R2]]: r2, [[StartEpochNs]]: startEpochNs, [[EndEpochNs]]: endEpochNs, [[StartDuration]]: startDuration, [[EndDuration]]: endDuration }.
    Ok(NudgeWindow {
        r1,
        r2,
        start_epoch_ns,
        end_epoch_ns,
        start_duration,
        end_duration,
    })
}

// 7.5.34 NudgeToCalendarUnit ( sign, duration, originEpochNs, destEpochNs, isoDateTime, timeZone, calendar, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-nudgetocalendarunit
#[allow(clippy::too_many_arguments)]
pub fn nudge_to_calendar_unit(
    vm: &Vm,
    sign: i8,
    duration: &InternalDuration,
    origin_epoch_ns: &SignedBigInteger,
    dest_epoch_ns: &SignedBigInteger,
    iso_date_time: &ISODateTime,
    time_zone: Option<Utf16View<'_>>,
    calendar: Utf16View<'_>,
    increment: u64,
    unit: Unit,
    rounding_mode: RoundingMode,
) -> ThrowCompletionOr<CalendarNudgeResult> {
    // 1. Let didExpandCalendarUnit be false.
    let mut did_expand_calendar_unit = false;

    // 2. Let nudgeWindow be ? ComputeNudgeWindow(sign, duration, originEpochNs, isoDateTime, timeZone, calendar, increment, unit, false).
    let mut nudge_window = compute_nudge_window(
        vm,
        sign,
        duration,
        origin_epoch_ns,
        iso_date_time,
        time_zone,
        calendar,
        increment,
        unit,
        false,
    )?;

    // 3. Let startEpochNs be nudgeWindow.[[StartEpochNs]].
    // 4. Let endEpochNs be nudgeWindow.[[EndEpochNs]].
    let recompute_nudge_window = || {
        compute_nudge_window(
            vm,
            sign,
            duration,
            origin_epoch_ns,
            iso_date_time,
            time_zone,
            calendar,
            increment,
            unit,
            true,
        )
    };

    // 5. If sign = 1, then
    if sign == 1 {
        // a. If startEpochNs ≤ destEpochNs ≤ endEpochNs is false, then
        if nudge_window.start_epoch_ns > *dest_epoch_ns || *dest_epoch_ns > nudge_window.end_epoch_ns {
            // i. Set nudgeWindow to ? ComputeNudgeWindow(sign, duration, originEpochNs, isoDateTime, timeZone, calendar, increment, unit, true).
            nudge_window = recompute_nudge_window()?;

            // ii. Assert: nudgeWindow.[[StartEpochNs]] ≤ destEpochNs ≤ nudgeWindow.[[EndEpochNs]].
            assert!(nudge_window.start_epoch_ns <= *dest_epoch_ns);
            assert!(*dest_epoch_ns <= nudge_window.end_epoch_ns);

            // iii. Set didExpandCalendarUnit to true.
            did_expand_calendar_unit = true;
        }
    }
    // 6. Else,
    else {
        // a. If endEpochNs ≤ destEpochNs ≤ startEpochNs is false, then
        if nudge_window.end_epoch_ns > *dest_epoch_ns || *dest_epoch_ns > nudge_window.start_epoch_ns {
            // i. Set nudgeWindow to ? ComputeNudgeWindow(sign, duration, originEpochNs, isoDateTime, timeZone, calendar, increment, unit, true).
            nudge_window = recompute_nudge_window()?;

            // ii. Assert: nudgeWindow.[[EndEpochNs]] ≤ destEpochNs ≤ nudgeWindow.[[StartEpochNs]].
            assert!(nudge_window.end_epoch_ns <= *dest_epoch_ns);
            assert!(*dest_epoch_ns <= nudge_window.start_epoch_ns);

            // iii. Set didExpandCalendarUnit to true.
            did_expand_calendar_unit = true;
        }
    }

    // 7. Let r1 be nudgeWindow.[[R1]].
    let r1 = nudge_window.r1;

    // 8. Let r2 be nudgeWindow.[[R2]].
    let r2 = nudge_window.r2;

    // 9. Set startEpochNs to nudgeWindow.[[StartEpochNs]].
    // 10. Set endEpochNs to nudgeWindow.[[StartEpochNs]].
    let start_epoch_ns = nudge_window.start_epoch_ns;
    let end_epoch_ns = nudge_window.end_epoch_ns;

    // 11. Let startDuration be nudgeWindow.[[StartDuration]].
    let start_duration = nudge_window.start_duration;

    // 12. Let endDuration be nudgeWindow.[[EndDuration]].
    let end_duration = nudge_window.end_duration;

    // 13. Assert: startEpochNs ≠ endEpochNs.
    assert!(start_epoch_ns != end_epoch_ns);

    // 14. Let progress be (destEpochNs - startEpochNs) / (endEpochNs - startEpochNs).
    let progress_numerator = dest_epoch_ns - &start_epoch_ns;
    let progress_denominator = &end_epoch_ns - &start_epoch_ns;
    let progress_equals_one = progress_numerator == progress_denominator;

    // 15. Let total be r1 + progress × increment × sign.
    let mut total_numerator = progress_numerator * SignedBigInteger::from(increment);

    if sign == -1 {
        total_numerator = -total_numerator;
    }
    if progress_denominator.is_negative() {
        total_numerator = -total_numerator;
    }

    let total_mv = &BigFraction::from_integer(big_integer_from_double(r1))
        + &BigFraction::new(total_numerator, progress_denominator.abs());
    let total = total_mv.to_double();

    // 16. NOTE: The above two steps cannot be implemented directly using floating-point arithmetic. This division can be
    //     implemented as if expressing total as the quotient of two time durations (which may not be safe integers),
    //     performing all other calculations before the division, and finally performing one division operation with a
    //     floating-point result for total. The division can be implemented in C++ with the __float128 type if the
    //     compiler supports it, or with software emulation such as in the SoftFP library.

    // 17. Assert: 0 ≤ progress ≤ 1.

    // 18. If sign < 0, let isNegative be NEGATIVE; else let isNegative be POSITIVE.
    let is_negative = if sign < 0 { Sign::Negative } else { Sign::Positive };

    // 19. Let unsignedRoundingMode be GetUnsignedRoundingMode(roundingMode, isNegative).
    let unsigned_rounding_mode = get_unsigned_rounding_mode(rounding_mode, is_negative);

    // 20. If progress = 1, then
    let rounded_unit = if progress_equals_one {
        // a. Let roundedUnit be abs(r2).
        r2.abs()
    }
    // 21. Else,
    else {
        // a. Assert: abs(r1) ≤ abs(total) < abs(r2).
        assert!(r1.abs() <= total.abs());
        assert!(total.abs() <= r2.abs());

        // b. Let roundedUnit be ApplyUnsignedRoundingMode(abs(total), abs(r1), abs(r2), unsignedRoundingMode).
        apply_unsigned_rounding_mode(total.abs(), r1.abs(), r2.abs(), unsigned_rounding_mode)
    };

    // 22. If roundedUnit is abs(r2), then
    let (result_duration, nudged_epoch_ns) = if rounded_unit == r2.abs() {
        // a. Set didExpandCalendarUnit to true.
        did_expand_calendar_unit = true;

        // b. Let resultDuration be endDuration.
        // c. Let nudgedEpochNs be endEpochNs.
        (end_duration, end_epoch_ns)
    }
    // 23. Else,
    else {
        // a. Let resultDuration be startDuration.
        // b. Let nudgedEpochNs be startEpochNs.
        (start_duration, start_epoch_ns)
    };

    // 24. Let nudgeResult be Duration Nudge Result Record { [[Duration]]: resultDuration, [[NudgedEpochNs]]: nudgedEpochNs, [[DidExpandCalendarUnit]]: didExpandCalendarUnit }.
    let nudge_result = DurationNudgeResult {
        duration: result_duration,
        nudged_epoch_ns,
        did_expand_calendar_unit,
    };

    // 25. Return the Record { [[NudgeResult]]: nudgeResult, [[Total]]: total }.
    Ok(CalendarNudgeResult {
        nudge_result,
        total: total_mv,
    })
}

// 7.5.35 NudgeToZonedTime ( sign, duration, isoDateTime, timeZone, calendar, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-nudgetozonedtime
#[allow(clippy::too_many_arguments)]
pub fn nudge_to_zoned_time(
    vm: &Vm,
    sign: i8,
    duration: &InternalDuration,
    iso_date_time: &ISODateTime,
    time_zone: Utf16View<'_>,
    calendar: Utf16View<'_>,
    increment: u64,
    unit: Unit,
    rounding_mode: RoundingMode,
) -> ThrowCompletionOr<DurationNudgeResult> {
    // 1. Let start be ? CalendarDateAdd(calendar, isoDateTime.[[ISODate]], duration.[[Date]], CONSTRAIN).
    let start = calendar_date_add(
        vm,
        calendar,
        iso_date_time.iso_date,
        &duration.date,
        Overflow::Constrain,
    )?;

    // 2. Let startDateTime be CombineISODateAndTimeRecord(start, isoDateTime.[[Time]]).
    let start_date_time = combine_iso_date_and_time_record(start, iso_date_time.time);

    // 3. Let endDate be AddDaysToISODate(start, sign).
    let end_date = add_days_to_iso_date(start, f64::from(sign));

    // 4. Let endDateTime be CombineISODateAndTimeRecord(endDate, isoDateTime.[[Time]]).
    let end_date_time = combine_iso_date_and_time_record(end_date, iso_date_time.time);

    // 5. Let startEpochNs be ? GetEpochNanosecondsFor(timeZone, startDateTime, COMPATIBLE).
    let start_epoch_ns = get_epoch_nanoseconds_for(vm, time_zone, &start_date_time, Disambiguation::Compatible)?;

    // 6. Let endEpochNs be ? GetEpochNanosecondsFor(timeZone, endDateTime, COMPATIBLE).
    let end_epoch_ns = get_epoch_nanoseconds_for(vm, time_zone, &end_date_time, Disambiguation::Compatible)?;

    // 7. Let daySpan be TimeDurationFromEpochNanosecondsDifference(endEpochNs, startEpochNs).
    let day_span = time_duration_from_epoch_nanoseconds_difference(&end_epoch_ns, &start_epoch_ns);

    // 8. Assert: TimeDurationSign(daySpan) = sign.
    assert!(time_duration_sign(&day_span) == sign);

    // 9. Let unitLength be the value in the "Length in Nanoseconds" column of the row of Table 21 whose "Value" column contains unit.
    let unit_length = temporal_unit_length_in_nanoseconds(unit);

    // 10. Let roundedTimeDuration be ? RoundTimeDurationToIncrement(duration.[[Time]], increment × unitLength, roundingMode).
    let unit_length_multiplied_by_increment = unit_length * SignedBigInteger::from(increment);
    let mut rounded_time_duration =
        round_time_duration_to_increment(vm, &duration.time, &unit_length_multiplied_by_increment, rounding_mode)?;

    // 11. Let beyondDaySpan be ! AddTimeDuration(roundedTimeDuration, -daySpan).
    let beyond_day_span = add_time_duration(vm, &rounded_time_duration, &-day_span).must();

    // 12. If TimeDurationSign(beyondDaySpan) ≠ -sign, then
    let (did_round_beyond_day, day_delta, nudged_epoch_ns) = if time_duration_sign(&beyond_day_span) != -sign {
        // c. Set roundedTimeDuration to ? RoundTimeDurationToIncrement(beyondDaySpan, increment × unitLength, roundingMode).
        rounded_time_duration = round_time_duration_to_increment(
            vm,
            &beyond_day_span,
            &unit_length_multiplied_by_increment,
            rounding_mode,
        )?;

        // a. Let didRoundBeyondDay be true.
        // b. Let dayDelta be sign.
        // d. Let nudgedEpochNs be AddTimeDurationToEpochNanoseconds(roundedTimeDuration, endEpochNs).
        (
            true,
            sign,
            add_time_duration_to_epoch_nanoseconds(&rounded_time_duration, &end_epoch_ns),
        )
    }
    // 13. Else,
    else {
        // a. Let didRoundBeyondDay be false.
        // b. Let dayDelta be 0.
        // c. Let nudgedEpochNs be AddTimeDurationToEpochNanoseconds(roundedTimeDuration, startEpochNs).
        (
            false,
            0,
            add_time_duration_to_epoch_nanoseconds(&rounded_time_duration, &start_epoch_ns),
        )
    };

    // 14. Let dateDuration be ! AdjustDateDurationRecord(duration.[[Date]], duration.[[Date]].[[Days]] + dayDelta).
    let date_duration = adjust_date_duration_record(
        vm,
        &duration.date,
        duration.date.days + f64::from(day_delta),
        None,
        None,
    )
    .must();

    // 15. Let resultDuration be CombineDateAndTimeDuration(dateDuration, roundedTimeDuration).
    let result_duration = combine_date_and_time_duration(date_duration, rounded_time_duration);

    // 16. Return Duration Nudge Result Record { [[Duration]]: resultDuration, [[NudgedEpochNs]]: nudgedEpochNs, [[DidExpandCalendarUnit]]: didRoundBeyondDay }.
    Ok(DurationNudgeResult {
        duration: result_duration,
        nudged_epoch_ns,
        did_expand_calendar_unit: did_round_beyond_day,
    })
}

// 7.5.36 NudgeToDayOrTime ( duration, destEpochNs, largestUnit, increment, smallestUnit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-nudgetodayortime
pub fn nudge_to_day_or_time(
    vm: &Vm,
    duration: &InternalDuration,
    dest_epoch_ns: &SignedBigInteger,
    largest_unit: Unit,
    increment: u64,
    smallest_unit: Unit,
    rounding_mode: RoundingMode,
) -> ThrowCompletionOr<DurationNudgeResult> {
    // 1. Let timeDuration be ! Add24HourDaysToTimeDuration(duration.[[Time]], duration.[[Date]].[[Days]]).
    let time_duration = add_24_hour_days_to_time_duration(vm, &duration.time, duration.date.days).must();

    // 2. Let unitLength be the value in the "Length in Nanoseconds" column of the row of Table 21 whose "Value" column contains smallestUnit.
    let unit_length = temporal_unit_length_in_nanoseconds(smallest_unit);

    // 3. Let roundedTime be ? RoundTimeDurationToIncrement(timeDuration, unitLength × increment, roundingMode).
    let unit_length_multiplied_by_increment = unit_length * SignedBigInteger::from(increment);
    let rounded_time =
        round_time_duration_to_increment(vm, &time_duration, &unit_length_multiplied_by_increment, rounding_mode)?;

    // 4. Let diffTime be ! AddTimeDuration(roundedTime, -timeDuration).
    let diff_time = add_time_duration(vm, &rounded_time, &-&time_duration).must();

    // 5. Let wholeDays be truncate(TotalTimeDuration(timeDuration, DAY)).
    let whole_days = total_time_duration(&time_duration, Unit::Day).to_double().trunc();

    // 6. Let roundedWholeDays be truncate(TotalTimeDuration(roundedTime, DAY)).
    let rounded_whole_days = total_time_duration(&rounded_time, Unit::Day).to_double().trunc();

    // 7. Let dayDelta be roundedWholeDays - wholeDays.
    let day_delta = rounded_whole_days - whole_days;

    // 8. If dayDelta < 0, let dayDeltaSign be -1; else if dayDelta > 0, let dayDeltaSign be 1; else let dayDeltaSign be 0.
    let day_delta_sign = if day_delta < 0.0 {
        -1
    } else if day_delta > 0.0 {
        1
    } else {
        0
    };

    // 9. If dayDeltaSign = TimeDurationSign(timeDuration), let didExpandDays be true; else let didExpandDays be false.
    let did_expand_days = day_delta_sign == time_duration_sign(&time_duration);

    // 10. Let nudgedEpochNs be AddTimeDurationToEpochNanoseconds(diffTime, destEpochNs).
    let nudged_epoch_ns = add_time_duration_to_epoch_nanoseconds(&diff_time, dest_epoch_ns);

    // 11. Let days be 0.
    let mut days = 0.0;

    // 12. Let remainder be roundedTime.
    // 13. If TemporalUnitCategory(largestUnit) is DATE, then
    let remainder = if temporal_unit_category(largest_unit) == UnitCategory::Date {
        // a. Set days to roundedWholeDays.
        days = rounded_whole_days;

        // b. Set remainder to ! AddTimeDuration(roundedTime, TimeDurationFromComponents(-roundedWholeDays * HoursPerDay, 0, 0, 0, 0, 0)).
        add_time_duration(
            vm,
            &rounded_time,
            &time_duration_from_components(-rounded_whole_days * HOURS_PER_DAY, 0.0, 0.0, 0.0, 0.0, 0.0),
        )
        .must()
    } else {
        rounded_time
    };

    // 14. Let dateDuration be ! AdjustDateDurationRecord(duration.[[Date]], days).
    let date_duration = adjust_date_duration_record(vm, &duration.date, days, None, None).must();

    // 15. Let resultDuration be CombineDateAndTimeDuration(dateDuration, remainder).
    let result_duration = combine_date_and_time_duration(date_duration, remainder);

    // 16. Return Duration Nudge Result Record { [[Duration]]: resultDuration, [[NudgedEpochNs]]: nudgedEpochNs, [[DidExpandCalendarUnit]]: didExpandDays }.
    Ok(DurationNudgeResult {
        duration: result_duration,
        nudged_epoch_ns,
        did_expand_calendar_unit: did_expand_days,
    })
}

// 7.5.37 BubbleRelativeDuration ( sign, duration, nudgedEpochNs, isoDateTime, timeZone, calendar, largestUnit, smallestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-bubblerelativeduration
#[allow(clippy::too_many_arguments)]
pub fn bubble_relative_duration(
    vm: &Vm,
    sign: i8,
    mut duration: InternalDuration,
    nudged_epoch_ns: &SignedBigInteger,
    iso_date_time: &ISODateTime,
    time_zone: Option<Utf16View<'_>>,
    calendar: Utf16View<'_>,
    largest_unit: Unit,
    smallest_unit: Unit,
) -> ThrowCompletionOr<InternalDuration> {
    // 1. If smallestUnit is largestUnit, return duration.
    if smallest_unit == largest_unit {
        return Ok(duration);
    }

    // 2. Let largestUnitIndex be the ordinal index of the row of Table 21 whose "Value" column contains largestUnit.
    let largest_unit_index = largest_unit as i32;

    // 3. Let smallestUnitIndex be the ordinal index of the row of Table 21 whose "Value" column contains smallestUnit.
    let smallest_unit_index = smallest_unit as i32;

    // 4. Let unitIndex be smallestUnitIndex - 1.
    let mut unit_index = smallest_unit_index - 1;

    // 5. Let done be false.
    let mut done = false;

    // 6. Repeat, while unitIndex ≥ largestUnitIndex and done is false,
    while unit_index >= largest_unit_index && !done {
        // a. Let unit be the value in the "Value" column of Table 21 in the row whose ordinal index is unitIndex.
        let unit = Unit::from_index(unit_index as usize);

        // b. If unit is not WEEK, or largestUnit is WEEK, then
        if unit != Unit::Week || largest_unit == Unit::Week {
            // i. If unit is YEAR, then
            let end_duration = if unit == Unit::Year {
                // 1. Let years be duration.[[Date]].[[Years]] + sign.
                let years = duration.date.years + f64::from(sign);

                // 2. Let endDuration be ? CreateDateDurationRecord(years, 0, 0, 0).
                create_date_duration_record(vm, years, 0.0, 0.0, 0.0)?
            }
            // ii. Else if unit is MONTH, then
            else if unit == Unit::Month {
                // 1. Let months be duration.[[Date]].[[Months]] + sign.
                let months = duration.date.months + f64::from(sign);

                // 2. Let endDuration be ? AdjustDateDurationRecord(duration.[[Date]], 0, 0, months).
                adjust_date_duration_record(vm, &duration.date, 0.0, Some(0.0), Some(months))?
            }
            // iii. Else,
            else {
                // 1. Assert: unit is WEEK.
                assert!(unit == Unit::Week);

                // 2. Let weeks be duration.[[Date]].[[Weeks]] + sign.
                let weeks = duration.date.weeks + f64::from(sign);

                // 3. Let endDuration be ? AdjustDateDurationRecord(duration.[[Date]], 0, weeks).
                adjust_date_duration_record(vm, &duration.date, 0.0, Some(weeks), None)?
            };

            // iv. Let end be ? CalendarDateAdd(calendar, isoDateTime.[[ISODate]], endDuration, CONSTRAIN).
            let end = calendar_date_add(vm, calendar, iso_date_time.iso_date, &end_duration, Overflow::Constrain)?;

            // v. Let endDateTime be CombineISODateAndTimeRecord(end, isoDateTime.[[Time]]).
            let end_date_time = combine_iso_date_and_time_record(end, iso_date_time.time);

            // vi. If timeZone is UNSET, then
            //     1. Let endEpochNs be GetUTCEpochNanoseconds(endDateTime).
            // vii. Else,
            //     1. Let endEpochNs be ? GetEpochNanosecondsFor(timeZone, endDateTime, COMPATIBLE).
            let end_epoch_ns = epoch_nanoseconds_of(vm, &end_date_time, time_zone)?;

            // viii. Let beyondEnd be nudgedEpochNs - endEpochNs.
            let beyond_end = nudged_epoch_ns - &end_epoch_ns;

            // ix. If beyondEnd < 0, let beyondEndSign be -1; else if beyondEnd > 0, let beyondEndSign be 1; else let beyondEndSign be 0.
            let beyond_end_sign = time_duration_sign(&beyond_end);

            // x. If beyondEndSign ≠ -sign, then
            if beyond_end_sign != -sign {
                // 1. Set duration to CombineDateAndTimeDuration(endDuration, 0).
                duration = combine_date_and_time_duration(end_duration, TimeDuration::zero());
            }
            // xi. Else,
            else {
                // 1. Set done to true.
                done = true;
            }
        }

        // c. Set unitIndex to unitIndex - 1.
        unit_index -= 1;
    }

    // 7. Return duration.
    Ok(duration)
}

// 7.5.38 RoundRelativeDuration ( duration, originEpochNs, destEpochNs, isoDateTime, timeZone, calendar, largestUnit, increment, smallestUnit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundrelativeduration
#[allow(clippy::too_many_arguments)]
pub fn round_relative_duration(
    vm: &Vm,
    duration: InternalDuration,
    origin_epoch_ns: &SignedBigInteger,
    dest_epoch_ns: &SignedBigInteger,
    iso_date_time: &ISODateTime,
    time_zone: Option<Utf16View<'_>>,
    calendar: Utf16View<'_>,
    largest_unit: Unit,
    increment: u64,
    smallest_unit: Unit,
    rounding_mode: RoundingMode,
) -> ThrowCompletionOr<InternalDuration> {
    // 1. Let irregularLengthUnit be false.
    // 2. If IsCalendarUnit(smallestUnit) is true, set irregularLengthUnit to true.
    // 3. If timeZone is not UNSET and smallestUnit is DAY, set irregularLengthUnit to true.
    let irregular_length_unit = is_calendar_unit(smallest_unit) || (time_zone.is_some() && smallest_unit == Unit::Day);

    // 4. If InternalDurationSign(duration) < 0, let sign be -1; else let sign be 1.
    let sign: i8 = if internal_duration_sign(&duration) < 0 { -1 } else { 1 };

    // 5. If irregularLengthUnit is true, then
    let nudge_result = if irregular_length_unit {
        // a. Let record be ? NudgeToCalendarUnit(sign, duration, originEpochNs, destEpochNs, isoDateTime, timeZone, calendar, increment, smallestUnit, roundingMode).
        let record = nudge_to_calendar_unit(
            vm,
            sign,
            &duration,
            origin_epoch_ns,
            dest_epoch_ns,
            iso_date_time,
            time_zone,
            calendar,
            increment,
            smallest_unit,
            rounding_mode,
        )?;

        // b. Let nudgeResult be record.[[NudgeResult]].
        record.nudge_result
    }
    // 6. Else if timeZone is not UNSET, then
    else if let Some(time_zone) = time_zone {
        // a. Let nudgeResult be ? NudgeToZonedTime(sign, duration, isoDateTime, timeZone, calendar, increment, smallestUnit, roundingMode).
        nudge_to_zoned_time(
            vm,
            sign,
            &duration,
            iso_date_time,
            time_zone,
            calendar,
            increment,
            smallest_unit,
            rounding_mode,
        )?
    }
    // 7. Else,
    else {
        // a. Let nudgeResult be ? NudgeToDayOrTime(duration, destEpochNs, largestUnit, increment, smallestUnit, roundingMode).
        nudge_to_day_or_time(
            vm,
            &duration,
            dest_epoch_ns,
            largest_unit,
            increment,
            smallest_unit,
            rounding_mode,
        )?
    };

    // 8. Set duration to nudgeResult.[[Duration]].
    let mut duration = nudge_result.duration;

    // 9. If nudgeResult.[[DidExpandCalendarUnit]] is true and smallestUnit is not WEEK, then
    if nudge_result.did_expand_calendar_unit && smallest_unit != Unit::Week {
        // a. Let startUnit be LargerOfTwoTemporalUnits(smallestUnit, DAY).
        let start_unit = larger_of_two_temporal_units(smallest_unit, Unit::Day);

        // b. Set duration to ? BubbleRelativeDuration(sign, duration, nudgeResult.[[NudgedEpochNs]], isoDateTime, timeZone, calendar, largestUnit, startUnit).
        duration = bubble_relative_duration(
            vm,
            sign,
            duration,
            &nudge_result.nudged_epoch_ns,
            iso_date_time,
            time_zone,
            calendar,
            largest_unit,
            start_unit,
        )?;
    }

    // 10. Return duration.
    Ok(duration)
}

// 7.5.39 TotalRelativeDuration ( duration, originEpochNs, destEpochNs, isoDateTime, timeZone, calendar, unit ), https://tc39.es/proposal-temporal/#sec-temporal-totalrelativeduration
#[allow(clippy::too_many_arguments)]
pub fn total_relative_duration(
    vm: &Vm,
    duration: &InternalDuration,
    origin_epoch_ns: &SignedBigInteger,
    dest_epoch_ns: &SignedBigInteger,
    iso_date_time: &ISODateTime,
    time_zone: Option<Utf16View<'_>>,
    calendar: Utf16View<'_>,
    unit: Unit,
) -> ThrowCompletionOr<BigFraction> {
    // 1. If IsCalendarUnit(unit) is true, or timeZone is not UNSET and unit is DAY, then
    if is_calendar_unit(unit) || (time_zone.is_some() && unit == Unit::Day) {
        // a. If InternalDurationSign(duration) < 0, let sign be -1; else let sign be 1.
        let sign = if internal_duration_sign(duration) < 0 { -1 } else { 1 };

        // b. Let record be ? NudgeToCalendarUnit(sign, duration, originEpochNs, destEpochNs, isoDateTime, timeZone, calendar, 1, unit, TRUNC).
        let record = nudge_to_calendar_unit(
            vm,
            sign,
            duration,
            origin_epoch_ns,
            dest_epoch_ns,
            iso_date_time,
            time_zone,
            calendar,
            1,
            unit,
            RoundingMode::Trunc,
        )?;

        // c. Return record.[[Total]].
        return Ok(record.total);
    }

    // 2. Let timeDuration be ! Add24HourDaysToTimeDuration(duration.[[Time]], duration.[[Date]].[[Days]]).
    let time_duration = add_24_hour_days_to_time_duration(vm, &duration.time, duration.date.days).must();

    // 3. Return TotalTimeDuration(timeDuration, unit).
    Ok(total_time_duration(&time_duration, unit))
}

// 7.5.40 TemporalDurationToString ( duration, precision ), https://tc39.es/proposal-temporal/#sec-temporal-temporaldurationtostring
pub fn temporal_duration_to_string(duration: &Duration, precision: Precision) -> String {
    // 1. Let sign be DurationSign(duration).
    let sign = duration_sign(duration);

    // 2. Let datePart be the empty String.
    let mut date_part = String::new();

    // 3. If duration.[[Years]] ≠ 0, then
    if duration.years() != 0.0 {
        // a. Set datePart to the string concatenation of abs(duration.[[Years]]) formatted as a decimal number and the
        //    code unit 0x0059 (LATIN CAPITAL LETTER Y).
        date_part.push_str(&format!("{}Y", AkDouble(duration.years().abs())));
    }
    // 4. If duration.[[Months]] ≠ 0, then
    if duration.months() != 0.0 {
        // a. Set datePart to the string concatenation of datePart, abs(duration.[[Months]]) formatted as a decimal number,
        //    and the code unit 0x004D (LATIN CAPITAL LETTER M).
        date_part.push_str(&format!("{}M", AkDouble(duration.months().abs())));
    }
    // 5. If duration.[[Weeks]] ≠ 0, then
    if duration.weeks() != 0.0 {
        // a. Set datePart to the string concatenation of datePart, abs(duration.[[Weeks]]) formatted as a decimal number,
        //    and the code unit 0x0057 (LATIN CAPITAL LETTER W).
        date_part.push_str(&format!("{}W", AkDouble(duration.weeks().abs())));
    }
    // 6. If duration.[[Days]] ≠ 0, then
    if duration.days() != 0.0 {
        // a. Set datePart to the string concatenation of datePart, abs(duration.[[Days]]) formatted as a decimal number,
        //    and the code unit 0x0044 (LATIN CAPITAL LETTER D).
        date_part.push_str(&format!("{}D", AkDouble(duration.days().abs())));
    }

    // 7. Let timePart be the empty String.
    let mut time_part = String::new();

    // 8. If duration.[[Hours]] ≠ 0, then
    if duration.hours() != 0.0 {
        // a. Set timePart to the string concatenation of abs(duration.[[Hours]]) formatted as a decimal number and the
        //    code unit 0x0048 (LATIN CAPITAL LETTER H).
        time_part.push_str(&format!("{}H", AkDouble(duration.hours().abs())));
    }
    // 9. If duration.[[Minutes]] ≠ 0, then
    if duration.minutes() != 0.0 {
        // a. Set timePart to the string concatenation of timePart, abs(duration.[[Minutes]]) formatted as a decimal number,
        //    and the code unit 0x004D (LATIN CAPITAL LETTER M).
        time_part.push_str(&format!("{}M", AkDouble(duration.minutes().abs())));
    }

    // 10. Let zeroMinutesAndHigher be false.
    // 11. If DefaultTemporalLargestUnit(duration) is one of SECOND, MILLISECOND, MICROSECOND, or NANOSECOND, set zeroMinutesAndHigher to true.
    let zero_minutes_and_higher = matches!(
        default_temporal_largest_unit(duration),
        Unit::Second | Unit::Millisecond | Unit::Microsecond | Unit::Nanosecond
    );

    // 12. Let secondsDuration be TimeDurationFromComponents(0, 0, duration.[[Seconds]], duration.[[Milliseconds]], duration.[[Microseconds]], duration.[[Nanoseconds]]).
    let seconds_duration = time_duration_from_components(
        0.0,
        0.0,
        duration.seconds(),
        duration.milliseconds(),
        duration.microseconds(),
        duration.nanoseconds(),
    );

    // 13. If secondsDuration ≠ 0, or zeroMinutesAndHigher is true, or precision is not auto, then
    if !seconds_duration.is_zero() || zero_minutes_and_higher || precision != Precision::Auto {
        let quotient = &seconds_duration / &*NANOSECONDS_PER_SECOND;
        let remainder = &seconds_duration % &*NANOSECONDS_PER_SECOND;

        // a. Let secondsPart be abs(truncate(secondsDuration / 10**9)) formatted as a decimal number.
        let seconds_part = quotient.magnitude().to_string();

        // b. Let subSecondsPart be FormatFractionalSeconds(abs(remainder(secondsDuration, 10**9)), precision).
        let sub_seconds_part = format_fractional_seconds(
            remainder
                .magnitude()
                .to_u64()
                .expect("less than a second fits in a u64"),
            precision,
        );

        // c. Set timePart to the string concatenation of timePart, secondsPart, subSecondsPart, and the code unit
        //    0x0053 (LATIN CAPITAL LETTER S).
        time_part.push_str(&format!("{seconds_part}{sub_seconds_part}S"));
    }

    // 14. Let signPart be the code unit 0x002D (HYPHEN-MINUS) if sign < 0, and otherwise the empty String.
    let sign_part = if sign < 0 { "-" } else { "" };

    // 15. Let result be the string concatenation of signPart, the code unit 0x0050 (LATIN CAPITAL LETTER P) and datePart.
    let mut result = format!("{sign_part}P{date_part}");

    // 16. If timePart is not the empty String, then
    if !time_part.is_empty() {
        // a. Set result to the string concatenation of result, the code unit 0x0054 (LATIN CAPITAL LETTER T), and timePart.
        result.push('T');
        result.push_str(&time_part);
    }

    // 17. Return result.
    result
}

// 7.5.41 AddDurations ( operation, duration, other ), https://tc39.es/proposal-temporal/#sec-temporal-adddurations
pub fn add_durations(
    vm: &Vm,
    operation: ArithmeticOperation,
    duration: &Duration,
    other_value: Value,
) -> ThrowCompletionOr<Gc<Duration>> {
    // 1. Set other to ? ToTemporalDuration(other).
    let mut other = to_temporal_duration(vm, other_value)?;

    // 2. If operation is subtract, set other to CreateNegatedTemporalDuration(other).
    if operation == ArithmeticOperation::Subtract {
        other = create_negated_temporal_duration(vm, &other);
    }

    // 3. Let largestUnit1 be DefaultTemporalLargestUnit(duration).
    let largest_unit1 = default_temporal_largest_unit(duration);

    // 4. Let largestUnit2 be DefaultTemporalLargestUnit(other).
    let largest_unit2 = default_temporal_largest_unit(&other);

    // 5. Let largestUnit be LargerOfTwoTemporalUnits(largestUnit1, largestUnit2).
    let largest_unit = larger_of_two_temporal_units(largest_unit1, largest_unit2);

    // 6. If IsCalendarUnit(largestUnit) is true, throw a RangeError exception.
    if is_calendar_unit(largest_unit) {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TemporalInvalidLargestUnit,
            &[&"a calendar unit"],
        );
    }

    // 7. Let d1 be ToInternalDurationRecordWith24HourDays(duration).
    let duration1 = to_internal_duration_record_with_24_hour_days(vm, duration);

    // 8. Let d2 be ToInternalDurationRecordWith24HourDays(other).
    let duration2 = to_internal_duration_record_with_24_hour_days(vm, &other);

    // 9. Let timeResult be ? AddTimeDuration(d1.[[Time]], d2.[[Time]]).
    let time_result = add_time_duration(vm, &duration1.time, &duration2.time)?;

    // 10. Let result be CombineDateAndTimeDuration(ZeroDateDuration(), timeResult).
    let result = combine_date_and_time_duration(zero_date_duration(vm), time_result);

    // 11. Return ? TemporalDurationFromInternal(result, largestUnit).
    temporal_duration_from_internal(vm, &result, largest_unit)
}
