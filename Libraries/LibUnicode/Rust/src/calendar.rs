/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use calendrical_calculations::rata_die::RataDie;
use icu_calendar::{AnyCalendar, AnyCalendarKind, Date, types::MonthCode};
use std::panic::{AssertUnwindSafe, catch_unwind};

#[repr(C)]
pub struct FfiISODate {
    pub year: i32,
    pub month: u8,
    pub day: u8,
}

#[repr(C)]
pub struct FfiOptionalISODate {
    pub iso_date: FfiISODate,
    pub has_value: bool,
}

#[repr(C)]
pub struct FfiCalendarDate {
    pub year: i32,
    pub month: u8,
    pub month_code: [u8; 5], // e.g. "M01\0\0" or "M05L\0"
    pub month_code_length: u8,
    pub day: u8,
    pub day_of_week: u8,
    pub day_of_year: u16,
    pub days_in_week: u8,
    pub days_in_month: u8,
    pub days_in_year: u16,
    pub months_in_year: u8,
    pub in_leap_year: bool,
}

const EMPTY_ISO_DATE: FfiOptionalISODate = FfiOptionalISODate {
    iso_date: FfiISODate {
        year: 0,
        month: 0,
        day: 0,
    },
    has_value: false,
};

const EMPTY_CALENDAR_DATE: FfiCalendarDate = FfiCalendarDate {
    year: 0,
    month: 0,
    month_code: [0; 5],
    month_code_length: 0,
    day: 0,
    day_of_week: 0,
    day_of_year: 0,
    days_in_week: 7,
    days_in_month: 0,
    days_in_year: 0,
    months_in_year: 0,
    in_leap_year: false,
};

fn abort_on_panic<F: FnOnce() -> R, R>(f: F) -> R {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(_) => std::process::abort(),
    }
}

/// SAFETY: All FFI string inputs are ASCII calendar names (e.g. "chinese") or month codes (e.g. "M01").
fn ascii_string_from_ffi<'a>(string: *const u8, length: usize) -> &'a str {
    let bytes = unsafe { std::slice::from_raw_parts(string, length) };
    debug_assert!(bytes.is_ascii(), "Expected ASCII input from FFI");
    unsafe { std::str::from_utf8_unchecked(bytes) }
}

fn iso_date_to_ffi(iso_date: Option<FfiISODate>) -> FfiOptionalISODate {
    match iso_date {
        Some(iso_date) => FfiOptionalISODate {
            iso_date,
            has_value: true,
        },
        None => EMPTY_ISO_DATE,
    }
}

fn make_calendar(calendar_name: &str) -> Option<AnyCalendar> {
    let kind = match calendar_name {
        "iso8601" => AnyCalendarKind::Iso,
        "gregory" => AnyCalendarKind::Gregorian,
        "buddhist" => AnyCalendarKind::Buddhist,
        "chinese" => AnyCalendarKind::Chinese,
        "coptic" => AnyCalendarKind::Coptic,
        "dangi" => AnyCalendarKind::Dangi,
        "ethiopic" => AnyCalendarKind::Ethiopian,
        "ethioaa" => AnyCalendarKind::EthiopianAmeteAlem,
        "hebrew" => AnyCalendarKind::Hebrew,
        "indian" => AnyCalendarKind::Indian,
        "islamic-civil" | "islamicc" => AnyCalendarKind::HijriTabularTypeIIFriday,
        "islamic-tbla" => AnyCalendarKind::HijriTabularTypeIIThursday,
        "islamic-umalqura" => AnyCalendarKind::HijriUmmAlQura,
        "islamic" => AnyCalendarKind::HijriSimulatedMecca,
        "japanese" => AnyCalendarKind::Japanese,
        "persian" => AnyCalendarKind::Persian,
        "roc" => AnyCalendarKind::Roc,
        _ => return None,
    };
    Some(AnyCalendar::new(kind))
}

fn is_chinese_or_dangi(calendar_name: &str) -> bool {
    calendar_name == "chinese" || calendar_name == "dangi"
}

fn parse_month_code(month_code: &str) -> Option<MonthCode> {
    if month_code.len() < 3 || !month_code.starts_with('M') {
        return None;
    }

    let month_number: u8 = month_code[1..3].parse().ok()?;
    let is_leap_month = month_code.len() == 4 && month_code.as_bytes()[3] == b'L';

    if is_leap_month {
        MonthCode::new_leap(month_number)
    } else {
        MonthCode::new_normal(month_number)
    }
}

fn encode_month_code(month_code: &MonthCode) -> ([u8; 5], u8) {
    let serialized = month_code.0.as_str();
    let bytes = serialized.as_bytes();

    let mut buffer = [0u8; 5];
    let length = bytes.len().min(5);
    buffer[..length].copy_from_slice(&bytes[..length]);

    (buffer, length as u8)
}

struct ChineseMonthInfo {
    month_code: MonthCode,
    days_in_month: u8,
}

/// Collect months starting from a known year-start date, iterating via RataDie arithmetic.
fn collect_year_months(
    calendar: &AnyCalendar,
    year_start: &Date<AnyCalendar>,
    extended_year: i32,
) -> Vec<ChineseMonthInfo> {
    let months_in_year = year_start.months_in_year();

    let mut result = Vec::with_capacity(months_in_year as usize);
    let mut current_date = year_start.clone();

    for _ in 0..months_in_year {
        let month_code = current_date.month().standard_code;
        let days_in_month = current_date.days_in_month();

        result.push(ChineseMonthInfo {
            month_code,
            days_in_month,
        });

        // Advance to day 1 of next month via RataDie arithmetic.
        let rata_die =
            RataDie::new(current_date.to_rata_die().to_i64_date() + days_in_month as i64);

        let next_date = Date::from_rata_die(rata_die, calendar.clone());
        if next_date.year().extended_year() != extended_year {
            break;
        }

        current_date = next_date;
    }

    result
}

/// Look up the ICU4X extended year for a Chinese/Dangi arithmetic year.
fn chinese_or_dangi_extended_year(calendar: &AnyCalendar, arithmetic_year: i32) -> Option<i32> {
    // Chinese new year falls in Jan/Feb. Use June 15 to land in the right calendar year.
    let approximate_iso = Date::try_new_iso(arithmetic_year, 6, 15).ok()?;
    let calendar_date = Date::new_from_iso(approximate_iso, calendar.clone());

    Some(calendar_date.year().extended_year())
}

/// Build a list of month info for each ordinal month in a Chinese/Dangi year.
fn chinese_year_months(
    calendar: &AnyCalendar,
    arithmetic_year: i32,
) -> Option<Vec<ChineseMonthInfo>> {
    let extended_year = chinese_or_dangi_extended_year(calendar, arithmetic_year)?;
    let month_one_code = MonthCode::new_normal(1)?;

    let year_start =
        Date::try_new_from_codes(None, extended_year, month_one_code, 1, calendar.clone()).ok()?;

    Some(collect_year_months(calendar, &year_start, extended_year))
}

/// Construct a calendar date from an arithmetic year and ordinal month.
fn make_calendar_date_from_ordinal(
    calendar_name: &str,
    calendar: &AnyCalendar,
    arithmetic_year: i32,
    ordinal_month: u8,
    day: u8,
) -> Option<Date<AnyCalendar>> {
    if is_chinese_or_dangi(calendar_name) {
        let months = chinese_year_months(calendar, arithmetic_year)?;

        let month_index = (ordinal_month as usize).checked_sub(1)?;
        if month_index >= months.len() {
            return None;
        }

        let month_code = months[month_index].month_code;
        let extended_year = chinese_or_dangi_extended_year(calendar, arithmetic_year)?;

        Date::try_new_from_codes(None, extended_year, month_code, day, calendar.clone()).ok()
    } else if calendar_name == "hebrew" {
        // Determine if this is a leap year by trying to construct a date with M05L.
        let adar_i_code = MonthCode::new_leap(5)?;
        let is_leap_year =
            Date::try_new_from_codes(None, arithmetic_year, adar_i_code, 1, calendar.clone())
                .is_ok();

        let month_code = if is_leap_year {
            if ordinal_month == 6 {
                MonthCode::new_leap(5)?
            } else if ordinal_month > 6 {
                MonthCode::new_normal(ordinal_month - 1)?
            } else {
                MonthCode::new_normal(ordinal_month)?
            }
        } else {
            MonthCode::new_normal(ordinal_month)?
        };

        Date::try_new_from_codes(None, arithmetic_year, month_code, day, calendar.clone()).ok()
    } else {
        let month_code = MonthCode::new_normal(ordinal_month)?;
        Date::try_new_from_codes(None, arithmetic_year, month_code, day, calendar.clone()).ok()
    }
}

fn iso_date_to_calendar_date_impl(
    calendar_name: &str,
    iso_year: i32,
    iso_month: u8,
    iso_day: u8,
) -> Option<FfiCalendarDate> {
    let calendar = make_calendar(calendar_name)?;

    let iso_date = Date::try_new_iso(iso_year, iso_month, iso_day).ok()?;
    let calendar_date = Date::new_from_iso(iso_date, calendar.clone());

    let (arithmetic_year, ordinal_month) = if is_chinese_or_dangi(calendar_name) {
        let extended_year = calendar_date.year().extended_year();

        let month_one_code = MonthCode::new_normal(1)?;
        let current_month_code = calendar_date.month().standard_code;

        let year_start =
            Date::try_new_from_codes(None, extended_year, month_one_code, 1, calendar.clone())
                .ok()?;

        let ordinal = collect_year_months(&calendar, &year_start, extended_year)
            .iter()
            .position(|m| m.month_code == current_month_code)
            .map(|i| (i + 1) as u8)
            .unwrap_or(1);

        (year_start.to_iso().extended_year(), ordinal)
    } else {
        (
            calendar_date.extended_year(),
            calendar_date.month().ordinal as u8,
        )
    };

    let month_code = calendar_date.month().standard_code;
    let (month_code_buffer, month_code_length) = encode_month_code(&month_code);

    Some(FfiCalendarDate {
        year: arithmetic_year,
        month: ordinal_month,
        month_code: month_code_buffer,
        month_code_length,
        day: calendar_date.day_of_month().0,
        day_of_week: calendar_date.day_of_week() as u8,
        day_of_year: calendar_date.day_of_year().0,
        days_in_week: 7,
        days_in_month: calendar_date.days_in_month(),
        days_in_year: calendar_date.days_in_year(),
        months_in_year: calendar_date.months_in_year(),
        in_leap_year: calendar_date.is_in_leap_year(),
    })
}

/// # Safety
/// `calendar` must point to a valid UTF-8 string of `calendar_length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn icu_iso_date_to_calendar_date(
    calendar: *const u8,
    calendar_length: usize,
    iso_year: i32,
    iso_month: u8,
    iso_day: u8,
) -> FfiCalendarDate {
    abort_on_panic(|| {
        let calendar_name = ascii_string_from_ffi(calendar, calendar_length);

        iso_date_to_calendar_date_impl(calendar_name, iso_year, iso_month, iso_day)
            .unwrap_or(EMPTY_CALENDAR_DATE)
    })
}

fn calendar_date_to_iso_date_impl(
    calendar_name: &str,
    arithmetic_year: i32,
    ordinal_month: u8,
    day: u8,
) -> Option<FfiISODate> {
    let calendar = make_calendar(calendar_name)?;

    let calendar_date = make_calendar_date_from_ordinal(
        calendar_name,
        &calendar,
        arithmetic_year,
        ordinal_month,
        day,
    )?;

    let iso_date = calendar_date.to_iso();

    Some(FfiISODate {
        year: iso_date.extended_year(),
        month: iso_date.month().ordinal as u8,
        day: iso_date.day_of_month().0,
    })
}

/// # Safety
/// `calendar` must point to a valid UTF-8 string of `calendar_length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn icu_calendar_date_to_iso_date(
    calendar: *const u8,
    calendar_length: usize,
    arithmetic_year: i32,
    ordinal_month: u8,
    day: u8,
) -> FfiOptionalISODate {
    abort_on_panic(|| {
        let calendar_name = ascii_string_from_ffi(calendar, calendar_length);

        iso_date_to_ffi(calendar_date_to_iso_date_impl(
            calendar_name,
            arithmetic_year,
            ordinal_month,
            day,
        ))
    })
}

fn iso_year_and_month_code_to_iso_date_impl(
    calendar_name: &str,
    month_code_string: &str,
    iso_year: i32,
    day: u8,
) -> Option<FfiISODate> {
    let calendar = make_calendar(calendar_name)?;
    let month_code = parse_month_code(month_code_string)?;

    let iso_jan1 = Date::try_new_iso(iso_year, 1, 1).ok()?;
    let iso_dec31 = Date::try_new_iso(iso_year, 12, 31).ok()?;

    let calendar_jan1 = Date::new_from_iso(iso_jan1, calendar.clone());
    let calendar_dec31 = Date::new_from_iso(iso_dec31, calendar.clone());

    let start_extended_year = calendar_jan1.extended_year();
    let end_extended_year = calendar_dec31.extended_year();

    let mut best_iso_date: Option<FfiISODate> = None;

    for extended_year in start_extended_year..=end_extended_year {
        let Ok(candidate) =
            Date::try_new_from_codes(None, extended_year, month_code, day, calendar.clone())
        else {
            continue;
        };

        if candidate.month().standard_code != month_code || candidate.day_of_month().0 != day {
            continue;
        }

        let iso_date = candidate.to_iso();
        if iso_date.extended_year() != iso_year {
            continue;
        }

        let candidate_date = FfiISODate {
            year: iso_date.extended_year(),
            month: iso_date.month().ordinal,
            day: iso_date.day_of_month().0,
        };

        best_iso_date = Some(match best_iso_date {
            Some(b) if (b.month, b.day) > (candidate_date.month, candidate_date.day) => b,
            _ => candidate_date,
        });
    }

    best_iso_date
}

/// # Safety
/// `calendar` must point to a valid UTF-8 string of `calendar_length` bytes.
/// `month_code` must point to a valid UTF-8 string of `month_code_length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn icu_iso_year_and_month_code_to_iso_date(
    calendar: *const u8,
    calendar_length: usize,
    iso_year: i32,
    month_code: *const u8,
    month_code_length: usize,
    day: u8,
) -> FfiOptionalISODate {
    abort_on_panic(|| {
        let calendar_name = ascii_string_from_ffi(calendar, calendar_length);
        let month_code_string = ascii_string_from_ffi(month_code, month_code_length);

        iso_date_to_ffi(iso_year_and_month_code_to_iso_date_impl(
            calendar_name,
            month_code_string,
            iso_year,
            day,
        ))
    })
}

fn calendar_year_and_month_code_to_iso_date_impl(
    calendar_name: &str,
    arithmetic_year: i32,
    month_code_string: &str,
    day: u8,
) -> Option<FfiISODate> {
    let calendar = make_calendar(calendar_name)?;
    let month_code = parse_month_code(month_code_string)?;

    let extended_year = if is_chinese_or_dangi(calendar_name) {
        chinese_or_dangi_extended_year(&calendar, arithmetic_year)?
    } else {
        arithmetic_year
    };

    let date = Date::try_new_from_codes(None, extended_year, month_code, day, calendar).ok()?;
    if date.month().standard_code != month_code || date.day_of_month().0 != day {
        return None;
    }

    let iso_date = date.to_iso();

    Some(FfiISODate {
        year: iso_date.extended_year(),
        month: iso_date.month().ordinal as u8,
        day: iso_date.day_of_month().0,
    })
}

/// # Safety
/// `calendar` must point to a valid UTF-8 string of `calendar_length` bytes.
/// `month_code` must point to a valid UTF-8 string of `month_code_length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn icu_calendar_year_and_month_code_to_iso_date(
    calendar: *const u8,
    calendar_length: usize,
    arithmetic_year: i32,
    month_code: *const u8,
    month_code_length: usize,
    day: u8,
) -> FfiOptionalISODate {
    abort_on_panic(|| {
        let calendar_name = ascii_string_from_ffi(calendar, calendar_length);
        let month_code_string = ascii_string_from_ffi(month_code, month_code_length);

        iso_date_to_ffi(calendar_year_and_month_code_to_iso_date_impl(
            calendar_name,
            arithmetic_year,
            month_code_string,
            day,
        ))
    })
}

fn calendar_months_in_year_impl(calendar_name: &str, arithmetic_year: i32) -> Option<u8> {
    let calendar = make_calendar(calendar_name)?;

    if is_chinese_or_dangi(calendar_name) {
        let months = chinese_year_months(&calendar, arithmetic_year)?;
        return Some(months.len() as u8);
    }

    let month_one_code = MonthCode::new_normal(1)?;

    let date = Date::try_new_from_codes(None, arithmetic_year, month_one_code, 1, calendar).ok()?;
    Some(date.months_in_year())
}

/// # Safety
/// `calendar` must point to a valid UTF-8 string of `calendar_length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn icu_calendar_months_in_year(
    calendar: *const u8,
    calendar_length: usize,
    arithmetic_year: i32,
) -> u8 {
    abort_on_panic(|| {
        let calendar_name = ascii_string_from_ffi(calendar, calendar_length);

        calendar_months_in_year_impl(calendar_name, arithmetic_year).unwrap_or(12)
    })
}

fn calendar_days_in_month_impl(
    calendar_name: &str,
    arithmetic_year: i32,
    ordinal_month: u8,
) -> Option<u8> {
    let calendar = make_calendar(calendar_name)?;

    if is_chinese_or_dangi(calendar_name) {
        let months = chinese_year_months(&calendar, arithmetic_year)?;
        let month_index = (ordinal_month as usize).checked_sub(1)?;

        return months.get(month_index).map(|m| m.days_in_month);
    }

    let date = make_calendar_date_from_ordinal(
        calendar_name,
        &calendar,
        arithmetic_year,
        ordinal_month,
        1,
    )?;

    Some(date.days_in_month())
}

/// # Safety
/// `calendar` must point to a valid UTF-8 string of `calendar_length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn icu_calendar_days_in_month(
    calendar: *const u8,
    calendar_length: usize,
    arithmetic_year: i32,
    ordinal_month: u8,
) -> u8 {
    abort_on_panic(|| {
        let calendar_name = ascii_string_from_ffi(calendar, calendar_length);

        calendar_days_in_month_impl(calendar_name, arithmetic_year, ordinal_month).unwrap_or(30)
    })
}

fn calendar_max_days_in_month_code_impl(
    calendar_name: &str,
    month_code_string: &str,
) -> Option<u8> {
    let calendar = make_calendar(calendar_name)?;
    let month_code = parse_month_code(month_code_string)?;

    let base_iso_date = Date::try_new_iso(1970, 7, 1).ok()?;
    let base_calendar_date = Date::new_from_iso(base_iso_date, calendar.clone());
    let base_extended_year = base_calendar_date.extended_year();

    let mut max_days_in_month: u8 = 0;

    for offset in -2i32..=2 {
        let extended_year = base_extended_year + offset;

        if let Ok(date) =
            Date::try_new_from_codes(None, extended_year, month_code, 1, calendar.clone())
            && date.month().standard_code == month_code
        {
            max_days_in_month = max_days_in_month.max(date.days_in_month());
        }
    }

    if max_days_in_month > 0 {
        Some(max_days_in_month)
    } else {
        None
    }
}

/// # Safety
/// `calendar` must point to a valid UTF-8 string of `calendar_length` bytes.
/// `month_code` must point to a valid UTF-8 string of `month_code_length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn icu_calendar_max_days_in_month_code(
    calendar: *const u8,
    calendar_length: usize,
    month_code: *const u8,
    month_code_length: usize,
) -> u8 {
    abort_on_panic(|| {
        let calendar_name = ascii_string_from_ffi(calendar, calendar_length);
        let month_code = ascii_string_from_ffi(month_code, month_code_length);

        calendar_max_days_in_month_code_impl(calendar_name, month_code).unwrap_or(30)
    })
}

fn year_contains_month_code_impl(
    calendar_name: &str,
    arithmetic_year: i32,
    month_code_string: &str,
) -> Option<bool> {
    let calendar = make_calendar(calendar_name)?;
    let month_code = parse_month_code(month_code_string)?;

    let contains_month_code = if is_chinese_or_dangi(calendar_name) {
        let months = chinese_year_months(&calendar, arithmetic_year)?;
        months.iter().any(|m| m.month_code == month_code)
    } else {
        matches!(
            Date::try_new_from_codes(None, arithmetic_year, month_code, 1, calendar),
            Ok(date) if date.month().standard_code == month_code
        )
    };

    Some(contains_month_code)
}

/// # Safety
/// `calendar` must point to a valid UTF-8 string of `calendar_length` bytes.
/// `month_code` must point to a valid UTF-8 string of `month_code_length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn icu_year_contains_month_code(
    calendar: *const u8,
    calendar_length: usize,
    arithmetic_year: i32,
    month_code: *const u8,
    month_code_length: usize,
) -> bool {
    abort_on_panic(|| {
        let calendar_name = ascii_string_from_ffi(calendar, calendar_length);
        let month_code_string = ascii_string_from_ffi(month_code, month_code_length);

        year_contains_month_code_impl(calendar_name, arithmetic_year, month_code_string)
            .unwrap_or(false)
    })
}
