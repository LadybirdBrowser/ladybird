/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Date objects and the time value math of Libraries/LibJS/Runtime/Date.cpp.

use core::cell::Cell;
use core::ops::Deref;
use std::sync::Mutex;

use ak::Utf16String;
use libjs_runtime_macros::Trace;
use num_integer::Integer;
use num_traits::{FromPrimitive, ToPrimitive};

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::big_int::SignedBigInteger;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::abstract_operations::get_available_named_time_zone_identifier;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::realm::Realm;
use crate::runtime::value_conversions::to_integer_or_infinity;
use crate::unicode::time_zone::{
    self as unicode_time_zone, IncludeGivenTime, TimeZoneOffset as UnicodeTimeZoneOffset, TimeZoneTransitionOptions,
    TransitionDirection, TransitionRule, UnixDateTime,
};
use crate::utf16::Utf16View;

/// A Date object, whose [[DateValue]] is `date_value`.
#[repr(C)]
#[derive(Trace)]
pub struct Date {
    base: Object,
    date_value: Cell<f64>,
}

define_cell!(Date, Object, extends: [Object]);

impl Deref for Date {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Date {
    /// Date(double date_value, Object& prototype).
    pub fn new(vm: &Vm, date_value: f64, prototype: Gc<Object>) -> Date {
        Date {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            date_value: Cell::new(date_value),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>, date_value: f64) -> Gc<Date> {
        realm.create_object(vm, Date::new(vm, date_value, realm.intrinsics().date_prototype(vm)))
    }

    pub fn date_value(&self) -> f64 {
        self.date_value.get()
    }

    pub fn set_date_value(&self, value: f64) {
        self.date_value.set(value);
    }

    pub fn iso_date_string(&self) -> Utf16String {
        let date_value = self.date_value();
        let year = year_from_time(date_value);

        let mut string = if year < 0 {
            format!("-{:06}", -year)
        } else if year > 9999 {
            format!("+{year:06}")
        } else {
            format!("{year:04}")
        };
        string.push_str(&format!(
            "-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
            month_from_time(date_value) + 1,
            date_from_time(date_value),
            hour_from_time(date_value),
            min_from_time(date_value),
            sec_from_time(date_value),
            ms_from_time(date_value),
        ));
        Utf16String::from_utf8(&string)
    }
}

// 21.4.1.22 Time Zone Identifier Record, https://tc39.es/ecma262/#sec-time-zone-identifier-record
#[derive(Clone)]
pub struct TimeZoneIdentifier {
    pub identifier: Utf16String,         // [[Identifier]]
    pub primary_identifier: Utf16String, // [[PrimaryIdentifier]]
}

// https://tc39.es/ecma262/#eqn-HoursPerDay
pub const HOURS_PER_DAY: f64 = 24.0;
// https://tc39.es/ecma262/#eqn-MinutesPerHour
pub const MINUTES_PER_HOUR: f64 = 60.0;
// https://tc39.es/ecma262/#eqn-SecondsPerMinute
pub const SECONDS_PER_MINUTE: f64 = 60.0;
// https://tc39.es/ecma262/#eqn-msPerSecond
pub const MS_PER_SECOND: f64 = 1_000.0;
// https://tc39.es/ecma262/#eqn-msPerMinute
pub const MS_PER_MINUTE: f64 = 60_000.0;
// https://tc39.es/ecma262/#eqn-msPerHour
pub const MS_PER_HOUR: f64 = 3_600_000.0;
// https://tc39.es/ecma262/#eqn-msPerDay
pub const MS_PER_DAY: f64 = 86_400_000.0;
// https://tc39.es/proposal-temporal/#eqn-nsPerDay
pub const NS_PER_DAY: f64 = 86_400_000_000_000.0;

// https://tc39.es/ecma262/#sec-time-values-and-time-range
// A time value supports a [...] range of -8,640,000,000,000,000 to 8,640,000,000,000,000 milliseconds
pub const MAX_TIME_VALUE: f64 = 8.64E15;

/// The notation “x modulo y” of the spec for doubles, as the C++ modulo() computes it with fmod.
fn modulo(x: f64, y: f64) -> f64 {
    assert!(y != 0.0 && y.is_finite());
    let remainder = x % y;
    if remainder < 0.0 { remainder + y } else { remainder }
}

// 21.4.1.3 Day ( t ), https://tc39.es/ecma262/#sec-day
pub fn day(time_value: f64) -> f64 {
    // 1. Return 𝔽(floor(ℝ(t / msPerDay))).
    (time_value / MS_PER_DAY).floor()
}

// 21.4.1.4 TimeWithinDay ( t ), https://tc39.es/ecma262/#sec-timewithinday
pub fn time_within_day(time: f64) -> f64 {
    // 1. Return 𝔽(ℝ(t) modulo ℝ(msPerDay)).
    modulo(time, MS_PER_DAY)
}

// 21.4.1.5 DaysInYear ( y ), https://tc39.es/ecma262/#sec-daysinyear
pub fn days_in_year(y: i32) -> u16 {
    // 1. Let ry be ℝ(y).
    let ry = f64::from(y);

    // 2. If (ry modulo 400) = 0, return 366𝔽.
    if modulo(ry, 400.0) == 0.0 {
        return 366;
    }

    // 3. If (ry modulo 100) = 0, return 365𝔽.
    if modulo(ry, 100.0) == 0.0 {
        return 365;
    }

    // 4. If (ry modulo 4) = 0, return 366𝔽.
    if modulo(ry, 4.0) == 0.0 {
        return 366;
    }

    // 5. Return 365𝔽.
    365
}

// 21.4.1.6 DayFromYear ( y ), https://tc39.es/ecma262/#sec-dayfromyear
pub fn day_from_year(y: i32) -> f64 {
    // 1. Let ry be ℝ(y).
    let ry = f64::from(y);

    // 2. NOTE: In the following steps, each _numYearsN_ is the number of years divisible by N that occur between the
    //    epoch and the start of year y. (The number is negative if y is before the epoch.)

    // 3. Let numYears1 be (ry - 1970).
    let num_years_1 = ry - 1970.0;

    // 4. Let numYears4 be floor((ry - 1969) / 4).
    let num_years_4 = ((ry - 1969.0) / 4.0).floor();

    // 5. Let numYears100 be floor((ry - 1901) / 100).
    let num_years_100 = ((ry - 1901.0) / 100.0).floor();

    // 6. Let numYears400 be floor((ry - 1601) / 400).
    let num_years_400 = ((ry - 1601.0) / 400.0).floor();

    // 7. Return 𝔽(365 × numYears1 + numYears4 - numYears100 + numYears400).
    365.0 * num_years_1 + num_years_4 - num_years_100 + num_years_400
}

// 21.4.1.7 TimeFromYear ( y ), https://tc39.es/ecma262/#sec-timefromyear
pub fn time_from_year(y: i32) -> f64 {
    // 1. Return msPerDay × DayFromYear(y).
    MS_PER_DAY * day_from_year(y)
}

// 21.4.1.8 YearFromTime ( t ), https://tc39.es/ecma262/#sec-yearfromtime
pub fn year_from_time(t: f64) -> i32 {
    // 1. Return the largest integral Number y (closest to +∞) such that TimeFromYear(y) ≤ t.
    if !t.is_finite() {
        return i32::MAX;
    }

    // Approximation using average number of milliseconds per year. We might have to adjust this guess afterwards.
    // NB: The conversion saturates, as the C++ static_cast does on the platforms Ladybird runs on, and so does the
    //     adjustment wrap around like the C++ arithmetic, for times far outside the time value range.
    let mut year = (t / (365.2425 * MS_PER_DAY) + 1970.0).floor() as i32;

    let year_t = time_from_year(year);
    if year_t > t {
        year = year.wrapping_sub(1);
    } else if year_t + f64::from(days_in_year(year)) * MS_PER_DAY <= t {
        year = year.wrapping_add(1);
    }

    year
}

/// static_cast<u16>() of an integral double, as the day within a year is computed: a value outside the u16 range,
/// which only a year far outside the time value range gives, stays outside the range of a day within a year, as the
/// C++ register it lives in does.
fn day_within_year_to_u16(value: f64) -> u16 {
    if (0.0..=f64::from(u16::MAX)).contains(&value) {
        value as u16
    } else {
        u16::MAX
    }
}

// 21.4.1.9 DayWithinYear ( t ), https://tc39.es/ecma262/#sec-daywithinyear
pub fn day_within_year(t: f64) -> u16 {
    if !t.is_finite() {
        return 0;
    }

    // 1. Return Day(t) - DayFromYear(YearFromTime(t)).
    day_within_year_to_u16(day(t) - day_from_year(year_from_time(t)))
}

// 21.4.1.10 InLeapYear ( t ), https://tc39.es/ecma262/#sec-inleapyear
pub fn in_leap_year(t: f64) -> bool {
    // 1. If DaysInYear(YearFromTime(t)) is 366𝔽, return 1𝔽; else return +0𝔽.
    days_in_year(year_from_time(t)) == 366
}

// 21.4.1.11 MonthFromTime ( t ), https://tc39.es/ecma262/#sec-monthfromtime
pub fn month_from_time(t: f64) -> u8 {
    // 1. Let inLeapYear be InLeapYear(t).
    let in_leap_year = u16::from(in_leap_year(t));

    // 2. Let dayWithinYear be DayWithinYear(t).
    let day_within_year = day_within_year(t);

    // 3. If dayWithinYear < 31𝔽, return +0𝔽.
    if day_within_year < 31 {
        return 0;
    }

    // 4. If dayWithinYear < 59𝔽 + inLeapYear, return 1𝔽.
    if day_within_year < 59 + in_leap_year {
        return 1;
    }

    // 5. If dayWithinYear < 90𝔽 + inLeapYear, return 2𝔽.
    if day_within_year < 90 + in_leap_year {
        return 2;
    }

    // 6. If dayWithinYear < 120𝔽 + inLeapYear, return 3𝔽.
    if day_within_year < 120 + in_leap_year {
        return 3;
    }

    // 7. If dayWithinYear < 151𝔽 + inLeapYear, return 4𝔽.
    if day_within_year < 151 + in_leap_year {
        return 4;
    }

    // 8. If dayWithinYear < 181𝔽 + inLeapYear, return 5𝔽.
    if day_within_year < 181 + in_leap_year {
        return 5;
    }

    // 9. If dayWithinYear < 212𝔽 + inLeapYear, return 6𝔽.
    if day_within_year < 212 + in_leap_year {
        return 6;
    }

    // 10. If dayWithinYear < 243𝔽 + inLeapYear, return 7𝔽.
    if day_within_year < 243 + in_leap_year {
        return 7;
    }

    // 11. If dayWithinYear < 273𝔽 + inLeapYear, return 8𝔽.
    if day_within_year < 273 + in_leap_year {
        return 8;
    }

    // 12. If dayWithinYear < 304𝔽 + inLeapYear, return 9𝔽.
    if day_within_year < 304 + in_leap_year {
        return 9;
    }

    // 13. If dayWithinYear < 334𝔽 + inLeapYear, return 10𝔽.
    if day_within_year < 334 + in_leap_year {
        return 10;
    }

    // 14. Assert: dayWithinYear < 365𝔽 + inLeapYear.
    assert!(
        day_within_year < 365 + in_leap_year,
        "day_within_year < (365 + in_leap_year)"
    );

    // 15. Return 11𝔽.
    11
}

// 21.4.1.12 DateFromTime ( t ), https://tc39.es/ecma262/#sec-datefromtime
pub fn date_from_time(t: f64) -> u8 {
    // 1. Let inLeapYear be InLeapYear(t).
    let in_leap_year = i32::from(in_leap_year(t));

    // 2. Let dayWithinYear be DayWithinYear(t).
    let day_within_year = i32::from(day_within_year(t));

    // 3. Let month be MonthFromTime(t).
    let month = month_from_time(t);

    let date = match month {
        // 4. If month is +0𝔽, return dayWithinYear + 1𝔽.
        0 => day_within_year + 1,
        // 5. If month is 1𝔽, return dayWithinYear - 30𝔽.
        1 => day_within_year - 30,
        // 6. If month is 2𝔽, return dayWithinYear - 58𝔽 - inLeapYear.
        2 => day_within_year - 58 - in_leap_year,
        // 7. If month is 3𝔽, return dayWithinYear - 89𝔽 - inLeapYear.
        3 => day_within_year - 89 - in_leap_year,
        // 8. If month is 4𝔽, return dayWithinYear - 119𝔽 - inLeapYear.
        4 => day_within_year - 119 - in_leap_year,
        // 9. If month is 5𝔽, return dayWithinYear - 150𝔽 - inLeapYear.
        5 => day_within_year - 150 - in_leap_year,
        // 10. If month is 6𝔽, return dayWithinYear - 180𝔽 - inLeapYear.
        6 => day_within_year - 180 - in_leap_year,
        // 11. If month is 7𝔽, return dayWithinYear - 211𝔽 - inLeapYear.
        7 => day_within_year - 211 - in_leap_year,
        // 12. If month is 8𝔽, return dayWithinYear - 242𝔽 - inLeapYear.
        8 => day_within_year - 242 - in_leap_year,
        // 13. If month is 9𝔽, return dayWithinYear - 272𝔽 - inLeapYear.
        9 => day_within_year - 272 - in_leap_year,
        // 14. If month is 10𝔽, return dayWithinYear - 303𝔽 - inLeapYear.
        10 => day_within_year - 303 - in_leap_year,
        _ => {
            // 15. Assert: month is 11𝔽.
            assert!(month == 11, "month == 11");

            // 16. Return dayWithinYear - 333𝔽 - inLeapYear.
            day_within_year - 333 - in_leap_year
        }
    };
    date as u8
}

// 21.4.1.13 WeekDay ( t ), https://tc39.es/ecma262/#sec-weekday
pub fn week_day(t: f64) -> u8 {
    if !t.is_finite() {
        return 0;
    }

    // 1. Return 𝔽(ℝ(Day(t) + 4𝔽) modulo 7).
    modulo(day(t) + 4.0, 7.0) as u8
}

// 21.4.1.14 HourFromTime ( t ), https://tc39.es/ecma262/#sec-hourfromtime
pub fn hour_from_time(t: f64) -> u8 {
    if !t.is_finite() {
        return 0;
    }

    // 1. Return 𝔽(floor(ℝ(t / msPerHour)) modulo HoursPerDay).
    modulo((t / MS_PER_HOUR).floor(), HOURS_PER_DAY) as u8
}

// 21.4.1.15 MinFromTime ( t ), https://tc39.es/ecma262/#sec-minfromtime
pub fn min_from_time(t: f64) -> u8 {
    if !t.is_finite() {
        return 0;
    }

    // 1. Return 𝔽(floor(ℝ(t / msPerMinute)) modulo MinutesPerHour).
    modulo((t / MS_PER_MINUTE).floor(), MINUTES_PER_HOUR) as u8
}

// 21.4.1.16 SecFromTime ( t ), https://tc39.es/ecma262/#sec-secfromtime
pub fn sec_from_time(t: f64) -> u8 {
    if !t.is_finite() {
        return 0;
    }

    // 1. Return 𝔽(floor(ℝ(t / msPerSecond)) modulo SecondsPerMinute).
    modulo((t / MS_PER_SECOND).floor(), SECONDS_PER_MINUTE) as u8
}

// 21.4.1.17 msFromTime ( t ), https://tc39.es/ecma262/#sec-msfromtime
pub fn ms_from_time(t: f64) -> u16 {
    if !t.is_finite() {
        return 0;
    }

    // 1. Return 𝔽(ℝ(t) modulo ℝ(msPerSecond)).
    modulo(t, MS_PER_SECOND) as u16
}

// 21.4.1.18 GetUTCEpochNanoseconds ( year, month, day, hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/ecma262/#sec-getutcepochnanoseconds
// 14.5.1 GetUTCEpochNanoseconds ( isoDateTime ), https://tc39.es/proposal-temporal/#sec-getutcepochnanoseconds
pub fn get_utc_epoch_nanoseconds(iso_date_time: &ISODateTime) -> SignedBigInteger {
    // 1. Let date be MakeDay(𝔽(isoDateTime.[[ISODate]].[[Year]]), 𝔽(isoDateTime.[[ISODate]].[[Month]] - 1), 𝔽(isoDateTime.[[ISODate]].[[Day]])).
    let date = make_day(
        f64::from(iso_date_time.iso_date.year),
        f64::from(iso_date_time.iso_date.month) - 1.0,
        f64::from(iso_date_time.iso_date.day),
    );

    // 2. Let time be MakeTime(𝔽(isoDateTime.[[Time]].[[Hour]]), 𝔽(isoDateTime.[[Time]].[[Minute]]), 𝔽(isoDateTime.[[Time]].[[Second]]), 𝔽(isoDateTime.[[Time]].[[Millisecond]])).
    let time = make_time(
        f64::from(iso_date_time.time.hour),
        f64::from(iso_date_time.time.minute),
        f64::from(iso_date_time.time.second),
        f64::from(iso_date_time.time.millisecond),
    );

    // 3. Let ms be MakeDate(date, time).
    let ms = make_date(date, time);

    // 4. Assert: ms is an integral Number.
    assert!(ms == ms.trunc(), "ms == trunc(ms)");

    // 5. Return ℤ(ℝ(ms) × 10**6 + isoDateTime.[[Time]].[[Microsecond]] × 10**3 + isoDateTime.[[Time]].[[Nanosecond]]).
    let ms = SignedBigInteger::from_f64(ms).expect("an integral Number is an integer");
    ms * NANOSECONDS_PER_MILLISECOND
        + SignedBigInteger::from(iso_date_time.time.microsecond) * NANOSECONDS_PER_MICROSECOND
        + SignedBigInteger::from(iso_date_time.time.nanosecond)
}

pub fn clip_bigint_to_sane_time(value: &SignedBigInteger) -> i64 {
    // The provided epoch (nano)seconds value is potentially out of range for AK::Duration and subsequently
    // get_time_zone_offset(). We can safely assume that the TZDB has no useful information that far
    // into the past and future anyway, so clamp it to the i64 range.
    value.to_i64().unwrap_or(if value.sign() == num_bigint::Sign::Minus {
        i64::MIN
    } else {
        i64::MAX
    })
}

pub fn clip_double_to_sane_time(value: f64) -> i64 {
    const MIN_DOUBLE: f64 = i64::MIN as f64;
    const MAX_DOUBLE: f64 = i64::MAX as f64;

    // The provided epoch milliseconds value is potentially out of range for AK::Duration and subsequently
    // get_time_zone_offset(). We can safely assume that the TZDB has no useful information that far
    // into the past and future anyway, so clamp it to the i64 range.
    if value < MIN_DOUBLE {
        return i64::MIN;
    }
    if value > MAX_DOUBLE {
        return i64::MAX;
    }

    value as i64
}

// 21.4.1.20 GetNamedTimeZoneEpochNanoseconds ( timeZoneIdentifier, year, month, day, hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/ecma262/#sec-getnamedtimezoneepochnanoseconds
// 14.6.3 GetNamedTimeZoneEpochNanoseconds ( timeZoneIdentifier, isoDateTime ), https://tc39.es/proposal-temporal/#sec-getnamedtimezoneepochnanoseconds
pub fn get_named_time_zone_epoch_nanoseconds(
    time_zone_identifier: Utf16View<'_>,
    iso_date_time: &ISODateTime,
) -> Vec<SignedBigInteger> {
    let local_nanoseconds = get_utc_epoch_nanoseconds(iso_date_time);
    let local_time = UnixDateTime::from_nanoseconds_since_epoch(clip_bigint_to_sane_time(&local_nanoseconds));

    let offsets = unicode_time_zone::disambiguated_time_zone_offsets(time_zone_identifier, local_time);

    offsets
        .iter()
        .map(|offset| &local_nanoseconds - SignedBigInteger::from(offset.offset_nanoseconds))
        .collect()
}

// 21.4.1.21 GetNamedTimeZoneOffsetNanoseconds ( timeZoneIdentifier, epochNanoseconds ), https://tc39.es/ecma262/#sec-getnamedtimezoneoffsetnanoseconds
pub fn get_named_time_zone_offset_nanoseconds(
    time_zone_identifier: Utf16View<'_>,
    epoch_nanoseconds: &SignedBigInteger,
) -> UnicodeTimeZoneOffset {
    // Since UnixDateTime::from_seconds_since_epoch() and UnixDateTime::from_nanoseconds_since_epoch() both take an i64, converting to
    // seconds first gives us a greater range. The TZDB doesn't have sub-second offsets.
    let seconds = epoch_nanoseconds.div_floor(&SignedBigInteger::from(NANOSECONDS_PER_SECOND));
    let time = UnixDateTime::from_seconds_since_epoch(clip_bigint_to_sane_time(&seconds));

    unicode_time_zone::time_zone_offset(time_zone_identifier, time).expect("offset.has_value()")
}

// 21.4.1.21 GetNamedTimeZoneOffsetNanoseconds ( timeZoneIdentifier, epochNanoseconds ), https://tc39.es/ecma262/#sec-getnamedtimezoneoffsetnanoseconds
// OPTIMIZATION: This overload is provided to allow callers to avoid BigInt construction if they do not need infinitely precise nanosecond resolution.
pub fn get_named_time_zone_offset_milliseconds(
    time_zone_identifier: Utf16View<'_>,
    epoch_milliseconds: f64,
) -> UnicodeTimeZoneOffset {
    let seconds = epoch_milliseconds / 1000.0;
    let time = UnixDateTime::from_seconds_since_epoch(clip_double_to_sane_time(seconds));

    unicode_time_zone::time_zone_offset(time_zone_identifier, time).expect("offset.has_value()")
}

/// The cached SystemTimeZoneIdentifier(), as code units, so that it can be shared by the threads of the agents.
static CACHED_SYSTEM_TIME_ZONE_IDENTIFIER: Mutex<Option<Vec<u16>>> = Mutex::new(None);

// 21.4.1.24 SystemTimeZoneIdentifier ( ), https://tc39.es/ecma262/#sec-systemtimezoneidentifier
pub fn system_time_zone_identifier() -> Utf16String {
    // OPTIMIZATION: We cache the system time zone to avoid the expensive lookups below.
    if let Some(identifier) = CACHED_SYSTEM_TIME_ZONE_IDENTIFIER
        .lock()
        .expect("the time zone cache is never poisoned")
        .as_ref()
    {
        return Utf16String::from_utf16(identifier);
    }

    // 1. If the implementation only supports the UTC time zone, return "UTC".

    // 2. Let systemTimeZoneString be the String representing the host environment's current time zone, either a primary
    //    time zone identifier or an offset time zone identifier.
    let mut system_time_zone_string = unicode_time_zone::current_time_zone();

    if !is_offset_time_zone_identifier(Utf16View::of_string(&system_time_zone_string)) {
        let Some(time_zone_identifier) =
            get_available_named_time_zone_identifier(Utf16View::of_string(&system_time_zone_string))
        else {
            return Utf16String::from_utf8("UTC");
        };

        system_time_zone_string = time_zone_identifier.primary_identifier.clone();
    }

    // 3. Return systemTimeZoneString.
    *CACHED_SYSTEM_TIME_ZONE_IDENTIFIER
        .lock()
        .expect("the time zone cache is never poisoned") =
        Some(Utf16View::of_string(&system_time_zone_string).code_units().collect());
    system_time_zone_string
}

pub fn clear_system_time_zone_cache() {
    *CACHED_SYSTEM_TIME_ZONE_IDENTIFIER
        .lock()
        .expect("the time zone cache is never poisoned") = None;
}

// 21.4.1.25 LocalTime ( t ), https://tc39.es/ecma262/#sec-localtime
// 14.5.6 LocalTime ( t ), https://tc39.es/proposal-temporal/#sec-localtime
pub fn local_time(time: f64) -> f64 {
    // 1. Let systemTimeZoneIdentifier be SystemTimeZoneIdentifier().
    let system_time_zone_identifier = system_time_zone_identifier();

    // 2. Let parseResult be ! ParseTimeZoneIdentifier(systemTimeZoneIdentifier).
    let parse_result = parse_time_zone_identifier(Utf16View::of_string(&system_time_zone_identifier));

    // 3. If parseResult.[[OffsetMinutes]] is not EMPTY, then
    let offset_nanoseconds = if let Some(offset_minutes) = parse_result.offset_minutes {
        // a. Let offsetNs be parseResult.[[OffsetMinutes]] × (60 × 10**9).
        offset_minutes as f64 * 60_000_000_000.0
    }
    // 4. Else,
    else {
        // a. Let offsetNs be GetNamedTimeZoneOffsetNanoseconds(systemTimeZoneIdentifier, ℤ(ℝ(t) × 10^6)).
        let offset = get_named_time_zone_offset_milliseconds(Utf16View::of_string(&system_time_zone_identifier), time);
        offset.offset_nanoseconds as f64
    };

    // 5. Let offsetMs be truncate(offsetNs / 10^6).
    let offset_milliseconds = (offset_nanoseconds / 1e6).trunc();

    // 6. Return t + 𝔽(offsetMs).
    time + offset_milliseconds
}

// 21.4.1.26 UTC ( t ), https://tc39.es/ecma262/#sec-utc-t
// 14.6.7 UTC ( t ), https://tc39.es/proposal-temporal/#sec-utc-t
pub fn utc_time(time: f64) -> f64 {
    // 1. If t is not finite, return NaN.
    if !time.is_finite() {
        return f64::NAN;
    }

    if time.abs() > MAX_TIME_VALUE + MS_PER_DAY {
        return f64::NAN;
    }

    // 2. Let systemTimeZoneIdentifier be SystemTimeZoneIdentifier().
    let system_time_zone_identifier = system_time_zone_identifier();
    let system_time_zone_identifier = Utf16View::of_string(&system_time_zone_identifier);

    // 3. Let parseResult be ! ParseTimeZoneIdentifier(systemTimeZoneIdentifier).
    let parse_result = parse_time_zone_identifier(system_time_zone_identifier);

    // 4. If parseResult.[[OffsetMinutes]] is not EMPTY, then
    let offset_nanoseconds = if let Some(offset_minutes) = parse_result.offset_minutes {
        // a. Let offsetNs be parseResult.[[OffsetMinutes]] × (60 × 10**9).
        offset_minutes as f64 * 60_000_000_000.0
    }
    // 5. Else,
    else {
        // a. Let isoDateTime be TimeValueToISODateTimeRecord(t).
        let iso_date_time = time_value_to_iso_date_time_record(time);

        // b. Let possibleInstants be GetNamedTimeZoneEpochNanoseconds(systemTimeZoneIdentifier, isoDateTime).
        let possible_instants = get_named_time_zone_epoch_nanoseconds(system_time_zone_identifier, &iso_date_time);

        // c. NOTE: The following steps ensure that when t represents local time repeating multiple times at a negative
        //    time zone transition (e.g. when the daylight saving time ends or the time zone offset is decreased due to
        //    a time zone rule change) or skipped local time at a positive time zone transition (e.g. when the daylight
        //    saving time starts or the time zone offset is increased due to a time zone rule change), t is interpreted
        //    using the time zone offset before the transition.
        let disambiguated_instant = if let Some(first) = possible_instants.into_iter().next() {
            // d. If possibleInstants is not empty, then
            //     i. Let disambiguatedInstant be possibleInstants[0].
            first
        }
        // e. Else,
        else {
            // i. NOTE: t represents a local time skipped at a positive time zone transition (e.g. due to daylight
            //    saving time starting or a time zone rule change increasing the UTC offset).

            // ii. Let possibleInstantsBefore be GetNamedTimeZoneEpochNanoseconds(systemTimeZoneIdentifier, TimeValueToISODateTimeRecord(tBefore)),
            //     where tBefore is the largest integral Number < t for which possibleInstantsBefore is not empty (i.e.,
            //     tBefore represents the last local time before the transition).
            // NB: We implement this by finding the next UTC offset transition after one day before the skipped time,
            //     which is guaranteed to be before the gap. The last valid instant before the transition is one
            //     nanosecond before the transition instant.
            let epoch_nanoseconds = get_utc_epoch_nanoseconds(&iso_date_time);
            let day_before = epoch_nanoseconds - SignedBigInteger::from(NANOSECONDS_PER_DAY);
            let transition = get_named_time_zone_next_transition(system_time_zone_identifier, &day_before)
                .expect("transition.has_value()");

            // iii. Let disambiguatedInstant be the last element of possibleInstantsBefore.
            transition - 1
        };

        // f. Let offsetNs be GetNamedTimeZoneOffsetNanoseconds(systemTimeZoneIdentifier, disambiguatedInstant).
        let offset = get_named_time_zone_offset_nanoseconds(system_time_zone_identifier, &disambiguated_instant);
        offset.offset_nanoseconds as f64
    };

    // 6. Let offsetMs be truncate(offsetNs / 10^6).
    let offset_milliseconds = (offset_nanoseconds / 1e6).trunc();

    // 7. Return t - 𝔽(offsetMs).
    time - offset_milliseconds
}

// 21.4.1.27 MakeTime ( hour, min, sec, ms ), https://tc39.es/ecma262/#sec-maketime
pub fn make_time(hour: f64, min: f64, sec: f64, ms: f64) -> f64 {
    // 1. If hour is not finite or min is not finite or sec is not finite or ms is not finite, return NaN.
    if !hour.is_finite() || !min.is_finite() || !sec.is_finite() || !ms.is_finite() {
        return f64::NAN;
    }

    // 2. Let h be 𝔽(! ToIntegerOrInfinity(hour)).
    let h = to_integer_or_infinity(hour);
    // 3. Let m be 𝔽(! ToIntegerOrInfinity(min)).
    let m = to_integer_or_infinity(min);
    // 4. Let s be 𝔽(! ToIntegerOrInfinity(sec)).
    let s = to_integer_or_infinity(sec);
    // 5. Let milli be 𝔽(! ToIntegerOrInfinity(ms)).
    let milli = to_integer_or_infinity(ms);
    // 6. Let t be ((h * msPerHour + m * msPerMinute) + s * msPerSecond) + milli, performing the arithmetic according to IEEE 754-2019 rules (that is, as if using the ECMAScript operators * and +).
    // NOTE: Rust arithmetic abides by IEEE 754 rules
    // 7. Return t.
    ((h * MS_PER_HOUR + m * MS_PER_MINUTE) + s * MS_PER_SECOND) + milli
}

/// AK::is_within_range<int>() of a double: whether it is an integer in the range of an i32.
fn is_within_i32_range(value: f64) -> bool {
    const BOUNDARY: f64 = 2_147_483_648.0;
    (-BOUNDARY..BOUNDARY).contains(&value) && f64::from(value as i32) == value
}

// Integer division rounding towards negative infinity, as AK::Detail::floor_div_by does.
fn floor_div_by(dividend: i64, divisor: i64) -> i64 {
    let is_negative = i64::from(dividend < 0);
    (dividend + is_negative) / divisor - is_negative
}

// Counts how many integers n are in the interval [begin, end) with n % positive_mod == 0, as
// AK::Detail::mod_zeros_in_range does.
fn mod_zeros_in_range(begin: i64, end: i64, positive_mod: i64) -> i64 {
    floor_div_by(end - 1, positive_mod) - floor_div_by(begin - 1, positive_mod)
}

/// AK::is_leap_year.
fn is_leap_year(year: i32) -> bool {
    year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)
}

/// AK::years_to_days_since_epoch.
fn years_to_days_since_epoch(year: i32) -> i64 {
    let (begin_year, end_year, leap_sign) = if year < 1970 {
        (i64::from(year), 1970, -1)
    } else {
        (1970, i64::from(year), 1)
    };
    let days = 365 * (i64::from(year) - 1970);
    let mut extra_leap_days = 0;
    extra_leap_days += mod_zeros_in_range(begin_year, end_year, 4);
    extra_leap_days -= mod_zeros_in_range(begin_year, end_year, 100);
    extra_leap_days += mod_zeros_in_range(begin_year, end_year, 400);
    days + extra_leap_days * leap_sign
}

/// AK::day_of_year, with a month in [1, 12].
fn day_of_year(year: i32, month: i32, day: i32) -> i64 {
    const SEEK_TABLE: [i64; 12] = [0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334];

    if !(1..=12).contains(&month) {
        return 0;
    }

    let mut day_of_year = SEEK_TABLE[(month - 1) as usize] + i64::from(day) - 1;
    if is_leap_year(year) && month >= 3 {
        day_of_year += 1;
    }
    day_of_year
}

/// AK::days_since_epoch.
pub(crate) fn days_since_epoch(year: i32, month: i32, day: i32) -> i64 {
    years_to_days_since_epoch(year) + day_of_year(year, month, day)
}

// 21.4.1.28 MakeDay ( year, month, date ), https://tc39.es/ecma262/#sec-makeday
pub fn make_day(year: f64, month: f64, date: f64) -> f64 {
    // 1. If year is not finite or month is not finite or date is not finite, return NaN.
    if !year.is_finite() || !month.is_finite() || !date.is_finite() {
        return f64::NAN;
    }

    // 2. Let y be 𝔽(! ToIntegerOrInfinity(year)).
    let y = to_integer_or_infinity(year);
    // 3. Let m be 𝔽(! ToIntegerOrInfinity(month)).
    let m = to_integer_or_infinity(month);
    // 4. Let dt be 𝔽(! ToIntegerOrInfinity(date)).
    let dt = to_integer_or_infinity(date);
    // 5. Let ym be y + 𝔽(floor(ℝ(m) / 12)).
    let ym = y + (m / 12.0).floor();
    // 6. If ym is not finite, return NaN.
    if !ym.is_finite() {
        return f64::NAN;
    }
    // 7. Let mn be 𝔽(ℝ(m) modulo 12).
    let mn = modulo(m, 12.0);

    // 8. Find a finite time value t such that YearFromTime(t) is ym and MonthFromTime(t) is mn and DateFromTime(t) is 1𝔽; but if this is not possible (because some argument is out of range), return NaN.
    if !is_within_i32_range(ym) || !is_within_i32_range(mn + 1.0) {
        return f64::NAN;
    }
    let t = days_since_epoch(ym as i32, mn as i32 + 1, 1) as f64 * MS_PER_DAY;

    // 9. Return Day(t) + dt - 1𝔽.
    day(t) + dt - 1.0
}

// 21.4.1.29 MakeDate ( day, time ), https://tc39.es/ecma262/#sec-makedate
pub fn make_date(day: f64, time: f64) -> f64 {
    // 1. If day is not finite or time is not finite, return NaN.
    if !day.is_finite() || !time.is_finite() {
        return f64::NAN;
    }

    // 2. Let tv be day × msPerDay + time.
    let tv = day * MS_PER_DAY + time;

    // 3. If tv is not finite, return NaN.
    if !tv.is_finite() {
        return f64::NAN;
    }

    // 4. Return tv.
    tv
}

// 21.4.1.31 TimeClip ( time ), https://tc39.es/ecma262/#sec-timeclip
pub fn time_clip(time: f64) -> f64 {
    // 1. If time is not finite, return NaN.
    if !time.is_finite() {
        return f64::NAN;
    }

    // 2. If abs(ℝ(time)) > 8.64 × 10^15, return NaN.
    if time.abs() > 8.64E15 {
        return f64::NAN;
    }

    // 3. Return 𝔽(! ToIntegerOrInfinity(time)).
    to_integer_or_infinity(time)
}

// 21.4.1.33.1 IsTimeZoneOffsetString ( offsetString ), https://tc39.es/ecma262/#sec-istimezoneoffsetstring
// 14.5.10 IsOffsetTimeZoneIdentifier ( offsetString ), https://tc39.es/proposal-temporal/#sec-isoffsettimezoneidentifier
pub fn is_offset_time_zone_identifier(offset_string: Utf16View<'_>) -> bool {
    // 1. Let parseResult be ParseText(StringToCodePoints(offsetString), UTCOffset[~SubMinutePrecision]).
    let parse_result = parse_utc_offset(offset_string, SubMinutePrecision::No);

    // 2. If parseResult is a List of errors, return false.
    // 3. Return true.
    parse_result.is_some()
}

// 21.4.1.33.2 ParseTimeZoneOffsetString ( offsetString ), https://tc39.es/ecma262/#sec-parsetimezoneoffsetstring
// 14.5.11 ParseDateTimeUTCOffset ( offsetString ), https://tc39.es/proposal-temporal/#sec-parsedatetimeutcoffset
pub fn parse_date_time_utc_offset_or_throw(vm: &Vm, offset_string: Utf16View<'_>) -> ThrowCompletionOr<f64> {
    // 1. Let parseResult be ParseText(offsetString, UTCOffset[+SubMinutePrecision]).
    let Some(parse_result) = parse_utc_offset(offset_string, SubMinutePrecision::Yes) else {
        // 2. If parseResult is a List of errors, throw a RangeError exception.
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TemporalInvalidTimeZoneString,
            &[&offset_string.to_utf8()],
        );
    };

    Ok(parse_date_time_utc_offset_from_parse_result(&parse_result))
}

// 21.4.1.33.2 ParseTimeZoneOffsetString ( offsetString ), https://tc39.es/ecma262/#sec-parsetimezoneoffsetstring
// 14.5.11 ParseDateTimeUTCOffset ( offsetString ), https://tc39.es/proposal-temporal/#sec-parsedatetimeutcoffset
pub fn parse_date_time_utc_offset(offset_string: Utf16View<'_>) -> f64 {
    // OPTIMIZATION: Some callers can assume that parsing will succeed.

    // 1. Let parseResult be ParseText(offsetString, UTCOffset[+SubMinutePrecision]).
    let parse_result = parse_utc_offset(offset_string, SubMinutePrecision::Yes).expect("parse_result.has_value()");

    parse_date_time_utc_offset_from_parse_result(&parse_result)
}

/// The value of a parse node of decimal digits, as Utf16View::to_number<u8>() computes it.
fn decimal_digits_value(digits: Utf16View<'_>) -> f64 {
    digits.code_units().fold(0.0, |value, code_unit| {
        value * 10.0 + f64::from(code_unit - u16::from(b'0'))
    })
}

// 21.4.1.33.2 ParseTimeZoneOffsetString ( offsetString ), https://tc39.es/ecma262/#sec-parsetimezoneoffsetstring
// 14.5.11 ParseDateTimeUTCOffset ( offsetString ), https://tc39.es/proposal-temporal/#sec-parsedatetimeutcoffset
pub fn parse_date_time_utc_offset_from_parse_result(parse_result: &TimeZoneOffset<'_>) -> f64 {
    // OPTIMIZATION: Some callers will have already parsed and validated the time zone identifier.

    // 3. Assert: parseResult contains a ASCIISign Parse Node.
    let parsed_sign = parse_result.sign.expect("parse_result.sign.has_value()");

    // 4. Let parsedSign be the source text matched by the ASCIISign Parse Node contained within parseResult.
    // 5. If parsedSign is the single code point U+002D (HYPHEN-MINUS), then
    //     a. Let sign be -1.
    // 6. Else,
    //     a. Let sign be 1.
    let sign = if parsed_sign == '-' { -1.0 } else { 1.0 };

    // 7. NOTE: Applications of StringToNumber below do not lose precision, since each of the parsed values is guaranteed
    //    to be a sufficiently short string of decimal digits.

    // 8. Assert: parseResult contains an Hour Parse Node.
    // 9. Let parsedHours be the source text matched by the Hour Parse Node contained within parseResult.
    // 10. Let hours be ℝ(StringToNumber(CodePointsToString(parsedHours))).
    let hours = decimal_digits_value(parse_result.hours.expect("parse_result.hours.has_value()"));

    // 11. If parseResult does not contain a MinuteSecond Parse Node, then
    //     a. Let minutes be 0.
    // 12. Else,
    //     a. Let parsedMinutes be the source text matched by the first MinuteSecond Parse Node contained within parseResult.
    //     b. Let minutes be ℝ(StringToNumber(CodePointsToString(parsedMinutes))).
    let minutes = parse_result.minutes.map_or(0.0, decimal_digits_value);

    // 13. If parseResult does not contain two MinuteSecond Parse Nodes, then
    //     a. Let seconds be 0.
    // 14. Else,
    //     a. Let parsedSeconds be the source text matched by the second secondSecond Parse Node contained within parseResult.
    //     b. Let seconds be ℝ(StringToNumber(CodePointsToString(parsedSeconds))).
    let seconds = parse_result.seconds.map_or(0.0, decimal_digits_value);

    let mut nanoseconds = 0.0;

    // 15. If parseResult does not contain a TemporalDecimalFraction Parse Node, then
    //     a. Let nanoseconds be 0.
    // 16. Else,
    if let Some(parsed_fraction) = parse_result.fraction {
        // a. Let parsedFraction be the source text matched by the TemporalDecimalFraction Parse Node contained within parseResult.
        // b. Let fraction be the string-concatenation of CodePointsToString(parsedFraction) and "000000000".
        // c. Let nanosecondsString be the substring of fraction from 1 to 10.
        // d. Let nanoseconds be ℝ(StringToNumber(nanosecondsString)).
        for i in 1..10 {
            nanoseconds *= 10.0;
            if i < parsed_fraction.length_in_code_units() {
                nanoseconds += f64::from(parsed_fraction.code_unit_at(i) - u16::from(b'0'));
            }
        }
    }

    // 17. Return sign × (((hours × 60 + minutes) × 60 + seconds) × 10^9 + nanoseconds).
    sign * (((hours * 60.0 + minutes) * 60.0 + seconds) * 1e9 + nanoseconds)
}

// NB: The rest of this file stands in for the operations of the Temporal and Intl namespaces that Date.cpp calls,
//     with the C++ names, for what Date needs of them, until the runtime has those namespaces.

pub const NANOSECONDS_PER_SECOND: i64 = 1_000_000_000;
pub const NANOSECONDS_PER_MILLISECOND: i64 = 1_000_000;
pub const NANOSECONDS_PER_MICROSECOND: i64 = 1_000;
pub const NANOSECONDS_PER_DAY: i64 = 86_400_000_000_000;

// 3.5.1 ISO Date Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-date-records
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ISODate {
    pub year: i32,
    pub month: u8,
    pub day: u8,
}

// 4.5.1 Time Records, https://tc39.es/proposal-temporal/#sec-temporal-time-records
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Time {
    pub days: f64,
    pub hour: u8,
    pub minute: u8,
    pub second: u8,
    pub millisecond: u16,
    pub microsecond: u16,
    pub nanosecond: u16,
}

// 5.5.1 ISO Date-Time Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-date-time-records
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ISODateTime {
    pub iso_date: ISODate,
    pub time: Time,
}

// 5.5.2 TimeValueToISODateTimeRecord ( t ), https://tc39.es/proposal-temporal/#sec-temporal-timevaluetoisodatetimerecord
pub fn time_value_to_iso_date_time_record(time_value: f64) -> ISODateTime {
    // 1. Let isoDate be CreateISODateRecord(ℝ(YearFromTime(t)), ℝ(MonthFromTime(t)) + 1, ℝ(DateFromTime(t))).
    let iso_date = ISODate {
        year: year_from_time(time_value),
        month: month_from_time(time_value) + 1,
        day: date_from_time(time_value),
    };
    assert!(
        (1..=12).contains(&iso_date.month) && (1..=31).contains(&iso_date.day),
        "is_valid_iso_date(year, month, day)"
    );

    // 2. Let time be CreateTimeRecord(ℝ(HourFromTime(t)), ℝ(MinFromTime(t)), ℝ(SecFromTime(t)), ℝ(msFromTime(t)), 0, 0).
    let time = Time {
        days: 0.0,
        hour: hour_from_time(time_value),
        minute: min_from_time(time_value),
        second: sec_from_time(time_value),
        millisecond: ms_from_time(time_value),
        microsecond: 0,
        nanosecond: 0,
    };

    // 3. Return ISO Date-Time Record { [[ISODate]]: isoDate, [[Time]]: time }.
    ISODateTime { iso_date, time }
}

// 11.1.3 GetNamedTimeZoneNextTransition ( timeZoneIdentifier, epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-getnamedtimezonenexttransition
pub fn get_named_time_zone_next_transition(
    time_zone: Utf16View<'_>,
    epoch_nanoseconds: &SignedBigInteger,
) -> Option<SignedBigInteger> {
    let epoch_milliseconds = epoch_nanoseconds.div_floor(&SignedBigInteger::from(NANOSECONDS_PER_MILLISECOND));
    let time = UnixDateTime::from_milliseconds_since_epoch(clip_bigint_to_sane_time(&epoch_milliseconds));

    let options = TimeZoneTransitionOptions {
        direction: TransitionDirection::Next,
        include_given_time: IncludeGivenTime::No,
        transition_rule: TransitionRule::TransitionWhereUTCOffsetChanges,
    };
    let transition_milliseconds = unicode_time_zone::get_time_zone_transition(time_zone, time, options)?;

    let result_nanoseconds = SignedBigInteger::from(transition_milliseconds) * NANOSECONDS_PER_MILLISECOND;
    // nsMaxInstant = 10**8 × nsPerDay = 8.64 × 10**21
    if result_nanoseconds > SignedBigInteger::from(NANOSECONDS_PER_DAY) * 100_000_000 {
        return None;
    }

    Some(result_nanoseconds)
}

// https://tc39.es/proposal-temporal/#sec-temporal-parsetimezoneidentifier, the Time Zone Identifier Parse Record.
#[derive(Clone)]
pub struct ParsedTimeZoneIdentifier {
    pub name: Option<Utf16String>,
    pub offset_minutes: Option<i64>,
}

// 11.1.16 ParseTimeZoneIdentifier ( identifier ), https://tc39.es/proposal-temporal/#sec-parsetimezoneidentifier
/// For an identifier that is known to parse, as the system time zone identifier is: an offset time zone identifier, or
/// else the IANA name of an available time zone.
pub fn parse_time_zone_identifier(identifier: Utf16View<'_>) -> ParsedTimeZoneIdentifier {
    // 1. Let parseResult be ParseText(StringToCodePoints(identifier), TimeZoneIdentifier).
    let Some(time_zone_offset) = parse_utc_offset(identifier, SubMinutePrecision::No) else {
        // 3. If parseResult contains a TimeZoneIANAName Parse Node, then
        //     a. Let name be the source text matched by the TimeZoneIANAName Parse Node contained within parseResult.
        //     b. NOTE: name is syntactically valid, but does not necessarily conform to IANA Time Zone Database naming
        //        guidelines or correspond with an available named time zone identifier.
        //     c. Return Time Zone Identifier Parse Record { [[Name]]: CodePointsToString(name), [[OffsetMinutes]]: EMPTY }.
        return ParsedTimeZoneIdentifier {
            name: Some(identifier.to_utf16_string()),
            offset_minutes: None,
        };
    };

    // 4. Assert: parseResult contains a UTCOffset[~SubMinutePrecision] Parse Node.
    // 5. Let offset be the source text matched by the UTCOffset[~SubMinutePrecision] Parse Node contained within parseResult.
    // 6. Let offsetNanoseconds be ! ParseDateTimeUTCOffset(CodePointsToString(offset)).
    let offset_nanoseconds = parse_date_time_utc_offset_from_parse_result(&time_zone_offset);

    // 7. Let offsetMinutes be offsetNanoseconds / (60 × 10**9).
    let offset_minutes = offset_nanoseconds / 60_000_000_000.0;

    // 8. Return Time Zone Identifier Parse Record { [[Name]]: empty, [[OffsetMinutes]]: offsetMinutes }.
    ParsedTimeZoneIdentifier {
        name: None,
        offset_minutes: Some(offset_minutes as i64),
    }
}

/// Temporal::SubMinutePrecision, the parameter of the UTCOffset production.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SubMinutePrecision {
    No,
    Yes,
}

/// Temporal::TimeZoneOffset, the parse nodes of a UTCOffset.
#[derive(Clone, Copy, Debug)]
pub struct TimeZoneOffset<'a> {
    pub sign: Option<char>,
    pub hours: Option<Utf16View<'a>>,
    pub minutes: Option<Utf16View<'a>>,
    pub seconds: Option<Utf16View<'a>>,
    pub fraction: Option<Utf16View<'a>>,
    pub source_text: Utf16View<'a>,
}

/// The productions of ISO8601Parser that UTCOffset is made of, each of which consumes nothing when it fails.
struct UtcOffsetParser<'a> {
    input: Utf16View<'a>,
    position: usize,
}

impl<'a> UtcOffsetParser<'a> {
    fn peek(&self, offset: usize) -> Option<u16> {
        let index = self.position + offset;
        (index < self.input.length_in_code_units()).then(|| self.input.code_unit_at(index))
    }

    fn consume_specific(&mut self, expected: u8) -> bool {
        if self.peek(0) != Some(u16::from(expected)) {
            return false;
        }
        self.position += 1;
        true
    }

    fn consume_specific_pair(&mut self, first: u8, second: u8) -> bool {
        if self.peek(0) != Some(u16::from(first)) || self.peek(1) != Some(u16::from(second)) {
            return false;
        }
        self.position += 2;
        true
    }

    /// ISO8601Parser::scoped_parse: the source text `parse` matches, or nothing consumed if it does not match.
    fn scoped_parse(&mut self, parse: impl FnOnce(&mut Self) -> bool) -> Option<Utf16View<'a>> {
        let start = self.position;
        if !parse(self) {
            self.position = start;
            return None;
        }
        Some(self.input.substring_view(start, self.position - start))
    }

    // https://tc39.es/ecma262/#prod-DecimalDigit
    fn parse_decimal_digit(&mut self) -> bool {
        // DecimalDigit : one of
        //     0 1 2 3 4 5 6 7 8 9
        if self
            .peek(0)
            .is_some_and(|code_unit| (u16::from(b'0')..=u16::from(b'9')).contains(&code_unit))
        {
            self.position += 1;
            return true;
        }
        false
    }

    // https://tc39.es/proposal-temporal/#prod-ASCIISign
    fn parse_ascii_sign(&mut self) -> bool {
        // ASCIISign : one of
        //     + -
        self.consume_specific(b'+') || self.consume_specific(b'-')
    }

    // https://tc39.es/ecma262/#prod-Hour
    fn parse_hour(&mut self) -> bool {
        // Hour :::
        //     0 DecimalDigit
        //     1 DecimalDigit
        //     20
        //     21
        //     22
        //     23
        if self.consume_specific(b'0') || self.consume_specific(b'1') {
            if !self.parse_decimal_digit() {
                return false;
            }
        } else {
            let success = self.consume_specific_pair(b'2', b'0')
                || self.consume_specific_pair(b'2', b'1')
                || self.consume_specific_pair(b'2', b'2')
                || self.consume_specific_pair(b'2', b'3');
            if !success {
                return false;
            }
        }

        true
    }

    // https://tc39.es/ecma262/#prod-MinuteSecond
    fn parse_minute_second(&mut self) -> bool {
        // MinuteSecond :::
        //     0 DecimalDigit
        //     1 DecimalDigit
        //     2 DecimalDigit
        //     3 DecimalDigit
        //     4 DecimalDigit
        //     5 DecimalDigit
        let success = (b'0'..=b'5').any(|first_digit| self.consume_specific(first_digit));
        if !success {
            return false;
        }
        if !self.parse_decimal_digit() {
            return false;
        }

        true
    }

    // https://tc39.es/ecma262/#prod-TemporalDecimalSeparator
    fn parse_temporal_decimal_separator(&mut self) -> bool {
        // TemporalDecimalSeparator ::: one of
        //    . ,
        self.consume_specific(b'.') || self.consume_specific(b',')
    }

    // https://tc39.es/proposal-temporal/#prod-TemporalDecimalFraction
    fn parse_temporal_decimal_fraction(&mut self) -> bool {
        // TemporalDecimalFraction :::
        //     TemporalDecimalSeparator DecimalDigit
        //     [...] up to nine DecimalDigits
        if !self.parse_temporal_decimal_separator() {
            return false;
        }
        if !self.parse_decimal_digit() {
            return false;
        }

        for _ in 0..8 {
            if !self.parse_decimal_digit() {
                break;
            }
        }

        true
    }

    // https://tc39.es/ecma262/#prod-TimeSeparator
    fn parse_extended_time_separator(&mut self) -> bool {
        // TimeSeparator[Extended] :::
        //     [+Extended] :
        //     [~Extended] [empty]
        self.consume_specific(b':')
    }

    // https://tc39.es/proposal-temporal/#prod-UTCOffset
    fn parse_utc_offset(&mut self, sub_minute_precision: SubMinutePrecision) -> Option<TimeZoneOffset<'a>> {
        let start = self.position;
        let mut time_zone_offset = TimeZoneOffset {
            sign: None,
            hours: None,
            minutes: None,
            seconds: None,
            fraction: None,
            source_text: self.input.substring_view(start, 0),
        };

        let mut parse = |parser: &mut Self| -> bool {
            // UTCOffset[SubMinutePrecision] :::
            //     ASCIISign Hour
            //     ASCIISign Hour TimeSeparator[+Extended] MinuteSecond
            //     ASCIISign Hour TimeSeparator[~Extended] MinuteSecond
            //     [+SubMinutePrecision] ASCIISign Hour TimeSeparator[+Extended] MinuteSecond TimeSeparator[+Extended] MinuteSecond TemporalDecimalFraction[opt]
            //     [+SubMinutePrecision] ASCIISign Hour TimeSeparator[~Extended] MinuteSecond TimeSeparator[~Extended] MinuteSecond TemporalDecimalFraction[opt]
            let Some(sign) = parser.scoped_parse(Self::parse_ascii_sign) else {
                return false;
            };
            time_zone_offset.sign = Some(if sign.code_unit_at(0) == u16::from(b'-') {
                '-'
            } else {
                '+'
            });
            time_zone_offset.hours = parser.scoped_parse(Self::parse_hour);
            if time_zone_offset.hours.is_none() {
                return false;
            }

            if parser.parse_extended_time_separator() {
                time_zone_offset.minutes = parser.scoped_parse(Self::parse_minute_second);
                if time_zone_offset.minutes.is_none() {
                    return false;
                }

                if sub_minute_precision == SubMinutePrecision::Yes && parser.parse_extended_time_separator() {
                    time_zone_offset.seconds = parser.scoped_parse(Self::parse_minute_second);
                    if time_zone_offset.seconds.is_none() {
                        return false;
                    }

                    time_zone_offset.fraction = parser.scoped_parse(Self::parse_temporal_decimal_fraction);
                }
            } else if let Some(minutes) = parser.scoped_parse(Self::parse_minute_second) {
                time_zone_offset.minutes = Some(minutes);
                if sub_minute_precision == SubMinutePrecision::Yes
                    && let Some(seconds) = parser.scoped_parse(Self::parse_minute_second)
                {
                    time_zone_offset.seconds = Some(seconds);
                    time_zone_offset.fraction = parser.scoped_parse(Self::parse_temporal_decimal_fraction);
                }
            }

            true
        };

        if !parse(self) {
            self.position = start;
            return None;
        }

        time_zone_offset.source_text = self.input.substring_view(start, self.position - start);
        Some(time_zone_offset)
    }
}

// https://tc39.es/proposal-temporal/#prod-UTCOffset
pub fn parse_utc_offset(input: Utf16View<'_>, sub_minute_precision: SubMinutePrecision) -> Option<TimeZoneOffset<'_>> {
    let mut parser = UtcOffsetParser { input, position: 0 };

    let utc_offset = parser.parse_utc_offset(sub_minute_precision)?;

    // If we parsed successfully but didn't reach the end, the string doesn't match the given production.
    if parser.position != input.length_in_code_units() {
        return None;
    }

    Some(utc_offset)
}

/// Temporal::TimeStyle.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TimeStyle {
    Separated,
    Unseparated,
}

// 13.26 FormatTimeString ( hour, minute, second, subSecondNanoseconds, precision [ , style ] ), https://tc39.es/proposal-temporal/#sec-temporal-formattimestring
/// With a precision of MINUTE, or else of 0 digits and no sub-second nanoseconds: the only ones Date formats with.
pub fn format_time_string(hour: u8, minute: u8, second: Option<u8>, style: Option<TimeStyle>) -> String {
    // 1. If style is present and style is UNSEPARATED, let separator be the empty String; else, let separator be ":".
    let separator = if style == Some(TimeStyle::Unseparated) { "" } else { ":" };

    // 2. Let hh be ToZeroPaddedDecimalString(hour, 2).
    // 3. Let mm be ToZeroPaddedDecimalString(minute, 2).

    // 4. If precision is minute, return the string-concatenation of hh, separator, and mm.
    let Some(second) = second else {
        return format!("{hour:02}{separator}{minute:02}");
    };

    // 5. Let ss be ToZeroPaddedDecimalString(second, 2).
    // 6. Let subSecondsPart be FormatFractionalSeconds(subSecondNanoseconds, precision).
    // 7. Return the string-concatenation of hh, separator, mm, separator, ss, and subSecondsPart.
    format!("{hour:02}{separator}{minute:02}{separator}{second:02}")
}

// 11.1.5 FormatOffsetTimeZoneIdentifier ( offsetMinutes [ , style ] ), https://tc39.es/proposal-temporal/#sec-temporal-formatoffsettimezoneidentifier
pub fn format_offset_time_zone_identifier(offset_minutes: i64, style: Option<TimeStyle>) -> String {
    // 1. If offsetMinutes ≥ 0, let sign be the code unit 0x002B (PLUS SIGN); else, let sign be the code unit 0x002D (HYPHEN-MINUS).
    let sign = if offset_minutes >= 0 { '+' } else { '-' };

    // 2. Let absoluteMinutes be abs(offsetMinutes).
    let absolute_minutes = offset_minutes.unsigned_abs() as f64;

    // 3. Let hour be floor(absoluteMinutes / 60).
    let hour = (absolute_minutes / 60.0).floor() as u8;

    // 4. Let minute be absoluteMinutes modulo 60.
    let minute = modulo(absolute_minutes, 60.0) as u8;

    // 5. Let timeString be FormatTimeString(hour, minute, 0, 0, MINUTE, style).
    let time_string = format_time_string(hour, minute, None, style);

    // 6. Return the string-concatenation of sign and timeString.
    format!("{sign}{time_string}")
}

// 2.3.2 SystemUTCEpochMilliseconds ( ), https://tc39.es/proposal-temporal/#sec-temporal-systemutcepochmilliseconds
pub fn system_utc_epoch_milliseconds() -> f64 {
    // 1. Let global be GetGlobalObject().
    // 2. Let nowNs be HostSystemUTCEpochNanoseconds(global).
    // AD-HOC: Every caller of SystemUTCEpochMilliseconds is via Date.now and the Date constructor, which do not need
    //         nanosecond precision. We can avoid unnecessary bigint math by returning milliseconds directly.
    let now = std::time::SystemTime::now();
    let now_ns: i64 = match now.duration_since(std::time::UNIX_EPOCH) {
        Ok(since_epoch) => i64::try_from(since_epoch.as_nanos()).unwrap_or(i64::MAX),
        Err(before_epoch) => {
            i64::try_from(before_epoch.duration().as_nanos()).map_or(i64::MIN, |nanoseconds| -nanoseconds)
        }
    };

    // 3. Return 𝔽(floor(nowNs / 10**6)).
    (now_ns / 1_000_000) as f64
}

// 6.5.1 AvailableNamedTimeZoneIdentifiers ( ), https://tc39.es/ecma402/#sup-availablenamedtimezoneidentifiers
