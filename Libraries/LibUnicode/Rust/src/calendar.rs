/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use calendrical_calculations::rata_die::RataDie;
use icu_calendar::{
    AnyCalendar, AnyCalendarKind, Date, Iso,
    types::{DateFields, Month},
};
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

/// Construct a date from extended year, month, and day.
///
/// Uses `try_from_fields` instead of `try_new` to support the wider year range (+/- 1,040,000) needed by
/// Chinese and Dangi calendars with extreme years.
fn try_new_date(extended_year: i32, month: Month, day: u8, calendar: AnyCalendar) -> Option<Date<AnyCalendar>> {
    let mut fields = DateFields::default();
    fields.extended_year = Some(extended_year);
    fields.month = Some(month);
    fields.day = Some(day);

    Date::try_from_fields(fields, Default::default(), calendar).ok()
}

fn encode_month_code(month: Month) -> ([u8; 5], u8) {
    let code = month.code();
    let serialized = code.0.as_str();
    let bytes = serialized.as_bytes();

    let mut buffer = [0u8; 5];
    let length = bytes.len().min(5);
    buffer[..length].copy_from_slice(&bytes[..length]);

    (buffer, length as u8)
}

struct ChineseMonthInfo {
    month: Month,
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
        let month = current_date.month().to_input();
        let days_in_month = current_date.days_in_month();

        result.push(ChineseMonthInfo { month, days_in_month });

        // Advance to day 1 of next month via RataDie arithmetic.
        let rata_die = RataDie::new(current_date.to_rata_die().to_i64_date() + days_in_month as i64);

        let next_date = Date::from_rata_die(rata_die, calendar.clone());
        if next_date.year().extended_year() != extended_year {
            break;
        }

        current_date = next_date;
    }

    result
}

/// Construct an ISO date using RataDie to bypass the -9999..=9999 year range limit in Date::try_new_iso.
fn make_iso_date(year: i32, month: u8, day: u8) -> Option<Date<Iso>> {
    if !(1..=12).contains(&month) || !(1..=31).contains(&day) {
        return None;
    }

    let rd = calendrical_calculations::gregorian::fixed_from_gregorian(year, month, day);
    Some(Date::from_rata_die(rd, Iso))
}

/// Look up the ICU4X extended year for a Chinese/Dangi arithmetic year.
fn chinese_or_dangi_extended_year(calendar: &AnyCalendar, arithmetic_year: i32) -> Option<i32> {
    // Chinese new year falls in Jan/Feb. Use June 15 to land in the right calendar year.
    let approximate_iso = make_iso_date(arithmetic_year, 6, 15)?;
    let calendar_date = approximate_iso.to_calendar(calendar.clone());

    Some(calendar_date.year().extended_year())
}

/// Build a list of month info for each ordinal month in a Chinese/Dangi year.
fn chinese_year_months(calendar: &AnyCalendar, arithmetic_year: i32) -> Option<Vec<ChineseMonthInfo>> {
    let extended_year = chinese_or_dangi_extended_year(calendar, arithmetic_year)?;

    let year_start = try_new_date(extended_year, Month::new(1), 1, calendar.clone())?;

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

        let month = months[month_index].month;
        let extended_year = chinese_or_dangi_extended_year(calendar, arithmetic_year)?;

        try_new_date(extended_year, month, day, calendar.clone())
    } else if calendar_name == "hebrew" {
        // Determine if this is a leap year by trying to construct a date with M05L.
        let adar_i = Month::leap(5);
        let is_leap_year = try_new_date(arithmetic_year, adar_i, 1, calendar.clone()).is_some();

        let month = if is_leap_year {
            if ordinal_month == 6 {
                Month::leap(5)
            } else if ordinal_month > 6 {
                Month::new(ordinal_month - 1)
            } else {
                Month::new(ordinal_month)
            }
        } else {
            Month::new(ordinal_month)
        };

        try_new_date(arithmetic_year, month, day, calendar.clone())
    } else {
        try_new_date(arithmetic_year, Month::new(ordinal_month), day, calendar.clone())
    }
}

fn iso_date_to_calendar_date_impl(
    calendar_name: &str,
    iso_year: i32,
    iso_month: u8,
    iso_day: u8,
) -> Option<FfiCalendarDate> {
    let calendar = make_calendar(calendar_name)?;

    let iso_date = make_iso_date(iso_year, iso_month, iso_day)?;
    let calendar_date = iso_date.to_calendar(calendar.clone());

    let (arithmetic_year, ordinal_month) = if is_chinese_or_dangi(calendar_name) {
        let extended_year = calendar_date.year().extended_year();

        let current_month = calendar_date.month().to_input();

        let year_start = try_new_date(extended_year, Month::new(1), 1, calendar.clone())?;

        let ordinal = collect_year_months(&calendar, &year_start, extended_year)
            .iter()
            .position(|m| m.month == current_month)
            .map(|i| (i + 1) as u8)
            .unwrap_or(1);

        (year_start.to_calendar(Iso).year().extended_year(), ordinal)
    } else {
        (calendar_date.year().extended_year(), calendar_date.month().ordinal)
    };

    let month = calendar_date.month().to_input();
    let (month_code_buffer, month_code_length) = encode_month_code(month);

    Some(FfiCalendarDate {
        year: arithmetic_year,
        month: ordinal_month,
        month_code: month_code_buffer,
        month_code_length,
        day: calendar_date.day_of_month().0,
        day_of_week: calendar_date.weekday() as u8,
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

        iso_date_to_calendar_date_impl(calendar_name, iso_year, iso_month, iso_day).unwrap_or(EMPTY_CALENDAR_DATE)
    })
}

fn calendar_date_to_iso_date_impl(
    calendar_name: &str,
    arithmetic_year: i32,
    ordinal_month: u8,
    day: u8,
) -> Option<FfiISODate> {
    let calendar = make_calendar(calendar_name)?;

    let calendar_date = make_calendar_date_from_ordinal(calendar_name, &calendar, arithmetic_year, ordinal_month, day)?;

    let iso_date = calendar_date.to_calendar(Iso);

    Some(FfiISODate {
        year: iso_date.year().extended_year(),
        month: iso_date.month().ordinal,
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
    let month = Month::try_from_str(month_code_string).ok()?;

    let iso_jan1 = make_iso_date(iso_year, 1, 1)?;
    let iso_dec31 = make_iso_date(iso_year, 12, 31)?;

    let calendar_jan1 = iso_jan1.to_calendar(calendar.clone());
    let calendar_dec31 = iso_dec31.to_calendar(calendar.clone());

    let start_extended_year = calendar_jan1.year().extended_year();
    let end_extended_year = calendar_dec31.year().extended_year();

    let mut best_iso_date: Option<FfiISODate> = None;

    for extended_year in start_extended_year..=end_extended_year {
        let Some(candidate) = try_new_date(extended_year, month, day, calendar.clone()) else {
            continue;
        };

        if candidate.month().to_input() != month || candidate.day_of_month().0 != day {
            continue;
        }

        let iso_date = candidate.to_calendar(Iso);
        if iso_date.year().extended_year() != iso_year {
            continue;
        }

        let candidate_date = FfiISODate {
            year: iso_date.year().extended_year(),
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
    let month = Month::try_from_str(month_code_string).ok()?;

    let extended_year = if is_chinese_or_dangi(calendar_name) {
        chinese_or_dangi_extended_year(&calendar, arithmetic_year)?
    } else {
        arithmetic_year
    };

    let date = try_new_date(extended_year, month, day, calendar)?;
    if date.month().to_input() != month || date.day_of_month().0 != day {
        return None;
    }

    let iso_date = date.to_calendar(Iso);

    Some(FfiISODate {
        year: iso_date.year().extended_year(),
        month: iso_date.month().ordinal,
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

    let date = try_new_date(arithmetic_year, Month::new(1), 1, calendar)?;
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

fn calendar_days_in_month_impl(calendar_name: &str, arithmetic_year: i32, ordinal_month: u8) -> Option<u8> {
    let calendar = make_calendar(calendar_name)?;

    if is_chinese_or_dangi(calendar_name) {
        let months = chinese_year_months(&calendar, arithmetic_year)?;
        let month_index = (ordinal_month as usize).checked_sub(1)?;

        return months.get(month_index).map(|m| m.days_in_month);
    }

    let date = make_calendar_date_from_ordinal(calendar_name, &calendar, arithmetic_year, ordinal_month, 1)?;

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

fn calendar_max_days_in_month_code_impl(calendar_name: &str, month_code_string: &str) -> Option<u8> {
    let calendar = make_calendar(calendar_name)?;
    let month = Month::try_from_str(month_code_string).ok()?;

    let base_iso_date = make_iso_date(1970, 7, 1)?;
    let base_calendar_date = base_iso_date.to_calendar(calendar.clone());
    let base_extended_year = base_calendar_date.year().extended_year();

    let mut max_days_in_month: u8 = 0;

    for offset in -2i32..=2 {
        let extended_year = base_extended_year + offset;

        if let Some(date) = try_new_date(extended_year, month, 1, calendar.clone())
            && date.month().to_input() == month
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

fn year_contains_month_code_impl(calendar_name: &str, arithmetic_year: i32, month_code_string: &str) -> Option<bool> {
    let calendar = make_calendar(calendar_name)?;
    let month = Month::try_from_str(month_code_string).ok()?;

    let contains_month_code = if is_chinese_or_dangi(calendar_name) {
        let months = chinese_year_months(&calendar, arithmetic_year)?;
        months.iter().any(|m| m.month == month)
    } else {
        matches!(
            try_new_date(arithmetic_year, month, 1, calendar),
            Some(date) if date.month().to_input() == month
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

        year_contains_month_code_impl(calendar_name, arithmetic_year, month_code_string).unwrap_or(false)
    })
}
