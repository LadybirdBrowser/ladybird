/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The calendars of LibUnicode, through its Rust crate, so that Temporal converts between ISO dates and calendar dates
//! with the same ICU data as the rest of Ladybird.

use ak::Utf16String;
use libunicode_rust::calendar as icu;
use libunicode_rust::calendar::FfiISODate;

use crate::utf16::Utf16View;

/// Unicode::ISODate, 3.5.1 ISO Date Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-date-records
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ISODate {
    pub year: i32,
    pub month: u8,
    pub day: u8,
}

impl From<FfiISODate> for ISODate {
    fn from(iso_date: FfiISODate) -> Self {
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

/// A month code as the ASCII string the calendars take. Month codes are validated before they get here, as
/// LibUnicode's month_code_as_ascii_bytes() requires.
fn month_code_as_ascii(month_code: Utf16View<'_>) -> String {
    let month_code: String = month_code
        .code_units()
        .map(|code_unit| {
            assert!(code_unit < 0x80, "month codes are ASCII");
            char::from(code_unit as u8)
        })
        .collect();
    assert!(month_code.len() <= 4, "month codes are at most four characters");
    month_code
}

fn is_valid_month_code_string(month_code: &[u16]) -> bool {
    // MonthCode :::
    //     M00L
    //     M0 NonZeroDigit L[opt]
    //     M NonZeroDigit DecimalDigit L[opt]
    let is_ascii_digit = |code_unit: u16| (u16::from(b'0')..=u16::from(b'9')).contains(&code_unit);
    let length = month_code.len();

    if length != 3 && length != 4 {
        return false;
    }

    if month_code[0] != u16::from(b'M') {
        return false;
    }

    if !is_ascii_digit(month_code[1]) || !is_ascii_digit(month_code[2]) {
        return false;
    }

    if length == 3 && month_code[1] == u16::from(b'0') && month_code[2] == u16::from(b'0') {
        return false;
    }
    if length == 4 && month_code[3] != u16::from(b'L') {
        return false;
    }

    true
}

/// 12.2.1 ParseMonthCode ( argument ), https://tc39.es/proposal-temporal/#sec-temporal-parsemonthcode
pub fn parse_month_code(month_code: Utf16View<'_>) -> Option<MonthCode> {
    let month_code: Vec<u16> = month_code.code_units().collect();

    // 3. If ParseText(StringToCodePoints(monthCode), MonthCode) is a List of errors, throw a RangeError exception.
    if !is_valid_month_code_string(&month_code) {
        return None;
    }

    // 4. Let isLeapMonth be false.
    let mut is_leap_month = false;

    // 5. If the length of monthCode = 4, then
    if month_code.len() == 4 {
        // a. Assert: The fourth code unit of monthCode is 0x004C (LATIN CAPITAL LETTER L).
        assert_eq!(month_code[3], u16::from(b'L'));

        // b. Set isLeapMonth to true.
        is_leap_month = true;
    }

    // 6. Let monthCodeDigits be the substring of monthCode from 1 to 3.

    // 7. Let monthNumber be ℝ(StringToNumber(monthCodeDigits)).
    let month_number = ((month_code[1] - u16::from(b'0')) * 10 + (month_code[2] - u16::from(b'0'))) as u8;

    // 8. Return the Record { [[MonthNumber]]: monthNumber, [[IsLeapMonth]]: isLeapMonth }.
    Some(MonthCode {
        month_number,
        is_leap_month,
    })
}

/// 12.2.2 CreateMonthCode ( monthNumber, isLeapMonth ), https://tc39.es/proposal-temporal/#sec-temporal-createmonthcode
pub fn create_month_code(month_number: u8, is_leap_month: bool) -> Utf16String {
    // 1. Assert: If isLeapMonth is false, monthNumber > 0.
    if !is_leap_month {
        assert!(month_number > 0);
    }

    // 2. Let numberPart be ToZeroPaddedDecimalString(monthNumber, 2).
    let number_part = format!("{month_number:02}");

    // 3. If isLeapMonth is true, then
    if is_leap_month {
        // a. Return the string-concatenation of the code unit 0x004D (LATIN CAPITAL LETTER M), numberPart, and the
        //    code unit 0x004C (LATIN CAPITAL LETTER L).
        return Utf16String::from_ascii(format!("M{number_part}L").as_bytes());
    }

    // 4. Return the string-concatenation of the code unit 0x004D (LATIN CAPITAL LETTER M) and numberPart.
    Utf16String::from_ascii(format!("M{number_part}").as_bytes())
}

/// Unicode::iso_date_to_calendar_date.
pub fn iso_date_to_calendar_date(calendar: &str, iso_date: ISODate) -> CalendarDate {
    let result = icu::iso_date_to_calendar_date(calendar, iso_date.year, iso_date.month, iso_date.day);
    let month_code = &result.month_code[..result.month_code_length as usize];
    CalendarDate {
        era: None,
        era_year: None,
        year: result.year,
        month: result.month,
        month_code: Utf16String::from_ascii(month_code),
        day: result.day,
        day_of_week: result.day_of_week,
        day_of_year: result.day_of_year,
        week_of_year: YearWeek::default(),
        days_in_week: result.days_in_week,
        days_in_month: result.days_in_month,
        days_in_year: result.days_in_year,
        months_in_year: result.months_in_year,
        in_leap_year: result.in_leap_year,
    }
}

/// Unicode::calendar_date_to_iso_date.
pub fn calendar_date_to_iso_date(calendar: &str, year: i32, month: u8, day: u8) -> Option<ISODate> {
    icu::calendar_date_to_iso_date(calendar, year, month, day).map(ISODate::from)
}

/// Unicode::iso_year_and_month_code_to_iso_date.
pub fn iso_year_and_month_code_to_iso_date(
    calendar: &str,
    year: i32,
    month_code: Utf16View<'_>,
    day: u8,
) -> Option<ISODate> {
    icu::iso_year_and_month_code_to_iso_date(calendar, &month_code_as_ascii(month_code), year, day).map(ISODate::from)
}

/// Unicode::calendar_months_in_year.
pub fn calendar_months_in_year(calendar: &str, arithmetic_year: i32) -> u8 {
    icu::calendar_months_in_year(calendar, arithmetic_year)
}

/// Unicode::calendar_days_in_month.
pub fn calendar_days_in_month(calendar: &str, arithmetic_year: i32, ordinal_month: u8) -> u8 {
    icu::calendar_days_in_month(calendar, arithmetic_year, ordinal_month)
}

/// Unicode::calendar_max_days_in_month_code.
pub fn calendar_max_days_in_month_code(calendar: &str, month_code: Utf16View<'_>) -> u8 {
    icu::calendar_max_days_in_month_code(calendar, &month_code_as_ascii(month_code))
}

/// Unicode::calendar_year_contains_month_code.
pub fn calendar_year_contains_month_code(calendar: &str, arithmetic_year: i32, month_code: Utf16View<'_>) -> bool {
    icu::year_contains_month_code(calendar, arithmetic_year, &month_code_as_ascii(month_code))
}
