/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Time, ISO Date-Time, ISO Year-Month and parse records of Temporal.

use ak::Utf16String;

use crate::runtime::big_int::SignedBigInteger;

// 3.5.1 ISO Date Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-date-records
pub use crate::unicode::calendar::ISODate;

// 4.5.1 Time Records, https://tc39.es/proposal-temporal/#sec-temporal-time-records
#[derive(Clone, Copy, Debug, Default, PartialEq)]
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
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ISODateTime {
    pub iso_date: ISODate,
    pub time: Time,
}

// 7.5.3 Internal Duration Records, https://tc39.es/proposal-temporal/#sec-temporal-internal-duration-records
// A time duration is an integer in the inclusive interval from -maxTimeDuration to maxTimeDuration, where
// maxTimeDuration = 2**53 × 10**9 - 1 = 9,007,199,254,740,991,999,999,999. It represents the portion of a
// Temporal.Duration object that deals with time units, but as a combined value of total nanoseconds.
pub type TimeDuration = SignedBigInteger;

// 9.5.1 ISO Year-Month Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-year-month-records
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ISOYearMonth {
    pub year: i32,
    pub month: u8,
}

// 13.32 ISO String Time Zone Parse Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-string-time-zone-parse-records
#[derive(Clone, Default)]
pub struct ParsedISOTimeZone {
    pub z_designator: bool,
    pub offset_string: Option<Utf16String>,
    pub time_zone_annotation: Option<Utf16String>,
}

// 13.33 Time Zone Identifier Parse Records, https://tc39.es/proposal-temporal/#sec-temporal-time-zone-identifier-parse-records
#[derive(Clone, Default)]
pub struct ParsedTimeZoneIdentifier {
    pub name: Option<Utf16String>,
    pub offset_minutes: Option<i64>,
}

/// The [[Time]] of an ISO Date-Time Parse Record: START-OF-DAY, or a Time Record.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum TimeOrStartOfDay {
    StartOfDay,
    Time(Time),
}

// 13.34 ISO Date-Time Parse Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-date-time-parse-records
#[derive(Clone)]
pub struct ParsedISODateTime {
    pub year: Option<i32>,
    pub month: u8,
    pub day: u8,
    pub time: TimeOrStartOfDay,
    pub time_zone: ParsedISOTimeZone,
    pub calendar: Option<Utf16String>,
}
