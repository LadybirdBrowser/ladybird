/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The calendars of LibUnicode/Calendar.h, through the C exports of LibUnicode/TextMapping.h, so that Temporal
//! converts between ISO dates and calendar dates with the same ICU data as the C++ runtime.

use ak::Utf16String;

use super::{UnicodeTextMappingOutput, collect_text};
use crate::utf16::Utf16View;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct UnicodeISODate {
    year: i32,
    month: u8,
    day: u8,
}

#[repr(C)]
#[derive(Default)]
struct UnicodeCalendarDate {
    has_era: bool,
    has_era_year: bool,
    era_year: i32,
    year: i32,
    month: u8,
    day: u8,
    day_of_week: u8,
    day_of_year: u16,
    has_week_of_year: bool,
    week_of_year: u8,
    has_year_of_week: bool,
    year_of_week: i32,
    days_in_week: u8,
    days_in_month: u8,
    days_in_year: u16,
    months_in_year: u8,
    in_leap_year: bool,
}

unsafe extern "C" {
    fn unicode_parse_month_code(
        month_code: *const u16,
        length: usize,
        month_number: *mut u8,
        is_leap_month: *mut bool,
    ) -> bool;
    fn unicode_create_month_code(month_number: u8, is_leap_month: bool, output: UnicodeTextMappingOutput);
    fn unicode_iso_date_to_calendar_date(
        calendar: *const u8,
        calendar_length: usize,
        iso_date: UnicodeISODate,
        calendar_date: *mut UnicodeCalendarDate,
        era: UnicodeTextMappingOutput,
        month_code: UnicodeTextMappingOutput,
    );
    fn unicode_calendar_date_to_iso_date(
        calendar: *const u8,
        calendar_length: usize,
        year: i32,
        month: u8,
        day: u8,
        iso_date: *mut UnicodeISODate,
    ) -> bool;
    fn unicode_iso_year_and_month_code_to_iso_date(
        calendar: *const u8,
        calendar_length: usize,
        year: i32,
        month_code: *const u16,
        month_code_length: usize,
        day: u8,
        iso_date: *mut UnicodeISODate,
    ) -> bool;
    fn unicode_calendar_months_in_year(calendar: *const u8, calendar_length: usize, arithmetic_year: i32) -> u8;
    fn unicode_calendar_days_in_month(
        calendar: *const u8,
        calendar_length: usize,
        arithmetic_year: i32,
        ordinal_month: u8,
    ) -> u8;
    fn unicode_calendar_max_days_in_month_code(
        calendar: *const u8,
        calendar_length: usize,
        month_code: *const u16,
        month_code_length: usize,
    ) -> u8;
    fn unicode_calendar_year_contains_month_code(
        calendar: *const u8,
        calendar_length: usize,
        arithmetic_year: i32,
        month_code: *const u16,
        month_code_length: usize,
    ) -> bool;
}

/// Unicode::ISODate, 3.5.1 ISO Date Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-date-records
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ISODate {
    pub year: i32,
    pub month: u8,
    pub day: u8,
}

impl From<UnicodeISODate> for ISODate {
    fn from(iso_date: UnicodeISODate) -> Self {
        ISODate {
            year: iso_date.year,
            month: iso_date.month,
            day: iso_date.day,
        }
    }
}

/// Unicode::MonthCode, 12.2 Month Codes, https://tc39.es/proposal-temporal/#sec-temporal-month-codes
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MonthCode {
    pub month_number: u8,
    pub is_leap_month: bool,
}

/// Unicode::YearWeek, 14.3 The Year-Week Record Specification Type,
/// https://tc39.es/proposal-temporal/#sec-year-week-record-specification-type
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct YearWeek {
    pub week: Option<u8>,
    pub year: Option<i32>,
}

/// Unicode::CalendarDate, 12.3.1 Calendar Date Records, https://tc39.es/proposal-temporal/#sec-temporal-calendar-date-records
#[derive(Clone, Default)]
pub struct CalendarDate {
    pub era: Option<Utf16String>,
    pub era_year: Option<i32>,
    pub year: i32,
    pub month: u8,
    pub month_code: Utf16String,
    pub day: u8,
    pub day_of_week: u8,
    pub day_of_year: u16,
    pub week_of_year: YearWeek,
    pub days_in_week: u8,
    pub days_in_month: u8,
    pub days_in_year: u16,
    pub months_in_year: u8,
    pub in_leap_year: bool,
}

fn code_units_of(text: Utf16View<'_>) -> Vec<u16> {
    text.code_units().collect()
}

/// Unicode::parse_month_code: the month number and leap flag of a month code, or None if it is not a month code.
pub fn parse_month_code(month_code: Utf16View<'_>) -> Option<MonthCode> {
    let code_units = code_units_of(month_code);
    let mut month_number = 0;
    let mut is_leap_month = false;
    // SAFETY: The month code is valid for its length, and the outputs are locals.
    let parsed = unsafe {
        unicode_parse_month_code(
            code_units.as_ptr(),
            code_units.len(),
            &raw mut month_number,
            &raw mut is_leap_month,
        )
    };
    parsed.then_some(MonthCode {
        month_number,
        is_leap_month,
    })
}

/// Unicode::create_month_code.
pub fn create_month_code(month_number: u8, is_leap_month: bool) -> Utf16String {
    // SAFETY: The output writes into a Vec.
    let ((), month_code) =
        collect_text(|output| unsafe { unicode_create_month_code(month_number, is_leap_month, output) });
    Utf16String::from_utf16(&month_code)
}

/// Unicode::iso_date_to_calendar_date.
pub fn iso_date_to_calendar_date(calendar: &str, iso_date: ISODate) -> CalendarDate {
    let mut result = UnicodeCalendarDate::default();
    let (month_code, era) = collect_text(|era_output| {
        let ((), month_code) = collect_text(|month_code_output| {
            // SAFETY: The calendar is valid for its length, the date is a local, and the outputs write into Vecs.
            unsafe {
                unicode_iso_date_to_calendar_date(
                    calendar.as_ptr(),
                    calendar.len(),
                    UnicodeISODate {
                        year: iso_date.year,
                        month: iso_date.month,
                        day: iso_date.day,
                    },
                    &raw mut result,
                    era_output,
                    month_code_output,
                );
            }
        });
        month_code
    });
    CalendarDate {
        era: result.has_era.then(|| Utf16String::from_utf16(&era)),
        era_year: result.has_era_year.then_some(result.era_year),
        year: result.year,
        month: result.month,
        month_code: Utf16String::from_utf16(&month_code),
        day: result.day,
        day_of_week: result.day_of_week,
        day_of_year: result.day_of_year,
        week_of_year: YearWeek {
            week: result.has_week_of_year.then_some(result.week_of_year),
            year: result.has_year_of_week.then_some(result.year_of_week),
        },
        days_in_week: result.days_in_week,
        days_in_month: result.days_in_month,
        days_in_year: result.days_in_year,
        months_in_year: result.months_in_year,
        in_leap_year: result.in_leap_year,
    }
}

/// Unicode::calendar_date_to_iso_date.
pub fn calendar_date_to_iso_date(calendar: &str, year: i32, month: u8, day: u8) -> Option<ISODate> {
    let mut iso_date = UnicodeISODate::default();
    // SAFETY: The calendar is valid for its length, and the output is a local.
    let converted = unsafe {
        unicode_calendar_date_to_iso_date(calendar.as_ptr(), calendar.len(), year, month, day, &raw mut iso_date)
    };
    converted.then(|| iso_date.into())
}

/// Unicode::iso_year_and_month_code_to_iso_date.
pub fn iso_year_and_month_code_to_iso_date(
    calendar: &str,
    year: i32,
    month_code: Utf16View<'_>,
    day: u8,
) -> Option<ISODate> {
    let month_code = code_units_of(month_code);
    let mut iso_date = UnicodeISODate::default();
    // SAFETY: The calendar and month code are valid for their lengths, and the output is a local.
    let converted = unsafe {
        unicode_iso_year_and_month_code_to_iso_date(
            calendar.as_ptr(),
            calendar.len(),
            year,
            month_code.as_ptr(),
            month_code.len(),
            day,
            &raw mut iso_date,
        )
    };
    converted.then(|| iso_date.into())
}

/// Unicode::calendar_months_in_year.
pub fn calendar_months_in_year(calendar: &str, arithmetic_year: i32) -> u8 {
    // SAFETY: The calendar is valid for its length.
    unsafe { unicode_calendar_months_in_year(calendar.as_ptr(), calendar.len(), arithmetic_year) }
}

/// Unicode::calendar_days_in_month.
pub fn calendar_days_in_month(calendar: &str, arithmetic_year: i32, ordinal_month: u8) -> u8 {
    // SAFETY: The calendar is valid for its length.
    unsafe { unicode_calendar_days_in_month(calendar.as_ptr(), calendar.len(), arithmetic_year, ordinal_month) }
}

/// Unicode::calendar_max_days_in_month_code.
pub fn calendar_max_days_in_month_code(calendar: &str, month_code: Utf16View<'_>) -> u8 {
    let month_code = code_units_of(month_code);
    // SAFETY: The calendar and month code are valid for their lengths.
    unsafe {
        unicode_calendar_max_days_in_month_code(
            calendar.as_ptr(),
            calendar.len(),
            month_code.as_ptr(),
            month_code.len(),
        )
    }
}

/// Unicode::calendar_year_contains_month_code.
pub fn calendar_year_contains_month_code(calendar: &str, arithmetic_year: i32, month_code: Utf16View<'_>) -> bool {
    let month_code = code_units_of(month_code);
    // SAFETY: The calendar and month code are valid for their lengths.
    unsafe {
        unicode_calendar_year_contains_month_code(
            calendar.as_ptr(),
            calendar.len(),
            arithmetic_year,
            month_code.as_ptr(),
            month_code.len(),
        )
    }
}
