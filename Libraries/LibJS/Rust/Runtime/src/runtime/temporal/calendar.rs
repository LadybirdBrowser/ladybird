/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/Calendar.cpp: calendar identifiers, calendar fields, and the date arithmetic of
//! the ISO 8601 calendar and of the calendars LibUnicode provides.

use std::cell::OnceCell;

use ak::Utf16String;

use crate::interpreter::vm::Vm;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::temporal::abstract_operations::{
    DateType, Overflow, ShowCalendar, Unit, ascii_view, epoch_days_to_epoch_ms, iso_date_to_epoch_days,
    parse_temporal_calendar_string, to_integer_with_truncation, to_offset_string, to_positive_integer_with_truncation,
};
use crate::runtime::temporal::date_equations::{
    epoch_time_for_year, epoch_time_to_day_in_year, epoch_time_to_week_day, mathematical_days_in_year,
    mathematical_in_leap_year,
};
use crate::runtime::temporal::duration::{DateDuration, create_date_duration_record, zero_date_duration};
use crate::runtime::temporal::iso_records::ISODate;
use crate::runtime::temporal::plain_date::{
    MonthOrCode, PlainDate, add_days_to_iso_date, compare_iso_date, compare_surpasses, create_iso_date_record,
    iso_date_surpasses, iso_date_within_limits, regulate_iso_date,
};
use crate::runtime::temporal::plain_date_time::PlainDateTime;
use crate::runtime::temporal::plain_month_day::PlainMonthDay;
use crate::runtime::temporal::plain_year_month::{
    PlainYearMonth, balance_iso_year_month, iso_year_month_within_limits,
};
use crate::runtime::temporal::time_zone::to_temporal_time_zone_identifier;
use crate::runtime::temporal::zoned_date_time::ZonedDateTime;
use crate::runtime::value::PreferredType;
use crate::unicode::calendar as unicode_calendar;
pub use crate::unicode::calendar::{CalendarDate, MonthCode, YearWeek};
use crate::unicode::intl as unicode_intl;
use crate::utf16::{TrimMode, Utf16View};

pub const ISO8601_CALENDAR: &str = "iso8601";

#[allow(
    clippy::enum_variant_names,
    reason = "the variants are the C++ CalendarFieldConversion values"
)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum CalendarFieldConversion {
    ToIntegerWithTruncation,
    ToMonthCode,
    ToOffsetString,
    ToPositiveIntegerWithTruncation,
    ToString,
    ToTemporalTimeZoneIdentifier,
}

// https://tc39.es/proposal-temporal/#table-temporal-calendar-fields-record-fields
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CalendarField {
    Era,
    EraYear,
    Year,
    Month,
    MonthCode,
    Day,
    Hour,
    Minute,
    Second,
    Millisecond,
    Microsecond,
    Nanosecond,
    Offset,
    TimeZone,
}

/// The rows of Table 19, in table order.
const CALENDAR_FIELDS: [CalendarField; 14] = [
    CalendarField::Era,
    CalendarField::EraYear,
    CalendarField::Year,
    CalendarField::Month,
    CalendarField::MonthCode,
    CalendarField::Day,
    CalendarField::Hour,
    CalendarField::Minute,
    CalendarField::Second,
    CalendarField::Millisecond,
    CalendarField::Microsecond,
    CalendarField::Nanosecond,
    CalendarField::Offset,
    CalendarField::TimeZone,
];

// https://tc39.es/proposal-temporal/#table-temporal-calendar-fields-record-fields
#[derive(Clone, PartialEq)]
pub struct CalendarFields {
    pub era: Option<Utf16String>,
    pub era_year: Option<i32>,
    pub year: Option<i32>,
    pub month: Option<u32>,
    pub month_code: Option<Utf16String>,
    pub day: Option<u32>,
    pub hour: Option<u8>,
    pub minute: Option<u8>,
    pub second: Option<u8>,
    pub millisecond: Option<u16>,
    pub microsecond: Option<u16>,
    pub nanosecond: Option<u16>,
    pub offset_string: Option<Utf16String>,
    pub time_zone: Option<Utf16String>,
}

/// The C++ CalendarFields {}, whose time fields default to 0.
impl Default for CalendarFields {
    fn default() -> Self {
        Self {
            hour: Some(0),
            minute: Some(0),
            second: Some(0),
            millisecond: Some(0),
            microsecond: Some(0),
            nanosecond: Some(0),
            ..Self::unset()
        }
    }
}

impl CalendarFields {
    pub fn unset() -> Self {
        Self {
            era: None,
            era_year: None,
            year: None,
            month: None,
            month_code: None,
            day: None,
            hour: None,
            minute: None,
            second: None,
            millisecond: None,
            microsecond: None,
            nanosecond: None,
            offset_string: None,
            time_zone: None,
        }
    }

    fn has_field(&self, field: CalendarField) -> bool {
        match field {
            CalendarField::Era => self.era.is_some(),
            CalendarField::EraYear => self.era_year.is_some(),
            CalendarField::Year => self.year.is_some(),
            CalendarField::Month => self.month.is_some(),
            CalendarField::MonthCode => self.month_code.is_some(),
            CalendarField::Day => self.day.is_some(),
            CalendarField::Hour => self.hour.is_some(),
            CalendarField::Minute => self.minute.is_some(),
            CalendarField::Second => self.second.is_some(),
            CalendarField::Millisecond => self.millisecond.is_some(),
            CalendarField::Microsecond => self.microsecond.is_some(),
            CalendarField::Nanosecond => self.nanosecond.is_some(),
            CalendarField::Offset => self.offset_string.is_some(),
            CalendarField::TimeZone => self.time_zone.is_some(),
        }
    }

    /// Sets this record's field to the same field of `other`.
    fn copy_field_from(&mut self, field: CalendarField, other: &CalendarFields) {
        match field {
            CalendarField::Era => self.era.clone_from(&other.era),
            CalendarField::EraYear => self.era_year = other.era_year,
            CalendarField::Year => self.year = other.year,
            CalendarField::Month => self.month = other.month,
            CalendarField::MonthCode => self.month_code.clone_from(&other.month_code),
            CalendarField::Day => self.day = other.day,
            CalendarField::Hour => self.hour = other.hour,
            CalendarField::Minute => self.minute = other.minute,
            CalendarField::Second => self.second = other.second,
            CalendarField::Millisecond => self.millisecond = other.millisecond,
            CalendarField::Microsecond => self.microsecond = other.microsecond,
            CalendarField::Nanosecond => self.nanosecond = other.nanosecond,
            CalendarField::Offset => self.offset_string.clone_from(&other.offset_string),
            CalendarField::TimeZone => self.time_zone.clone_from(&other.time_zone),
        }
    }

    /// The C++ set_field_value() with a double, which C++ converts to the integer type of the field: to an i32 or a
    /// u32 saturating, and to the u8 and u16 time fields through an i32, wrapping.
    fn set_number_field(&mut self, field: CalendarField, value: f64) {
        match field {
            CalendarField::EraYear => self.era_year = Some(value as i32),
            CalendarField::Year => self.year = Some(value as i32),
            CalendarField::Month => self.month = Some(value as u32),
            CalendarField::Day => self.day = Some(value as u32),
            CalendarField::Hour => self.hour = Some(value as i32 as u8),
            CalendarField::Minute => self.minute = Some(value as i32 as u8),
            CalendarField::Second => self.second = Some(value as i32 as u8),
            CalendarField::Millisecond => self.millisecond = Some(value as i32 as u16),
            CalendarField::Microsecond => self.microsecond = Some(value as i32 as u16),
            CalendarField::Nanosecond => self.nanosecond = Some(value as i32 as u16),
            CalendarField::Era | CalendarField::MonthCode | CalendarField::Offset | CalendarField::TimeZone => {}
        }
    }

    /// The C++ set_field_value() with a string.
    fn set_string_field(&mut self, field: CalendarField, value: Utf16String) {
        match field {
            CalendarField::Era => self.era = Some(value),
            CalendarField::MonthCode => self.month_code = Some(value),
            CalendarField::Offset => self.offset_string = Some(value),
            CalendarField::TimeZone => self.time_zone = Some(value),
            _ => {}
        }
    }
}

/// CalendarFieldListOrPartial: PARTIAL, or a List of fields.
#[derive(Clone, Copy, Debug)]
pub enum CalendarFieldListOrPartial<'a> {
    Partial,
    List(&'a [CalendarField]),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BalancedDate {
    pub year: i32,
    pub month: u8,
    pub day: u8,
}

struct CalendarFieldData {
    key: CalendarField,
    property: PropertyKey,
    conversion: CalendarFieldConversion,
}

// https://tc39.es/proposal-temporal/#table-temporal-calendar-fields-record-fields
fn calendar_field_data(vm: &Vm, field: CalendarField) -> CalendarFieldData {
    let names = &vm.names;
    let (property, conversion) = match field {
        CalendarField::Era => (&names.era, CalendarFieldConversion::ToString),
        CalendarField::EraYear => (&names.eraYear, CalendarFieldConversion::ToIntegerWithTruncation),
        CalendarField::Year => (&names.year, CalendarFieldConversion::ToIntegerWithTruncation),
        CalendarField::Month => (&names.month, CalendarFieldConversion::ToPositiveIntegerWithTruncation),
        CalendarField::MonthCode => (&names.monthCode, CalendarFieldConversion::ToMonthCode),
        CalendarField::Day => (&names.day, CalendarFieldConversion::ToPositiveIntegerWithTruncation),
        CalendarField::Hour => (&names.hour, CalendarFieldConversion::ToIntegerWithTruncation),
        CalendarField::Minute => (&names.minute, CalendarFieldConversion::ToIntegerWithTruncation),
        CalendarField::Second => (&names.second, CalendarFieldConversion::ToIntegerWithTruncation),
        CalendarField::Millisecond => (&names.millisecond, CalendarFieldConversion::ToIntegerWithTruncation),
        CalendarField::Microsecond => (&names.microsecond, CalendarFieldConversion::ToIntegerWithTruncation),
        CalendarField::Nanosecond => (&names.nanosecond, CalendarFieldConversion::ToIntegerWithTruncation),
        CalendarField::Offset => (&names.offset, CalendarFieldConversion::ToOffsetString),
        CalendarField::TimeZone => (&names.timeZone, CalendarFieldConversion::ToTemporalTimeZoneIdentifier),
    };
    CalendarFieldData {
        key: field,
        property: property.clone(),
        conversion,
    }
}

fn sorted_calendar_fields(vm: &Vm, fields: &[CalendarField]) -> Vec<CalendarFieldData> {
    let mut result: Vec<CalendarFieldData> = fields.iter().map(|&field| calendar_field_data(vm, field)).collect();

    result.sort_by(|lhs, rhs| {
        let lhs = Utf16View::of_fly_string(lhs.property.as_string());
        let rhs = Utf16View::of_fly_string(rhs.property.as_string());
        lhs.code_units().cmp(rhs.code_units())
    });

    result
}

// Table 1: Calendar types described in CLDR, https://tc39.es/proposal-intl-era-monthcode/#table-calendar-types
const CLDR_CALENDAR_TYPES: [&str; 18] = [
    "buddhist",
    "chinese",
    "coptic",
    "dangi",
    "ethioaa",
    "ethiopic",
    "ethiopic-amete-alem",
    "gregory",
    "hebrew",
    "indian",
    "islamic-civil",
    "islamic-tbla",
    "islamic-umalqura",
    "islamicc",
    "iso8601",
    "japanese",
    "persian",
    "roc",
];

#[derive(Clone, Copy, PartialEq, Eq)]
enum EraKind {
    Epoch,
    Offset,
    Negative,
}

// Table 2: Eras, https://tc39.es/proposal-intl-era-monthcode/#table-eras
struct CalendarEraData {
    calendar: &'static str,
    era: &'static str,
    alias: Option<&'static str>,
    minimum_era_year: Option<i32>,
    maximum_era_year: Option<i32>,
    kind: EraKind,
    offset: Option<i32>,

    // NB: This column is not in the spec table, but is needed to handle calendars with mid-year era transitions.
    iso_era_start: Option<ISODate>,
}

#[allow(clippy::too_many_arguments, reason = "one argument per column of the era table")]
const fn era(
    calendar: &'static str,
    era: &'static str,
    alias: Option<&'static str>,
    minimum_era_year: Option<i32>,
    maximum_era_year: Option<i32>,
    kind: EraKind,
    offset: Option<i32>,
    iso_era_start: Option<(i32, u8, u8)>,
) -> CalendarEraData {
    CalendarEraData {
        calendar,
        era,
        alias,
        minimum_era_year,
        maximum_era_year,
        kind,
        offset,
        iso_era_start: match iso_era_start {
            Some((year, month, day)) => Some(ISODate { year, month, day }),
            None => None,
        },
    }
}

#[rustfmt::skip]
const CALENDAR_ERA_DATA: [CalendarEraData; 25] = [
    era("buddhist",         "be",     None,       None,    None,       EraKind::Epoch,    None,        None                  ),
    era("coptic",           "am",     None,       None,    None,       EraKind::Epoch,    None,        None                  ),
    era("ethioaa",          "aa",     None,       None,    None,       EraKind::Epoch,    None,        None                  ),
    era("ethiopic",         "am",     None,       Some(1), None,       EraKind::Epoch,    None,        None                  ),
    era("ethiopic",         "aa",     None,       None,    Some(5500), EraKind::Offset,   Some(-5499), None                  ),
    era("gregory",          "ce",     Some("ad"), Some(1), None,       EraKind::Epoch,    None,        None                  ),
    era("gregory",          "bce",    Some("bc"), Some(1), None,       EraKind::Negative, None,        None                  ),
    era("hebrew",           "am",     None,       None,    None,       EraKind::Epoch,    None,        None                  ),
    era("indian",           "shaka",  None,       None,    None,       EraKind::Epoch,    None,        None                  ),
    era("islamic-civil",    "ah",     None,       Some(1), None,       EraKind::Epoch,    None,        None                  ),
    era("islamic-civil",    "bh",     None,       Some(1), None,       EraKind::Negative, None,        None                  ),
    era("islamic-tbla",     "ah",     None,       Some(1), None,       EraKind::Epoch,    None,        None                  ),
    era("islamic-tbla",     "bh",     None,       Some(1), None,       EraKind::Negative, None,        None                  ),
    era("islamic-umalqura", "ah",     None,       Some(1), None,       EraKind::Epoch,    None,        None                  ),
    era("islamic-umalqura", "bh",     None,       Some(1), None,       EraKind::Negative, None,        None                  ),
    era("japanese",         "reiwa",  None,       Some(1), None,       EraKind::Offset,   Some(2019),  Some((2019, 5, 1))    ),
    era("japanese",         "heisei", None,       Some(1), Some(31),   EraKind::Offset,   Some(1989),  Some((1989, 1, 8))    ),
    era("japanese",         "showa",  None,       Some(1), Some(64),   EraKind::Offset,   Some(1926),  Some((1926, 12, 25))  ),
    era("japanese",         "taisho", None,       Some(1), Some(15),   EraKind::Offset,   Some(1912),  Some((1912, 7, 30))   ),
    era("japanese",         "meiji",  None,       Some(1), Some(45),   EraKind::Offset,   Some(1868),  Some((1873, 1, 1))    ),
    era("japanese",         "ce",     Some("ad"), Some(1), Some(1872), EraKind::Epoch,    None,        None                  ),
    era("japanese",         "bce",    Some("bc"), Some(1), None,       EraKind::Negative, None,        None                  ),
    era("persian",          "ap",     None,       None,    None,       EraKind::Epoch,    None,        None                  ),
    era("roc",              "roc",    None,       Some(1), None,       EraKind::Epoch,    None,        None                  ),
    era("roc",              "broc",   None,       Some(1), None,       EraKind::Negative, None,        None                  ),
];

#[derive(Clone, Copy, PartialEq, Eq)]
enum Leap {
    SkipBackward,
    SkipForward,
}

// Table 3: Additional Month Codes in Calendars, https://tc39.es/proposal-intl-era-monthcode/#table-additional-month-codes
struct AdditionalMonthCodes {
    calendar: &'static str,
    additional_month_codes: &'static [&'static str],
    leap_to_common_month_transformation: Option<Leap>,
}

const ALL_LEAP_MONTH_CODES: [&str; 12] = [
    "M01L", "M02L", "M03L", "M04L", "M05L", "M06L", "M07L", "M08L", "M09L", "M10L", "M11L", "M12L",
];
const THIRTEENTH_MONTH_CODES: [&str; 1] = ["M13"];
const HEBREW_ADAR_I_MONTH_CODES: [&str; 1] = ["M05L"];

const ADDITIONAL_MONTH_CODES: [AdditionalMonthCodes; 6] = [
    AdditionalMonthCodes {
        calendar: "chinese",
        additional_month_codes: &ALL_LEAP_MONTH_CODES,
        leap_to_common_month_transformation: Some(Leap::SkipBackward),
    },
    AdditionalMonthCodes {
        calendar: "coptic",
        additional_month_codes: &THIRTEENTH_MONTH_CODES,
        leap_to_common_month_transformation: None,
    },
    AdditionalMonthCodes {
        calendar: "dangi",
        additional_month_codes: &ALL_LEAP_MONTH_CODES,
        leap_to_common_month_transformation: Some(Leap::SkipBackward),
    },
    AdditionalMonthCodes {
        calendar: "ethioaa",
        additional_month_codes: &THIRTEENTH_MONTH_CODES,
        leap_to_common_month_transformation: None,
    },
    AdditionalMonthCodes {
        calendar: "ethiopic",
        additional_month_codes: &THIRTEENTH_MONTH_CODES,
        leap_to_common_month_transformation: None,
    },
    AdditionalMonthCodes {
        calendar: "hebrew",
        additional_month_codes: &HEBREW_ADAR_I_MONTH_CODES,
        leap_to_common_month_transformation: Some(Leap::SkipForward),
    },
];

fn additional_month_codes_of(calendar: Utf16View<'_>) -> Option<&'static AdditionalMonthCodes> {
    ADDITIONAL_MONTH_CODES.iter().find(|row| calendar == row.calendar)
}

// Table 6: "chinese" and "dangi" Calendars ISO Reference Years, https://tc39.es/proposal-intl-era-monthcode/#chinese-dangi-iso-reference-years
struct ISOReferenceYears {
    month_code: &'static str,
    days_1_to_29: Option<i32>,
    day_30: Option<i32>,
}

const fn reference_years(
    month_code: &'static str,
    days_1_to_29: Option<i32>,
    day_30: Option<i32>,
) -> ISOReferenceYears {
    ISOReferenceYears {
        month_code,
        days_1_to_29,
        day_30,
    }
}

const CHINESE_AND_DANGI_ISO_REFERENCE_YEARS: [ISOReferenceYears; 24] = [
    reference_years("M01", Some(1972), Some(1970)),
    reference_years("M01L", None, None),
    reference_years("M02", Some(1972), Some(1972)),
    reference_years("M02L", Some(1947), None),
    reference_years("M03", Some(1972), Some(0)), // Day=30 depends on the calendar and is handled below.
    reference_years("M03L", Some(1966), Some(1955)),
    reference_years("M04", Some(1972), Some(1970)),
    reference_years("M04L", Some(1963), Some(1944)),
    reference_years("M05", Some(1972), Some(1972)),
    reference_years("M05L", Some(1971), Some(1952)),
    reference_years("M06", Some(1972), Some(1971)),
    reference_years("M06L", Some(1960), Some(1941)),
    reference_years("M07", Some(1972), Some(1972)),
    reference_years("M07L", Some(1968), Some(1938)),
    reference_years("M08", Some(1972), Some(1971)),
    reference_years("M08L", Some(1957), None),
    reference_years("M09", Some(1972), Some(1972)),
    reference_years("M09L", Some(2014), None),
    reference_years("M10", Some(1972), Some(1972)),
    reference_years("M10L", Some(1984), None),
    reference_years("M11", Some(1972), Some(1970)),
    reference_years("M11L", Some(0), None), // The reference year for days 1-10 and days 11-29 differ and is handled below.
    reference_years("M12", Some(1972), Some(1972)),
    reference_years("M12L", None, None),
];

fn chinese_or_dangi_reference_year(calendar: Utf16View<'_>, month_code: Utf16View<'_>, day: u8) -> Option<i32> {
    let row = CHINESE_AND_DANGI_ISO_REFERENCE_YEARS
        .iter()
        .find(|row| month_code == row.month_code)
        .expect("the month code is a chinese or dangi month code");

    if (1..30).contains(&day) {
        if month_code == "M11L" {
            return Some(if day <= 10 { 2033 } else { 2034 });
        }
        return row.days_1_to_29;
    }

    if day == 30 {
        if month_code == "M03" {
            return Some(if calendar == "chinese" { 1966 } else { 1968 });
        }
        return row.day_30;
    }

    None
}

/// The calendar as the UTF-8 string LibUnicode takes it. Calendar identifiers are ASCII.
fn calendar_for_unicode(calendar: Utf16View<'_>) -> String {
    calendar.to_utf8()
}

pub fn canonicalize_calendar(vm: &Vm, id: Utf16View<'_>) -> ThrowCompletionOr<Utf16String> {
    if !id.is_ascii() {
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TemporalInvalidCalendarIdentifier,
            &[&id],
        );
    }

    let canonical = with_available_calendars(|calendars| {
        calendars
            .iter()
            .find(|calendar| Utf16View::of_string(calendar).equals_ignoring_ascii_case(id))
            .map(|calendar| {
                unicode_intl::canonicalize_unicode_extension_values(
                    Utf16View::Ascii(b"ca"),
                    Utf16View::of_string(calendar),
                )
            })
    });
    if let Some(canonical) = canonical {
        return Ok(canonical);
    }

    vm.throw_completion(
        ErrorKind::RangeError,
        ErrorType::TemporalInvalidCalendarIdentifier,
        &[&id],
    )
}

std::thread_local! {
    static AVAILABLE_CALENDARS: OnceCell<Vec<Utf16String>> = const { OnceCell::new() };
}

// 12.1.2 AvailableCalendars ( ), https://tc39.es/proposal-temporal/#sec-availablecalendars
// 1.1.1 AvailableCalendars ( ), https://tc39.es/proposal-intl-era-monthcode/#sup-availablecalendars
/// Calls `callback` with the available calendars, which are computed once per thread, as the C++ computes them once
/// per process.
pub fn with_available_calendars<R>(callback: impl FnOnce(&[Utf16String]) -> R) -> R {
    // The implementation-defined abstract operation AvailableCalendars takes no arguments and returns a List of calendar
    // types. The returned List is sorted according to lexicographic code unit order, and contains unique calendar types
    // in canonical form (12.1) identifying the calendars for which the implementation provides the functionality of
    // Intl.DateTimeFormat objects, including their aliases (e.g., both "islamicc" and "islamic-civil"). The List must
    // consist of the "Calendar Type" value of every row of Table 1, except the header row.
    AVAILABLE_CALENDARS.with(|calendars| {
        callback(calendars.get_or_init(|| {
            let mut calendars = unicode_intl::available_calendars();

            for calendar in CLDR_CALENDAR_TYPES {
                let calendar_string = Utf16String::from_utf8(calendar);
                if !calendars.contains(&calendar_string) {
                    calendars.push(calendar_string);
                }
            }

            calendars.sort_by(|lhs, rhs| {
                Utf16View::of_string(lhs)
                    .code_units()
                    .cmp(Utf16View::of_string(rhs).code_units())
            });
            calendars
        }))
    })
}

// 12.2.1 ParseMonthCode ( argument ), https://tc39.es/proposal-temporal/#sec-temporal-parsemonthcode
pub fn parse_month_code(vm: &Vm, argument: Value) -> ThrowCompletionOr<MonthCode> {
    // 1. Let monthCode be ? ToPrimitive(argument, STRING).
    let month_code = argument.to_primitive(vm, PreferredType::String)?;

    // 2. If monthCode is not a String, throw a TypeError exception.
    if !month_code.is_string() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAString, &[&month_code]);
    }

    let month_code = month_code.as_string().utf16_string();
    parse_month_code_of_string(vm, Utf16View::of_string(&month_code))
}

// 12.2.1 ParseMonthCode ( argument ), https://tc39.es/proposal-temporal/#sec-temporal-parsemonthcode
pub fn parse_month_code_of_string(vm: &Vm, month_code: Utf16View<'_>) -> ThrowCompletionOr<MonthCode> {
    // 3. If ParseText(StringToCodePoints(monthCode), MonthCode) is a List of errors, throw a RangeError exception.
    if let Some(result) = unicode_calendar::parse_month_code(month_code) {
        return Ok(result);
    }
    vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidMonthCode, &[])
}

// 12.3.3 PrepareCalendarFields ( calendar, fields, calendarFieldNames, nonCalendarFieldNames, requiredFieldNames ), https://tc39.es/proposal-temporal/#sec-temporal-preparecalendarfields
pub fn prepare_calendar_fields(
    vm: &Vm,
    calendar: Utf16View<'_>,
    fields: &Object,
    calendar_field_names: &[CalendarField],
    non_calendar_field_names: &[CalendarField],
    required_field_names: CalendarFieldListOrPartial<'_>,
) -> ThrowCompletionOr<CalendarFields> {
    // 1. Assert: If requiredFieldNames is a List, requiredFieldNames contains zero or one of each of the elements of
    //    calendarFieldNames and nonCalendarFieldNames.

    // 2. Let fieldNames be the list-concatenation of calendarFieldNames and nonCalendarFieldNames.
    let mut field_names = Vec::with_capacity(calendar_field_names.len() + non_calendar_field_names.len());
    field_names.extend_from_slice(calendar_field_names);
    field_names.extend_from_slice(non_calendar_field_names);

    // 3. Let extraFieldNames be CalendarExtraFields(calendar, calendarFieldNames).
    let extra_field_names = calendar_extra_fields(calendar, calendar_field_names);

    // 4. Set fieldNames to the list-concatenation of fieldNames and extraFieldNames.
    field_names.extend(extra_field_names);

    // 5. Assert: fieldNames contains no duplicate elements.

    // 6. Let result be a Calendar Fields Record with all fields equal to UNSET.
    let mut result = CalendarFields::unset();

    // 7. Let any be false.
    let mut any = false;

    // 8. Let sortedPropertyNames be a List whose elements are the values in the Property Key column of Table 19
    //    corresponding to the elements of fieldNames, sorted according to lexicographic code unit order.
    let sorted_property_names = sorted_calendar_fields(vm, &field_names);

    // 9. For each property name property of sortedPropertyNames, do
    for CalendarFieldData {
        key,
        property,
        conversion,
    } in &sorted_property_names
    {
        let key = *key;

        // a. Let key be the value in the Enumeration Key column of Table 19 corresponding to the row whose Property Key value is property.

        // b. Let value be ? Get(fields, property).
        let value = fields.get(vm, property)?;

        // c. If value is not undefined, then
        if !value.is_undefined() {
            // i. Set any to true.
            any = true;

            // ii. Let Conversion be the Conversion value of the same row.
            match conversion {
                // iii. If Conversion is TO-INTEGER-WITH-TRUNCATION, then
                CalendarFieldConversion::ToIntegerWithTruncation => {
                    // 1. Set value to ? ToIntegerWithTruncation(value).
                    // 2. Set value to 𝔽(value).
                    let integer = to_integer_with_truncation(
                        vm,
                        value,
                        ErrorType::TemporalInvalidCalendarFieldName,
                        &[property],
                    )?;
                    result.set_number_field(key, integer);
                }
                // iv. Else if Conversion is TO-POSITIVE-INTEGER-WITH-TRUNCATION, then
                CalendarFieldConversion::ToPositiveIntegerWithTruncation => {
                    // 1. Set value to ? ToPositiveIntegerWithTruncation(value).
                    // 2. Set value to 𝔽(value).
                    let integer = to_positive_integer_with_truncation(
                        vm,
                        value,
                        ErrorType::TemporalInvalidCalendarFieldName,
                        &[property],
                    )?;
                    result.set_number_field(key, integer);
                }
                // v. Else if Conversion is TO-STRING, then
                CalendarFieldConversion::ToString => {
                    // 1. Set value to ? ToString(value).
                    let string = value.to_utf16_string(vm)?;
                    result.set_string_field(key, string);
                }
                // vi. Else if Conversion is TO-TEMPORAL-TIME-ZONE-IDENTIFIER, then
                CalendarFieldConversion::ToTemporalTimeZoneIdentifier => {
                    // 1. Set value to ? ToTemporalTimeZoneIdentifier(value).
                    let time_zone = to_temporal_time_zone_identifier(vm, value)?;
                    result.set_string_field(key, time_zone);
                }
                // vii. Else if Conversion is TO-MONTH-CODE, then
                CalendarFieldConversion::ToMonthCode => {
                    // 1. Let parsed be ? ParseMonthCode(value).
                    let parsed = parse_month_code(vm, value)?;

                    // 2. Set value to CreateMonthCode(parsed.[[MonthNumber]], parsed.[[IsLeapMonth]]).
                    result.set_string_field(
                        key,
                        unicode_calendar::create_month_code(parsed.month_number, parsed.is_leap_month),
                    );
                }
                // viii. Else,
                CalendarFieldConversion::ToOffsetString => {
                    // 1. Assert: Conversion is TO-OFFSET-STRING.
                    // 2. Set value to ? ToOffsetString(value).
                    let offset_string = to_offset_string(vm, value)?;
                    result.set_string_field(key, offset_string);
                }
            }

            // ix. Set result's field whose name is given in the Field Name column of the same row to value.
        }
        // d. Else if requiredFieldNames is a List, then
        else if let CalendarFieldListOrPartial::List(required) = required_field_names {
            // i. If requiredFieldNames contains key, throw a TypeError exception.
            if required.contains(&key) {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::MissingRequiredProperty, &[property]);
            }

            // ii. Set result's field whose name is given in the Field Name column of the same row to the corresponding
            //     Default value of the same row.
            result.copy_field_from(key, &CalendarFields::default());
        }
    }

    // 10. If requiredFieldNames is PARTIAL and any is false, throw a TypeError exception.
    if matches!(required_field_names, CalendarFieldListOrPartial::Partial) && !any {
        return vm.throw_completion(
            ErrorKind::TypeError,
            ErrorType::TemporalObjectMustBePartialTemporalObject,
            &[],
        );
    }

    // 11. Return result.
    Ok(result)
}

// 12.3.4 CalendarFieldKeysPresent ( fields ), https://tc39.es/proposal-temporal/#sec-temporal-calendarfieldkeyspresent
pub fn calendar_field_keys_present(fields: &CalendarFields) -> Vec<CalendarField> {
    // 1. Let list be « ».
    // 2. For each row of Table 19, except the header row, do
    //     a. Let value be fields' field whose name is given in the Field Name column of the row.
    //     b. Let enumerationKey be the value in the Enumeration Key column of the row.
    //     c. If value is not unset, append enumerationKey to list.
    // 3. Return list.
    CALENDAR_FIELDS
        .iter()
        .copied()
        .filter(|&field| fields.has_field(field))
        .collect()
}

// 12.3.5 CalendarMergeFields ( calendar, fields, additionalFields ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmergefields
pub fn calendar_merge_fields(
    calendar: Utf16View<'_>,
    fields: &CalendarFields,
    additional_fields: &CalendarFields,
) -> CalendarFields {
    // 1. Let additionalKeys be CalendarFieldKeysPresent(additionalFields).
    let additional_keys = calendar_field_keys_present(additional_fields);

    // 2. Let overriddenKeys be CalendarFieldKeysToIgnore(calendar, additionalKeys).
    let overridden_keys = calendar_field_keys_to_ignore(calendar, &additional_keys);

    // 3. Let merged be a Calendar Fields Record with all fields set to unset.
    let mut merged = CalendarFields::unset();

    // 4. Let fieldsKeys be CalendarFieldKeysPresent(fields).
    let fields_keys = calendar_field_keys_present(fields);

    // 5. For each row of Table 19, except the header row, do
    for key in CALENDAR_FIELDS {
        // a. Let key be the value in the Enumeration Key column of the row.

        // b. If fieldsKeys contains key and overriddenKeys does not contain key, then
        if fields_keys.contains(&key) && !overridden_keys.contains(&key) {
            // i. Let propValue be fields' field whose name is given in the Field Name column of the row.
            // ii. Set merged's field whose name is given in the Field Name column of the row to propValue.
            merged.copy_field_from(key, fields);
        }

        // c. If additionalKeys contains key, then
        if additional_keys.contains(&key) {
            // i. Let propValue be additionalFields' field whose name is given in the Field Name column of the row.
            // ii. Set merged's field whose name is given in the Field Name column of the row to propValue.
            merged.copy_field_from(key, additional_fields);
        }
    }

    // 6. Return merged.
    merged
}

// 12.3.6 NonISODateAdd ( calendar, isoDate, duration, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-nonisodateadd
// 4.1.18 NonISODateAdd ( calendar, isoDate, duration, overflow ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-nonisodateadd
pub fn non_iso_date_add(
    vm: &Vm,
    calendar: Utf16View<'_>,
    iso_date: ISODate,
    duration: &DateDuration,
    overflow: Overflow,
) -> ThrowCompletionOr<ISODate> {
    // 1. Let parts be CalendarISOToDate(calendar, isoDate).
    let parts = non_iso_calendar_iso_to_date(calendar, iso_date);

    // 2. Let y0 be parts.[[Year]] + duration.[[Years]].
    let y0 = parts.year.wrapping_add(duration.years as i32);

    // 3. Let m0 be MonthCodeToOrdinal(calendar, y0, ? ConstrainMonthCode(calendar, y0, parts.[[MonthCode]], overflow)).
    let constrained_month_code = constrain_month_code(vm, calendar, y0, &parts.month_code, overflow)?;
    let m0 = month_code_to_ordinal(calendar, y0, Utf16View::of_string(&constrained_month_code));

    // 4. Let endOfMonth be BalanceNonISODate(calendar, y0, m0 + duration.[[Months]] + 1, 0).
    let end_of_month = balance_non_iso_date(calendar, y0, (f64::from(m0) + duration.months + 1.0) as i32, 0);

    // 5. Let baseDay be parts.[[Day]].
    let base_day = parts.day;

    // 6. If baseDay ≤ endOfMonth.[[Day]], then
    let regulated_day = if base_day <= end_of_month.day {
        // a. Let regulatedDay be baseDay.
        base_day
    }
    // 7. Else,
    else {
        // a. If overflow is REJECT, throw a RangeError exception.
        if overflow == Overflow::Reject {
            return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidISODate, &[]);
        }

        // b. Let regulatedDay be endOfMonth.[[Day]].
        end_of_month.day
    };

    // 8. Let balancedDate be BalanceNonISODate(calendar, endOfMonth.[[Year]], endOfMonth.[[Month]], regulatedDay + 7 * duration.[[Weeks]] + duration.[[Days]]).
    let balanced_date = balance_non_iso_date(
        calendar,
        end_of_month.year,
        i32::from(end_of_month.month),
        (f64::from(regulated_day) + (7.0 * duration.weeks) + duration.days) as i32,
    );

    // 9. Let result be ? CalendarIntegersToISO(calendar, balancedDate.[[Year]], balancedDate.[[Month]], balancedDate.[[Day]]).
    let result = calendar_integers_to_iso(vm, calendar, balanced_date.year, balanced_date.month, balanced_date.day)?;

    // 10. If ISODateWithinLimits(result) is false, throw a RangeError exception.
    // NB: This is handled by the caller, CalendarDateAdd.

    // 11. Return result.
    Ok(result)
}

// 12.3.7 CalendarDateAdd ( calendar, isoDate, duration, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendardateadd
pub fn calendar_date_add(
    vm: &Vm,
    calendar: Utf16View<'_>,
    iso_date: ISODate,
    duration: &DateDuration,
    overflow: Overflow,
) -> ThrowCompletionOr<ISODate> {
    // 1. If calendar is "iso8601", then
    let result = if calendar == ISO8601_CALENDAR {
        // a. Let intermediate be BalanceISOYearMonth(isoDate.[[Year]] + duration.[[Years]], isoDate.[[Month]] + duration.[[Months]]).
        let intermediate = balance_iso_year_month(
            f64::from(iso_date.year) + duration.years,
            f64::from(iso_date.month) + duration.months,
        );

        // b. Set intermediate to ? RegulateISODate(intermediate.[[Year]], intermediate.[[Month]], isoDate.[[Day]], overflow).
        let intermediate_date = regulate_iso_date(
            vm,
            f64::from(intermediate.year),
            f64::from(intermediate.month),
            f64::from(iso_date.day),
            overflow,
        )?;

        // c. Let days be duration.[[Days]] + 7 × duration.[[Weeks]].
        let days = duration.days + (7.0 * duration.weeks);

        // d. Let result be AddDaysToISODate(intermediate, days).
        add_days_to_iso_date(intermediate_date, days)
    }
    // 2. Else,
    else {
        // a. Let result be ? NonISODateAdd(calendar, isoDate, duration, overflow).
        non_iso_date_add(vm, calendar, iso_date, duration, overflow)?
    };

    // 3. If ISODateWithinLimits(result) is false, throw a RangeError exception.
    if !iso_date_within_limits(result) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidISODate, &[]);
    }

    // 4. Return result.
    Ok(result)
}

// 12.3.8 NonISODateUntil ( calendar, one, two, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-nonisodateuntil
// 4.1.19 NonISODateUntil ( calendar, one, two, largestUnit ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-nonisodateuntil
pub fn non_iso_date_until(
    vm: &Vm,
    calendar: Utf16View<'_>,
    one: ISODate,
    two: ISODate,
    largest_unit: Unit,
) -> DateDuration {
    // 1. Let sign be -1 × CompareISODate(one, two).
    let sign = -compare_iso_date(one, two);

    // 2. If sign = 0, return ZeroDateDuration().
    if sign == 0 {
        return zero_date_duration(vm);
    }

    // OPTIMIZATION: For DAY and WEEK largest units, calendar days equal ISO days. We can compute the difference
    //               directly from ISO epoch days without any calendar arithmetic.
    if largest_unit == Unit::Day || largest_unit == Unit::Week {
        let mut days = iso_date_to_epoch_days(f64::from(two.year), f64::from(two.month) - 1.0, f64::from(two.day))
            - iso_date_to_epoch_days(f64::from(one.year), f64::from(one.month) - 1.0, f64::from(one.day));
        let mut weeks = 0.0;

        if largest_unit == Unit::Week {
            weeks = (days / 7.0).trunc();
            days %= 7.0;
        }

        return create_date_duration_record(vm, 0.0, 0.0, weeks, days).must();
    }

    // OPTIMIZATION: Pre-compute calendar dates for `from` and `to` to avoid expensive redundant conversions.
    let calendar_one = non_iso_calendar_iso_to_date(calendar, one);
    let calendar_two = non_iso_calendar_iso_to_date(calendar, two);

    // 3. Let years be 0.
    let mut years = 0.0;

    // OPTIMIZATION: If the largestUnit is MONTH, we want to skip ahead to the correct year. If implemented in exact
    //               accordance with the spec, we could enter the second NonISODateSurpasses loop below with a very
    //               large number of months to traverse.

    // 4. If largestUnit is YEAR, then
    if largest_unit == Unit::Year {
        // OPTIMIZATION: Skip ahead by estimating the year difference from calendar dates to avoid a large number of
        //               iterations in the NonISODateSurpasses loop below.
        let mut estimated_years = calendar_two.year.wrapping_sub(calendar_one.year);
        if estimated_years != 0 {
            estimated_years -= i32::from(sign);
        }

        // a. Let candidateYears be sign.
        let mut candidate_years = if estimated_years != 0 {
            f64::from(estimated_years)
        } else {
            f64::from(sign)
        };

        // b. Repeat, while NonISODateSurpasses(calendar, sign, one, two, candidateYears, 0, 0, 0) is false,
        while !non_iso_date_surpasses(
            vm,
            calendar,
            sign,
            &calendar_one,
            &calendar_two,
            candidate_years,
            0.0,
            0.0,
            0.0,
        ) {
            // i. Set years to candidateYears.
            years = candidate_years;

            // ii. Set candidateYears to candidateYears + sign.
            candidate_years += f64::from(sign);
        }
    }

    // 5. Let months be 0.
    let mut months = 0.0;

    // 6. If largestUnit is YEAR or largestUnit is MONTH, then
    if largest_unit == Unit::Year || largest_unit == Unit::Month {
        // a. Let candidateMonths be sign.
        let mut candidate_months = f64::from(sign);

        // OPTIMIZATION: Skip ahead by estimating the month difference from calendar dates to avoid a large number of
        //               iterations in the NonISODateSurpasses loop below.
        if largest_unit == Unit::Month {
            let mut estimated_months = (calendar_two.year.wrapping_sub(calendar_one.year))
                .wrapping_mul(12)
                .wrapping_add(i32::from(calendar_two.month) - i32::from(calendar_one.month));
            if estimated_months != 0 {
                estimated_months -= i32::from(sign);
            }

            if estimated_months != 0 {
                candidate_months = f64::from(estimated_months);
            }
        }

        // b. Repeat, while NonISODateSurpasses(calendar, sign, one, two, years, candidateMonths, 0, 0) is false,
        while !non_iso_date_surpasses(
            vm,
            calendar,
            sign,
            &calendar_one,
            &calendar_two,
            years,
            candidate_months,
            0.0,
            0.0,
        ) {
            // i. Set months to candidateMonths.
            months = candidate_months;

            // ii. Set candidateMonths to candidateMonths + sign.
            candidate_months += f64::from(sign);
        }
    }

    // 9. Let days be 0.
    let mut days = 0.0;

    // 10. Let candidateDays be sign.
    let mut candidate_days = f64::from(sign);

    // 11. Repeat, while NonISODateSurpasses(calendar, sign, one, two, years, months, weeks, candidateDays) is false,
    while !non_iso_date_surpasses(
        vm,
        calendar,
        sign,
        &calendar_one,
        &calendar_two,
        years,
        months,
        0.0,
        candidate_days,
    ) {
        // a. Set days to candidateDays.
        days = candidate_days;

        // b. Set candidateDays to candidateDays + sign.
        candidate_days += f64::from(sign);
    }

    // 12. Return ! CreateDateDurationRecord(years, months, weeks, days).
    create_date_duration_record(vm, years, months, 0.0, days).must()
}

// 12.3.9 CalendarDateUntil ( calendar, one, two, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-calendardateuntil
pub fn calendar_date_until(
    vm: &Vm,
    calendar: Utf16View<'_>,
    one: ISODate,
    two: ISODate,
    largest_unit: Unit,
) -> DateDuration {
    // 1. Let sign be CompareISODate(one, two).
    let mut sign = compare_iso_date(one, two);

    // 2. If sign = 0, return ZeroDateDuration().
    if sign == 0 {
        return zero_date_duration(vm);
    }

    // 3. If calendar is "iso8601", then
    if calendar == ISO8601_CALENDAR {
        // a. Set sign to -sign.
        sign = -sign;

        // b. Let years be 0.
        let mut years = 0.0;

        // d. Let months be 0.
        let mut months = 0.0;

        // OPTIMIZATION: If the largestUnit is MONTH, we want to skip ahead to the correct year. If implemented in exact
        //               accordance with the spec, we could enter the second ISODateSurpasses loop below with a very large
        //               number of months to traverse.

        // c. If largestUnit is YEAR, then
        // e. If largestUnit is either YEAR or MONTH, then
        if largest_unit == Unit::Year || largest_unit == Unit::Month {
            // c.i. Let candidateYears be sign.
            let mut candidate_years = two.year.wrapping_sub(one.year);
            if candidate_years != 0 {
                candidate_years -= i32::from(sign);
            }

            // c.ii. Repeat, while ISODateSurpasses(sign, one, two, candidateYears, 0, 0, 0) is false,
            while !iso_date_surpasses(vm, sign, one, two, f64::from(candidate_years), 0.0, 0.0, 0.0) {
                // 1. Set years to candidateYears.
                years = f64::from(candidate_years);

                // 2. Set candidateYears to candidateYears + sign.
                candidate_years += i32::from(sign);
            }

            // e.i. Let candidateMonths be sign.
            let mut candidate_months = f64::from(sign);

            // e.ii. Repeat, while ISODateSurpasses(sign, one, two, years, candidateMonths, 0, 0) is false,
            while !iso_date_surpasses(vm, sign, one, two, years, candidate_months, 0.0, 0.0) {
                // 1. Set months to candidateMonths.
                months = candidate_months;

                // 2. Set candidateMonths to candidateMonths + sign.
                candidate_months += f64::from(sign);
            }

            if largest_unit == Unit::Month {
                months += years * 12.0;
                years = 0.0;
            }
        }

        // f. Let weeks be 0.
        let mut weeks = 0.0;

        // OPTIMIZATION: If the largestUnit is DAY, we do not want to enter an ISODateSurpasses loop. The loop would have
        //               us increment the intermediate ISOYearMonth one day at time, which will take an extremely long
        //               time if the difference is a large number of years. Instead, we can compute the day difference,
        //               and convert to weeks if needed.
        let year_month = balance_iso_year_month(f64::from(one.year) + years, f64::from(one.month) + months);
        let regulated_date = regulate_iso_date(
            vm,
            f64::from(year_month.year),
            f64::from(year_month.month),
            f64::from(one.day),
            Overflow::Constrain,
        )
        .must();

        let mut days = iso_date_to_epoch_days(f64::from(two.year), f64::from(two.month) - 1.0, f64::from(two.day))
            - iso_date_to_epoch_days(
                f64::from(regulated_date.year),
                f64::from(regulated_date.month) - 1.0,
                f64::from(regulated_date.day),
            );

        if largest_unit == Unit::Week {
            weeks = (days / 7.0).trunc();
            days %= 7.0;
        }

        // k. Return ! CreateDateDurationRecord(years, months, weeks, days).
        return create_date_duration_record(vm, years, months, weeks, days).must();
    }

    // 4. Return NonISODateUntil(calendar, one, two, largestUnit).
    non_iso_date_until(vm, calendar, one, two, largest_unit)
}

// 12.3.10 ToTemporalCalendarIdentifier ( temporalCalendarLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalcalendaridentifier
pub fn to_temporal_calendar_identifier(vm: &Vm, temporal_calendar_like: Value) -> ThrowCompletionOr<Utf16String> {
    // 1. If temporalCalendarLike is an Object and temporalCalendarLike has an [[InitializedTemporalDate]],
    //    [[InitializedTemporalDateTime]], [[InitializedTemporalMonthDay]], [[InitializedTemporalYearMonth]], or
    //    [[InitializedTemporalZonedDateTime]] internal slot, return temporalCalendarLike.[[Calendar]].
    if temporal_calendar_like.is_object()
        && let Some(calendar) = calendar_of_temporal_object(&temporal_calendar_like.as_object())
    {
        return Ok(calendar);
    }

    // 2. If temporalCalendarLike is not a String, throw a TypeError exception.
    if !temporal_calendar_like.is_string() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::TemporalInvalidCalendar, &[]);
    }

    // 3. Let identifier be ? ParseTemporalCalendarString(temporalCalendarLike).
    let temporal_calendar_like = temporal_calendar_like.as_string().utf16_string();
    let identifier = parse_temporal_calendar_string(vm, Utf16View::of_string(&temporal_calendar_like))?;

    // 4. Return ? CanonicalizeCalendar(identifier).
    canonicalize_calendar(vm, Utf16View::of_string(&identifier))
}

/// The [[Calendar]] of an object with an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]],
/// [[InitializedTemporalMonthDay]], [[InitializedTemporalYearMonth]], or [[InitializedTemporalZonedDateTime]] slot.
fn calendar_of_temporal_object(object: &Object) -> Option<Utf16String> {
    let object = object.as_gc();
    if let Some(plain_date) = object.downcast::<PlainDate>() {
        return Some(plain_date.calendar());
    }
    if let Some(plain_date_time) = object.downcast::<PlainDateTime>() {
        return Some(plain_date_time.calendar());
    }
    if let Some(plain_month_day) = object.downcast::<PlainMonthDay>() {
        return Some(plain_month_day.calendar());
    }
    if let Some(plain_year_month) = object.downcast::<PlainYearMonth>() {
        return Some(plain_year_month.calendar());
    }
    if let Some(zoned_date_time) = object.downcast::<ZonedDateTime>() {
        return Some(zoned_date_time.calendar());
    }
    None
}

// 12.3.11 GetTemporalCalendarIdentifierWithISODefault ( item ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalcalendarslotvaluewithisodefault
pub fn get_temporal_calendar_identifier_with_iso_default(vm: &Vm, item: &Object) -> ThrowCompletionOr<Utf16String> {
    // 1. If item has an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], [[InitializedTemporalMonthDay]],
    //    [[InitializedTemporalYearMonth]], or [[InitializedTemporalZonedDateTime]] internal slot, then
    //     a. Return item.[[Calendar]].
    if let Some(calendar) = calendar_of_temporal_object(item) {
        return Ok(calendar);
    }

    // 2. Let calendarLike be ? Get(item, "calendar").
    let calendar_like = item.get(vm, &vm.names.calendar)?;

    // 3. If calendarLike is undefined, return "iso8601".
    if calendar_like.is_undefined() {
        return Ok(Utf16String::from_utf8(ISO8601_CALENDAR));
    }

    // 4. Return ? ToTemporalCalendarIdentifier(calendarLike).
    to_temporal_calendar_identifier(vm, calendar_like)
}

// 12.3.12 CalendarDateFromFields ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendardatefromfields
pub fn calendar_date_from_fields(
    vm: &Vm,
    calendar: Utf16View<'_>,
    fields: &mut CalendarFields,
    overflow: Overflow,
) -> ThrowCompletionOr<ISODate> {
    // 1. Perform ? CalendarResolveFields(calendar, fields, DATE).
    calendar_resolve_fields(vm, calendar, fields, DateType::Date)?;

    // 2. Let result be ? CalendarDateToISO(calendar, fields, overflow).
    let result = calendar_date_to_iso(vm, calendar, fields, overflow)?;

    // 3. If ISODateWithinLimits(result) is false, throw a RangeError exception.
    if !iso_date_within_limits(result) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidISODate, &[]);
    }

    // 4. Return result.
    Ok(result)
}

// 12.3.13 CalendarYearMonthFromFields ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendaryearmonthfromfields
pub fn calendar_year_month_from_fields(
    vm: &Vm,
    calendar: Utf16View<'_>,
    fields: &mut CalendarFields,
    overflow: Overflow,
) -> ThrowCompletionOr<ISODate> {
    // 1. Set fields.[[Day]] to 1.
    fields.day = Some(1);

    // 2. Perform ? CalendarResolveFields(calendar, fields, YEAR-MONTH).
    calendar_resolve_fields(vm, calendar, fields, DateType::YearMonth)?;

    // 3. Let result be ? CalendarDateToISO(calendar, fields, overflow).
    let result = calendar_date_to_iso(vm, calendar, fields, overflow)?;

    // 4. If ISOYearMonthWithinLimits(result) is false, throw a RangeError exception.
    if !iso_year_month_within_limits(result) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidISODate, &[]);
    }

    // 5. Return result.
    Ok(result)
}

// 12.3.14 CalendarMonthDayFromFields ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmonthdayfromfields
pub fn calendar_month_day_from_fields(
    vm: &Vm,
    calendar: Utf16View<'_>,
    fields: &mut CalendarFields,
    overflow: Overflow,
) -> ThrowCompletionOr<ISODate> {
    // 1. Perform ? CalendarResolveFields(calendar, fields, MONTH-DAY).
    calendar_resolve_fields(vm, calendar, fields, DateType::MonthDay)?;

    // 2. Let result be ? CalendarMonthDayToISOReferenceDate(calendar, fields, overflow).
    let result = calendar_month_day_to_iso_reference_date(vm, calendar, fields, overflow)?;

    // 3. Assert: ISODateWithinLimits(result) is true.
    assert!(iso_date_within_limits(result));

    // 4. Return result.
    Ok(result)
}

// 12.3.15 FormatCalendarAnnotation ( id, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-formatcalendarannotation
pub fn format_calendar_annotation(id: Utf16View<'_>, show_calendar: ShowCalendar) -> String {
    // 1. If showCalendar is NEVER, return the empty String.
    if show_calendar == ShowCalendar::Never {
        return String::new();
    }

    // 2. If showCalendar is AUTO and id is "iso8601", return the empty String.
    if show_calendar == ShowCalendar::Auto && id == ISO8601_CALENDAR {
        return String::new();
    }

    // 3. If showCalendar is CRITICAL, let flag be "!"; else, let flag be the empty String.
    let flag = if show_calendar == ShowCalendar::Critical {
        "!"
    } else {
        ""
    };

    // 4. Return the string-concatenation of "[", flag, "u-ca=", id, and "]".
    format!("[{flag}u-ca={}]", id.to_utf8())
}

// 12.3.16 CalendarEquals ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-calendarequals
pub fn calendar_equals(one: Utf16View<'_>, two: Utf16View<'_>) -> bool {
    // 1. If CanonicalizeUValue("ca", one) is CanonicalizeUValue("ca", two), return true.
    // 2. Return false.
    unicode_intl::canonicalize_unicode_extension_values(Utf16View::Ascii(b"ca"), one)
        == unicode_intl::canonicalize_unicode_extension_values(Utf16View::Ascii(b"ca"), two)
}

// 12.3.17 ISODaysInMonth ( year, month ), https://tc39.es/proposal-temporal/#sec-temporal-isodaysinmonth
pub fn iso_days_in_month(year: f64, month: f64) -> u8 {
    // 1. If month is one of 1, 3, 5, 7, 8, 10, or 12, return 31.
    if [1.0, 3.0, 5.0, 7.0, 8.0, 10.0, 12.0].contains(&month) {
        return 31;
    }

    // 2. If month is one of 4, 6, 9, or 11, return 30.
    if [4.0, 6.0, 9.0, 11.0].contains(&month) {
        return 30;
    }

    // 3. Assert: month is 2.
    assert!(month == 2.0);

    // 4. Return 28 + MathematicalInLeapYear(EpochTimeForYear(year)).
    28 + mathematical_in_leap_year(epoch_time_for_year(year))
}

// 12.3.18 ISOWeekOfYear ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isoweekofyear
pub fn iso_week_of_year(iso_date: ISODate) -> YearWeek {
    // 1. Let year be isoDate.[[Year]].
    let year = iso_date.year;

    // 2. Let wednesday be 3.
    const WEDNESDAY: i32 = 3;

    // 3. Let thursday be 4.
    const THURSDAY: i32 = 4;

    // 4. Let friday be 5.
    const FRIDAY: u8 = 5;

    // 5. Let saturday be 6.
    const SATURDAY: u8 = 6;

    // 6. Let daysInWeek be 7.
    const DAYS_IN_WEEK: i32 = 7;

    // 7. Let maxWeekNumber be 53.
    const MAX_WEEK_NUMBER: u8 = 53;

    // 8. Let dayOfYear be ISODayOfYear(isoDate).
    let day_of_year = iso_day_of_year(iso_date);

    // 9. Let dayOfWeek be ISODayOfWeek(isoDate).
    let day_of_week = iso_day_of_week(iso_date);

    // 10. Let week be floor((dayOfYear + daysInWeek - dayOfWeek + wednesday) / daysInWeek).
    let week = (f64::from(i32::from(day_of_year) + DAYS_IN_WEEK - i32::from(day_of_week) + WEDNESDAY)
        / f64::from(DAYS_IN_WEEK))
    .floor();

    // 11. If week < 1, then
    if week < 1.0 {
        // a. NOTE: This is the last week of the previous year.

        // b. Let jan1st be CreateISODateRecord(year, 1, 1).
        let jan1st = create_iso_date_record(f64::from(year), 1.0, 1.0);

        // c. Let dayOfJan1st be ISODayOfWeek(jan1st).
        let day_of_jan1st = iso_day_of_week(jan1st);

        // d. If dayOfJan1st = friday, then
        if day_of_jan1st == FRIDAY {
            // i. Return Year-Week Record { [[Week]]: maxWeekNumber, [[Year]]: year - 1 }.
            return YearWeek {
                week: Some(MAX_WEEK_NUMBER),
                year: Some(year - 1),
            };
        }

        // e. If dayOfJan1st = saturday, and MathematicalInLeapYear(EpochTimeForYear(year - 1)) = 1, then
        if day_of_jan1st == SATURDAY && mathematical_in_leap_year(epoch_time_for_year(f64::from(year - 1))) == 1 {
            // i. Return Year-Week Record { [[Week]]: maxWeekNumber. [[Year]]: year - 1 }.
            return YearWeek {
                week: Some(MAX_WEEK_NUMBER),
                year: Some(year - 1),
            };
        }

        // f. Return Year-Week Record { [[Week]]: maxWeekNumber - 1, [[Year]]: year - 1 }.
        return YearWeek {
            week: Some(MAX_WEEK_NUMBER - 1),
            year: Some(year - 1),
        };
    }

    // 12. If week = maxWeekNumber, then
    if week == f64::from(MAX_WEEK_NUMBER) {
        // a. Let daysInYear be MathematicalDaysInYear(year).
        let days_in_year = i32::from(mathematical_days_in_year(year));

        // b. Let daysLaterInYear be daysInYear - dayOfYear.
        let days_later_in_year = days_in_year - i32::from(day_of_year);

        // c. Let daysAfterThursday be thursday - dayOfWeek.
        let days_after_thursday = THURSDAY - i32::from(day_of_week);

        // d. If daysLaterInYear < daysAfterThursday, then
        if days_later_in_year < days_after_thursday {
            // i. Return Year-Week Record { [[Week]]: 1, [[Year]]: year + 1 }.
            return YearWeek {
                week: Some(1),
                year: Some(year + 1),
            };
        }
    }

    // 13. Return Year-Week Record { [[Week]]: week, [[Year]]: year }.
    YearWeek {
        week: Some(week as u8),
        year: Some(year),
    }
}

// 12.3.19 ISODayOfYear ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isodayofyear
pub fn iso_day_of_year(iso_date: ISODate) -> u16 {
    // 1. Let epochDays be ISODateToEpochDays(isoDate.[[Year]], isoDate.[[Month]] - 1, isoDate.[[Day]]).
    let epoch_days = iso_date_to_epoch_days(
        f64::from(iso_date.year),
        f64::from(iso_date.month) - 1.0,
        f64::from(iso_date.day),
    );

    // 2. Return EpochTimeToDayInYear(EpochDaysToEpochMs(epochDays, 0)) + 1.
    epoch_time_to_day_in_year(epoch_days_to_epoch_ms(epoch_days, 0.0)) + 1
}

// 12.3.20 ISODayOfWeek ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isodayofweek
pub fn iso_day_of_week(iso_date: ISODate) -> u8 {
    // 1. Let epochDays be ISODateToEpochDays(isoDate.[[Year]], isoDate.[[Month]] - 1, isoDate.[[Day]]).
    let epoch_days = iso_date_to_epoch_days(
        f64::from(iso_date.year),
        f64::from(iso_date.month) - 1.0,
        f64::from(iso_date.day),
    );

    // 2. Let dayOfWeek be EpochTimeToWeekDay(EpochDaysToEpochMs(epochDays, 0)).
    let day_of_week = epoch_time_to_week_day(epoch_days_to_epoch_ms(epoch_days, 0.0));

    // 3. If dayOfWeek = 0, return 7.
    if day_of_week == 0 {
        return 7;
    }

    // 4. Return dayOfWeek.
    day_of_week
}

// 12.3.21 NonISOCalendarDateToISO ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-nonisocalendardatetoiso
// 4.1.20 NonISOCalendarDateToISO ( calendar, fields, overflow ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-nonisocalendardatetoiso
pub fn non_iso_calendar_date_to_iso(
    vm: &Vm,
    calendar: Utf16View<'_>,
    fields: &CalendarFields,
    overflow: Overflow,
) -> ThrowCompletionOr<ISODate> {
    // 1. Assert: fields.[[Year]], fields.[[Month]], and fields.[[Day]] are not UNSET.
    let fields_year = fields.year.expect("the year is resolved");
    let fields_month = fields.month.expect("the month is resolved");
    let fields_day = fields.day.expect("the day is resolved");

    // 2. If fields.[[MonthCode]] is not UNSET, then
    if let Some(month_code) = &fields.month_code {
        // a. Perform ? ConstrainMonthCode(calendar, fields.[[Year]], fields.[[MonthCode]], overflow).
        constrain_month_code(vm, calendar, fields_year, month_code, overflow)?;
    }

    // 3. Let monthsInYear be CalendarMonthsInYear(calendar, fields.[[Year]]).
    let months_in_year = calendar_months_in_year(calendar, fields_year);

    // 4. If fields.[[Month]] > monthsInYear, then
    let month = if fields_month > u32::from(months_in_year) {
        // a. If overflow is REJECT, throw a RangeError exception.
        if overflow == Overflow::Reject {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::TemporalInvalidCalendarFieldName,
                &[&"month"],
            );
        }

        // b. Let month be monthsInYear.
        months_in_year
    }
    // 5. Else,
    else {
        // a. Let month be fields.[[Month]].
        fields_month as u8
    };

    // 6. Let daysInMonth be CalendarDaysInMonth(calendar, fields.[[Year]], fields.[[Month]]).
    // FIXME: Spec issue: We should use the `month` value that we just constrained.
    let days_in_month = calendar_days_in_month(calendar, fields_year, month);

    // 7. If fields.[[Day]] > daysInMonth, then
    let day = if fields_day > u32::from(days_in_month) {
        // a. If overflow is REJECT, throw a RangeError exception.
        if overflow == Overflow::Reject {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::TemporalInvalidCalendarFieldName,
                &[&"day"],
            );
        }

        // b. Let day be daysInMonth.
        days_in_month
    }
    // 8. Else,
    else {
        // a. Let day be fields.[[Day]].
        fields_day as u8
    };

    // 9. Return ? CalendarIntegersToISO(calendar, fields.[[Year]], month, day).
    calendar_integers_to_iso(vm, calendar, fields_year, month, day)
}

// 12.3.22 CalendarDateToISO ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendardatetoiso
pub fn calendar_date_to_iso(
    vm: &Vm,
    calendar: Utf16View<'_>,
    fields: &CalendarFields,
    overflow: Overflow,
) -> ThrowCompletionOr<ISODate> {
    // 1. If calendar is "iso8601", then
    if calendar == ISO8601_CALENDAR {
        // a. Assert: fields.[[Year]], fields.[[Month]], and fields.[[Day]] are not UNSET.
        let year = fields.year.expect("the year is resolved");
        let month = fields.month.expect("the month is resolved");
        let day = fields.day.expect("the day is resolved");

        // b. Return ? RegulateISODate(fields.[[Year]], fields.[[Month]], fields.[[Day]], overflow).
        return regulate_iso_date(vm, f64::from(year), f64::from(month), f64::from(day), overflow);
    }

    // 2. Return ? NonISOCalendarDateToISO(calendar, fields, overflow).
    non_iso_calendar_date_to_iso(vm, calendar, fields, overflow)
}

// 12.3.23 NonISOMonthDayToISOReferenceDate ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-nonisomonthdaytoisoreferencedate
// 4.1.21 NonISOMonthDayToISOReferenceDate ( calendar, fields, overflow ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-nonisomonthdaytoisoreferencedate
pub fn non_iso_month_day_to_iso_reference_date(
    vm: &Vm,
    calendar: Utf16View<'_>,
    fields: &CalendarFields,
    overflow: Overflow,
) -> ThrowCompletionOr<ISODate> {
    // 1. Assert: fields.[[Day]] is not UNSET.
    let fields_day = fields.day.expect("the day is resolved");

    let days_in_month: u8;
    let mut month_code: Utf16String;

    // 2. If fields.[[Year]] is not UNSET, then
    if let Some(fields_year) = fields.year {
        // a. Assert: fields.[[Month]] is not UNSET.
        let fields_month = fields.month.expect("the month is resolved");

        // b. If there exists no combination of inputs such that ! CalendarIntegersToISO(calendar, fields.[[Year]], ..., ...)
        //    would return an ISO Date Record isoDate for which ISODateWithinLimits(isoDate) is true, throw a RangeError exception.
        // c. NOTE: The above step exists so as not to require calculating whether the month and day described in fields
        //    exist in user-provided years arbitrarily far in the future or past.
        match calendar_integers_to_iso(vm, calendar, fields_year, 1, 1) {
            Ok(iso_date) if iso_date_within_limits(iso_date) => {}
            _ => return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidISODate, &[]),
        }

        // d. Let monthsInYear be CalendarMonthsInYear(calendar, fields.[[Year]]).
        let months_in_year = calendar_months_in_year(calendar, fields_year);

        // e. If fields.[[Month]] > monthsInYear, then
        let month = if fields_month > u32::from(months_in_year) {
            // i. If overflow is REJECT, throw a RangeError exception.
            if overflow == Overflow::Reject {
                return vm.throw_completion(
                    ErrorKind::RangeError,
                    ErrorType::TemporalInvalidCalendarFieldName,
                    &[&"month"],
                );
            }

            // ii. Let month be monthsInYear.
            months_in_year
        }
        // f. Else,
        else {
            // i. Let month be fields.[[Month]].
            fields_month as u8
        };

        // g. If fields.[[MonthCode]] is UNSET, then
        month_code = match &fields.month_code {
            None => {
                // i. Let fieldsISODate be ! CalendarIntegersToISO(calendar, fields.[[Year]], month, 1).
                let fields_iso_date = calendar_integers_to_iso(vm, calendar, fields_year, month, 1).must();

                // ii. Let monthCode be NonISOCalendarISOToDate(calendar, fieldsISODate).[[MonthCode]].
                non_iso_calendar_iso_to_date(calendar, fields_iso_date).month_code
            }
            // h. Else,
            Some(fields_month_code) => {
                // i. Let monthCode be ? ConstrainMonthCode(calendar, fields.[[Year]], fields.[[MonthCode]], overflow).
                constrain_month_code(vm, calendar, fields_year, fields_month_code, overflow)?
            }
        };

        // i. Let daysInMonth be CalendarDaysInMonth(calendar, fields.[[Year]], month).
        days_in_month = calendar_days_in_month(calendar, fields_year, month);
    }
    // 3. Else,
    else {
        // a. Assert: fields.[[MonthCode]] is not UNSET.
        // b. Let monthCode be fields.[[MonthCode]].
        month_code = fields.month_code.clone().expect("the month code is resolved");

        // c. If calendar is "chinese" or "dangi", let daysInMonth be 30; else, let daysInMonth be the maximum number of
        //    days in the month described by monthCode in any year.
        days_in_month = if calendar == "chinese" || calendar == "dangi" {
            30
        } else {
            unicode_calendar::calendar_max_days_in_month_code(
                &calendar_for_unicode(calendar),
                Utf16View::of_string(&month_code),
            )
        };
    }

    // 4. If fields.[[Day]] > daysInMonth, then
    let day = if fields_day > u32::from(days_in_month) {
        // a. If overflow is REJECT, throw a RangeError exception.
        if overflow == Overflow::Reject {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::TemporalInvalidCalendarFieldName,
                &[&"day"],
            );
        }

        // b. Let day be daysInMonth.
        days_in_month
    }
    // 5. Else,
    else {
        // a. Let day be fields.[[Day]].
        fields_day as u8
    };

    let is_chinese_or_dangi = calendar == "chinese" || calendar == "dangi";

    // 6. If calendar is "chinese" or "dangi", then
    if is_chinese_or_dangi {
        // a. NOTE: This special case handles combinations of month and day that theoretically could occur but are not
        //    known to have occurred historically and cannot be accurately calculated to occur in the future, even if it
        //    may be possible to construct a PlainDate with such combinations due to inaccurate approximations. This is
        //    explicitly mentioned here because as time goes on, these dates may become known to have occurred
        //    historically, or may be more accurately calculated to occur in the future.

        // b. Let row be the row in Table 6 with a value in the "Month Code" column matching monthCode.
        let reference_year = chinese_or_dangi_reference_year(calendar, Utf16View::of_string(&month_code), day);

        // c. If the "Reference Year (Days 1-29)" column of row is "—", or day = 30 and the "Reference Year (Day 30)"
        //    column of row is "—", then
        if reference_year.is_none() {
            // i. If overflow is REJECT, throw a RangeError exception.
            if overflow == Overflow::Reject {
                return vm.throw_completion(
                    ErrorKind::RangeError,
                    ErrorType::TemporalInvalidCalendarFieldName,
                    &[&"monthCode"],
                );
            }

            // ii. Set monthCode to CreateMonthCode(! ParseMonthCode(monthCode).[[MonthNumber]], false).
            if Utf16View::of_string(&month_code).ends_with_code_unit(u16::from(b'L')) {
                month_code = Utf16View::of_string(&month_code)
                    .trim(&[u16::from(b'L')], TrimMode::Right)
                    .to_utf16_string();
            }
        }
    }

    // 7. Let referenceYear be the ISO reference year for monthCode and day as described above. If calendar is "chinese"
    //    or "dangi", the reference years in Table 6 are to be used.
    // 8. Return the latest possible ISO Date Record isoDate such that isoDate.[[Year]] = referenceYear and
    //    NonISOCalendarISOToDate(calendar, isoDate) returns a Calendar Date Record whose [[MonthCode]] and [[Day]]
    //    field values respectively equal monthCode and day.
    let unicode_calendar_name = calendar_for_unicode(calendar);
    let month_code_view = Utf16View::of_string(&month_code);
    let result = if is_chinese_or_dangi {
        let reference_year = chinese_or_dangi_reference_year(calendar, month_code_view, day)
            .expect("the month code was constrained to one with a reference year");
        unicode_calendar::iso_year_and_month_code_to_iso_date(
            &unicode_calendar_name,
            reference_year,
            month_code_view,
            day,
        )
    } else {
        (1900..=1972).rev().chain(1973..=2035).find_map(|iso_year| {
            unicode_calendar::iso_year_and_month_code_to_iso_date(
                &unicode_calendar_name,
                iso_year,
                month_code_view,
                day,
            )
        })
    };

    Ok(result.expect("every month code and day has an ISO reference date"))
}

// 12.3.24 CalendarMonthDayToISOReferenceDate ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmonthdaytoisoreferencedate
pub fn calendar_month_day_to_iso_reference_date(
    vm: &Vm,
    calendar: Utf16View<'_>,
    fields: &CalendarFields,
    overflow: Overflow,
) -> ThrowCompletionOr<ISODate> {
    // 1. If calendar is "iso8601", then
    if calendar == ISO8601_CALENDAR {
        // a. Assert: fields.[[Month]] and fields.[[Day]] are not UNSET.
        let month = fields.month.expect("the month is resolved");
        let day = fields.day.expect("the day is resolved");

        // b. Let referenceISOYear be 1972 (the first ISO 8601 leap year after the epoch).
        const REFERENCE_ISO_YEAR: i32 = 1972;

        // c. If fields.[[Year]] is UNSET, let year be referenceISOYear; else let year be fields.[[Year]].
        let year = fields.year.unwrap_or(REFERENCE_ISO_YEAR);

        // d. Let result be ? RegulateISODate(year, fields.[[Month]], fields.[[Day]], overflow).
        let result = regulate_iso_date(vm, f64::from(year), f64::from(month), f64::from(day), overflow)?;

        // e. Return CreateISODateRecord(referenceISOYear, result.[[Month]], result.[[Day]]).
        return Ok(create_iso_date_record(
            f64::from(REFERENCE_ISO_YEAR),
            f64::from(result.month),
            f64::from(result.day),
        ));
    }

    // 2. Return ? NonISOMonthDayToISOReferenceDate(calendar, fields, overflow).
    non_iso_month_day_to_iso_reference_date(vm, calendar, fields, overflow)
}

// 12.3.25 NonISOCalendarISOToDate ( calendar, isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-nonisocalendarisotodate
pub fn non_iso_calendar_iso_to_date(calendar: Utf16View<'_>, iso_date: ISODate) -> CalendarDate {
    let mut result = unicode_calendar::iso_date_to_calendar_date(&calendar_for_unicode(calendar), iso_date);

    for row in &CALENDAR_ERA_DATA {
        if calendar != row.calendar {
            continue;
        }

        let era_year = match row.kind {
            EraKind::Epoch => result.year,
            EraKind::Negative => 1 - result.year,
            EraKind::Offset => result.year - row.offset.expect("an offset era has an offset") + 1,
        };

        if row.minimum_era_year.is_some_and(|minimum| era_year < minimum) {
            continue;
        }
        if row.maximum_era_year.is_some_and(|maximum| era_year > maximum) {
            continue;
        }
        if row
            .iso_era_start
            .is_some_and(|start| compare_iso_date(iso_date, start) < 0)
        {
            continue;
        }

        result.era = Some(Utf16String::from_utf8(row.era));
        result.era_year = Some(era_year);
        break;
    }

    result
}

// 12.3.26 CalendarISOToDate ( calendar, isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-calendarisotodate
pub fn calendar_iso_to_date(calendar: Utf16View<'_>, iso_date: ISODate) -> CalendarDate {
    // 1. If calendar is "iso8601", then
    if calendar == ISO8601_CALENDAR {
        // a. If MathematicalInLeapYear(EpochTimeForYear(isoDate.[[Year]])) = 1, let inLeapYear be true; else let inLeapYear be false.
        let in_leap_year = mathematical_in_leap_year(epoch_time_for_year(f64::from(iso_date.year))) == 1;

        // b. Return Calendar Date Record { [[Era]]: undefined, [[EraYear]]: undefined, [[Year]]: isoDate.[[Year]],
        //    [[Month]]: isoDate.[[Month]], [[MonthCode]]: CreateMonthCode(isoDate.[[Month]], false), [[Day]]: isoDate.[[Day]],
        //    [[DayOfWeek]]: ISODayOfWeek(isoDate), [[DayOfYear]]: ISODayOfYear(isoDate), [[WeekOfYear]]: ISOWeekOfYear(isoDate),
        //    [[DaysInWeek]]: 7, [[DaysInMonth]]: ISODaysInMonth(isoDate.[[Year]], isoDate.[[Month]]),
        //    [[DaysInYear]]: MathematicalDaysInYear(isoDate.[[Year]]), [[MonthsInYear]]: 12, [[InLeapYear]]: inLeapYear }.
        return CalendarDate {
            era: None,
            era_year: None,
            year: iso_date.year,
            month: iso_date.month,
            month_code: unicode_calendar::create_month_code(iso_date.month, false),
            day: iso_date.day,
            day_of_week: iso_day_of_week(iso_date),
            day_of_year: iso_day_of_year(iso_date),
            week_of_year: iso_week_of_year(iso_date),
            days_in_week: 7,
            days_in_month: iso_days_in_month(f64::from(iso_date.year), f64::from(iso_date.month)),
            days_in_year: mathematical_days_in_year(iso_date.year),
            months_in_year: 12,
            in_leap_year,
        };
    }

    // 2. Return NonISOCalendarISOToDate(calendar, isoDate).
    non_iso_calendar_iso_to_date(calendar, iso_date)
}

// 12.3.27 CalendarExtraFields ( calendar, fields ), https://tc39.es/proposal-temporal/#sec-temporal-calendarextrafields
// 4.1.22 CalendarExtraFields ( calendar, fields ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-calendarextrafields
pub fn calendar_extra_fields(calendar: Utf16View<'_>, fields: &[CalendarField]) -> Vec<CalendarField> {
    // 1. If fields contains an element equal to YEAR and CalendarSupportsEra(calendar) is true, then
    if fields.contains(&CalendarField::Year) && calendar_supports_era(calendar) {
        // a. Return « ERA, ERA-YEAR ».
        return vec![CalendarField::Era, CalendarField::EraYear];
    }

    // 2. Return an empty List.
    Vec::new()
}

// 12.3.28 NonISOFieldKeysToIgnore ( calendar, keys ), https://tc39.es/proposal-temporal/#sec-temporal-nonisofieldkeystoignore
// 4.1.23 NonISOFieldKeysToIgnore ( calendar, keys ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-nonisofieldkeystoignore
pub fn non_iso_field_keys_to_ignore(calendar: Utf16View<'_>, keys: &[CalendarField]) -> Vec<CalendarField> {
    // 1. Let ignoredKeys be a copy of keys.
    let mut ignored_keys = keys.to_vec();

    // 2. For each element key of keys, do
    for &key in keys {
        // a. If key is MONTH, append MONTH-CODE to ignoredKeys.
        if key == CalendarField::Month {
            ignored_keys.push(CalendarField::MonthCode);
        }

        // b. If key is MONTH-CODE, append month.
        if key == CalendarField::MonthCode {
            ignored_keys.push(CalendarField::Month);
        }

        // c. If key is one of ERA, ERA-YEAR, or YEAR and CalendarSupportsEra(calendar) is true, then
        if matches!(key, CalendarField::Era | CalendarField::EraYear | CalendarField::Year)
            && calendar_supports_era(calendar)
        {
            // i. Append ERA, ERA-YEAR, and YEAR to ignoredKeys.
            ignored_keys.push(CalendarField::Era);
            ignored_keys.push(CalendarField::EraYear);
            ignored_keys.push(CalendarField::Year);
        }

        // d. If key is one of DAY, MONTH, or MONTH-CODE and CalendarHasMidYearEras(calendar) is true, then
        if matches!(
            key,
            CalendarField::Day | CalendarField::Month | CalendarField::MonthCode
        ) && calendar_has_mid_year_eras(calendar)
        {
            // i. Append ERA and ERA-YEAR to ignoredKeys.
            ignored_keys.push(CalendarField::Era);
            ignored_keys.push(CalendarField::EraYear);
        }
    }

    // 3. NOTE: While ignoredKeys can have duplicate elements, this is not intended to be meaningful. This specification
    //    only checks whether particular keys are or are not members of the list.

    // 4. Return ignoredKeys.
    ignored_keys
}

// 12.3.29 CalendarFieldKeysToIgnore ( calendar, keys ), https://tc39.es/proposal-temporal/#sec-temporal-calendarfieldkeystoignore
pub fn calendar_field_keys_to_ignore(calendar: Utf16View<'_>, keys: &[CalendarField]) -> Vec<CalendarField> {
    // 1. If calendar is "iso8601", then
    if calendar == ISO8601_CALENDAR {
        // a. Let ignoredKeys be a new empty List.
        let mut ignored_keys = Vec::new();

        // b. For each element key of keys, do
        for &key in keys {
            // i. Append key to ignoredKeys.
            ignored_keys.push(key);

            // ii. If key is MONTH, append MONTH-CODE to ignoredKeys.
            if key == CalendarField::Month {
                ignored_keys.push(CalendarField::MonthCode);
            }
            // iii. Else if key is MONTH-CODE, append MONTH to ignoredKeys.
            else if key == CalendarField::MonthCode {
                ignored_keys.push(CalendarField::Month);
            }
        }

        // c. NOTE: While ignoredKeys can have duplicate elements, this is not intended to be meaningful. This specification
        //    only checks whether particular keys are or are not members of the list.

        // d. Return ignoredKeys.
        return ignored_keys;
    }

    // 2. Return NonISOFieldKeysToIgnore(calendar, keys).
    non_iso_field_keys_to_ignore(calendar, keys)
}

// 12.3.30 NonISOResolveFields ( calendar, fields, type ), https://tc39.es/proposal-temporal/#sec-temporal-nonisoresolvefields
// 4.1.24 NonISOResolveFields ( calendar, fields, type ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-nonisoresolvefields
pub fn non_iso_resolve_fields(
    vm: &Vm,
    calendar: Utf16View<'_>,
    fields: &mut CalendarFields,
    date_type: DateType,
) -> ThrowCompletionOr<()> {
    // 1. Let needsYear be false.
    // 2. If type is DATE or type is YEAR-MONTH, set needsYear to true.
    // 3. If fields.[[MonthCode]] is UNSET, set needsYear to true.
    // 4. If fields.[[Month]] is not UNSET, set needsYear to true.
    let needs_year = date_type == DateType::Date
        || date_type == DateType::YearMonth
        || fields.month_code.is_none()
        || fields.month.is_some();

    // 5. Let needsOrdinalMonth be false.
    // 6. If fields.[[Year]] is not UNSET, set needsOrdinalMonth to true.
    // 7. If fields.[[EraYear]] is not UNSET, set needsOrdinalMonth to true.
    let needs_ordinal_month = fields.year.is_some() || fields.era_year.is_some();

    // 8. Let needsDay be false.
    // 9. If type is DATE or type is MONTH-DAY, set needsDay to true.
    let needs_day = date_type == DateType::Date || date_type == DateType::MonthDay;

    // 10. If needsYear is true, then
    if needs_year {
        // a. If fields.[[Year]] is UNSET, then
        if fields.year.is_none() {
            // i. If CalendarSupportsEra(calendar) is false, throw a TypeError exception.
            if !calendar_supports_era(calendar) {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::MissingRequiredProperty, &[&"year"]);
            }

            // ii. If fields.[[Era]] is UNSET or fields.[[EraYear]] is UNSET, throw a TypeError exception.
            if fields.era.is_none() || fields.era_year.is_none() {
                return vm.throw_completion(ErrorKind::TypeError, ErrorType::MissingRequiredProperty, &[&"era"]);
            }
        }
    }

    // 11. If CalendarSupportsEra(calendar) is true, then
    if calendar_supports_era(calendar) {
        // a. If fields.[[Era]] is not UNSET and fields.[[EraYear]] is UNSET, throw a TypeError exception.
        if fields.era.is_some() && fields.era_year.is_none() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::MissingRequiredProperty, &[&"eraYear"]);
        }

        // b. If fields.[[EraYear]] is not UNSET and fields.[[Era]] is UNSET, throw a TypeError exception.
        if fields.era_year.is_some() && fields.era.is_none() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::MissingRequiredProperty, &[&"era"]);
        }
    }

    // 12. If needsDay is true and fields.[[Day]] is UNSET, throw a TypeError exception.
    if needs_day && fields.day.is_none() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::MissingRequiredProperty, &[&"day"]);
    }

    // 13. If fields.[[Month]] is UNSET and fields.[[MonthCode]] is UNSET, throw a TypeError exception.
    if fields.month.is_none() && fields.month_code.is_none() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::MissingRequiredProperty, &[&"month"]);
    }

    // 14. If CalendarSupportsEra(calendar) is true and fields.[[EraYear]] is not UNSET, then
    if calendar_supports_era(calendar)
        && let Some(era_year) = fields.era_year
    {
        let era = fields.era.clone().expect("an era year comes with an era");

        // a. Let canonicalEra be CanonicalizeEraInCalendar(calendar, fields.[[Era]]).
        let canonical_era = canonicalize_era_in_calendar(calendar, Utf16View::of_string(&era));

        // b. If canonicalEra is undefined, throw a RangeError exception.
        if canonical_era.is_none() {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::TemporalInvalidCalendarFieldName,
                &[&"era"],
            );
        }

        // c. Let arithmeticYear be CalendarDateArithmeticYearForEraYear(calendar, canonicalEra, fields.[[EraYear]]).
        let arithmetic_year =
            calendar_date_arithmetic_year_for_era_year(calendar, Utf16View::of_string(&era), era_year);

        // d. If fields.[[Year]] is not UNSET, and fields.[[Year]] ≠ arithmeticYear, throw a RangeError exception.
        if fields.year.is_some_and(|year| year != arithmetic_year) {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::TemporalInvalidCalendarFieldName,
                &[&"year"],
            );
        }

        // e. Set fields.[[Year]] to arithmeticYear.
        fields.year = Some(arithmetic_year);
    }

    // 15. Set fields.[[Era]] to UNSET.
    fields.era = None;

    // 16. Set fields.[[EraYear]] to UNSET.
    fields.era_year = None;

    // 17. NOTE: fields.[[Era]] and fields.[[EraYear]] are erased in order to allow a lenient interpretation of
    //     out-of-bounds values, which is particularly useful for consistent interpretation of dates in calendars with
    //     regnal eras.

    // 18. If fields.[[MonthCode]] is not UNSET, then
    if let Some(month_code) = fields.month_code.clone() {
        // a. If IsValidMonthCodeForCalendar(calendar, fields.[[MonthCode]]) is false, throw a RangeError exception.
        if !is_valid_month_code_for_calendar(calendar, Utf16View::of_string(&month_code)) {
            return vm.throw_completion(
                ErrorKind::RangeError,
                ErrorType::TemporalInvalidCalendarFieldName,
                &[&"monthCode"],
            );
        }

        // b. If fields.[[Year]] is not UNSET, then
        if let Some(year) = fields.year {
            // i. If YearContainsMonthCode(calendar, fields.[[Year]], fields.[[MonthCode]]) is true, let constrainedMonthCode be fields.[[MonthCode]];
            //    else let constrainedMonthCode be ! ConstrainMonthCode(calendar, fields.[[Year]], fields.[[MonthCode]], CONSTRAIN).
            let constrained_month_code = if year_contains_month_code(calendar, year, Utf16View::of_string(&month_code))
            {
                month_code
            } else {
                constrain_month_code(vm, calendar, year, &month_code, Overflow::Constrain).must()
            };

            // ii. Let month be MonthCodeToOrdinal(calendar, fields.[[Year]], constrainedMonthCode).
            let month = month_code_to_ordinal(calendar, year, Utf16View::of_string(&constrained_month_code));

            // iii. If fields.[[Month]] is not UNSET and fields.[[Month]] ≠ month, throw a RangeError exception.
            if fields
                .month
                .is_some_and(|fields_month| fields_month != u32::from(month))
            {
                return vm.throw_completion(
                    ErrorKind::RangeError,
                    ErrorType::TemporalInvalidCalendarFieldName,
                    &[&"month"],
                );
            }

            // iv. Set fields.[[Month]] to month.
            fields.month = Some(u32::from(month));

            // v. NOTE: fields.[[MonthCode]] is intentionally not overwritten with constrainedMonthCode. Pending the
            //    "overflow" parameter in CalendarDateToISO or CalendarMonthDayToISOReferenceDate, a month code not
            //    occurring in fields.[[Year]] may cause that operation to throw. However, if fields.[[Month]] is
            //    present, it must agree with the constrained month code.
        }
    }

    // 19. Assert: fields.[[Era]] and fields.[[EraYear]] are UNSET.
    assert!(fields.era.is_none());
    assert!(fields.era_year.is_none());

    // 20. Assert: If needsYear is true, fields.[[Year]] is not UNSET.
    if needs_year {
        assert!(fields.year.is_some());
    }

    // 21. Assert: If needsOrdinalMonth is true, fields.[[Month]] is not UNSET.
    if needs_ordinal_month {
        assert!(fields.month.is_some());
    }

    // 22. Assert: If needsDay is true, fields.[[Day]] is not UNSET.
    if needs_day {
        assert!(fields.day.is_some());
    }

    // 23. Return unused.
    Ok(())
}

// 12.3.31 CalendarResolveFields ( calendar, fields, type ), https://tc39.es/proposal-temporal/#sec-temporal-calendarresolvefields
pub fn calendar_resolve_fields(
    vm: &Vm,
    calendar: Utf16View<'_>,
    fields: &mut CalendarFields,
    date_type: DateType,
) -> ThrowCompletionOr<()> {
    // 1. If calendar is "iso8601", then
    if calendar == ISO8601_CALENDAR {
        // a. Let needsYear be false.
        // b. If type is either DATE or YEAR-MONTH, set needsYear to true.
        let needs_year = date_type == DateType::Date || date_type == DateType::YearMonth;

        // c. Let needsDay be false.
        // d. If type is either DATE or MONTH-DAY, set needsDay to true.
        let needs_day = date_type == DateType::Date || date_type == DateType::MonthDay;

        // e. If needsYear is true and fields.[[Year]] is UNSET, throw a TypeError exception.
        if needs_year && fields.year.is_none() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::MissingRequiredProperty, &[&"year"]);
        }

        // f. If needsDay is true and fields.[[Day]] is UNSET, throw a TypeError exception.
        if needs_day && fields.day.is_none() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::MissingRequiredProperty, &[&"day"]);
        }

        // g. If fields.[[Month]] is UNSET and fields.[[MonthCode]] is UNSET, throw a TypeError exception.
        if fields.month.is_none() && fields.month_code.is_none() {
            return vm.throw_completion(ErrorKind::TypeError, ErrorType::MissingRequiredProperty, &[&"month"]);
        }

        // h. If fields.[[MonthCode]] is not UNSET, then
        if let Some(month_code) = &fields.month_code {
            // i. Let parsedMonthCode be ! ParseMonthCode(fields.[[MonthCode]]).
            let parsed_month_code =
                unicode_calendar::parse_month_code(Utf16View::of_string(month_code)).expect("the month code is valid");

            // ii. If parsedMonthCode.[[IsLeapMonth]] is true, throw a RangeError exception.
            if parsed_month_code.is_leap_month {
                return vm.throw_completion(
                    ErrorKind::RangeError,
                    ErrorType::TemporalInvalidCalendarFieldName,
                    &[&"monthCode"],
                );
            }

            // iii. Let month be parsedMonthCode.[[MonthNumber]].
            let month = parsed_month_code.month_number;

            // iv. If month > 12, throw a RangeError exception.
            if month > 12 {
                return vm.throw_completion(
                    ErrorKind::RangeError,
                    ErrorType::TemporalInvalidCalendarFieldName,
                    &[&"monthCode"],
                );
            }

            // v. If fields.[[Month]] is not UNSET and fields.[[Month]] ≠ month, throw a RangeError exception.
            if fields
                .month
                .is_some_and(|fields_month| fields_month != u32::from(month))
            {
                return vm.throw_completion(
                    ErrorKind::RangeError,
                    ErrorType::TemporalInvalidCalendarFieldName,
                    &[&"month"],
                );
            }

            // vi. Set fields.[[Month]] to month.
            fields.month = Some(u32::from(month));
        }
    }
    // 2. Else,
    else {
        // a. Perform ? NonISOResolveFields(calendar, fields, type).
        return non_iso_resolve_fields(vm, calendar, fields, date_type);
    }

    // 3. Return UNUSED.
    Ok(())
}

// 4.1.1 CalendarSupportsEra ( calendar ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-calendarsupportsera
pub fn calendar_supports_era(calendar: Utf16View<'_>) -> bool {
    // 1. If calendar is listed in the "Calendar" column of Table 2, return true.
    // 2. If calendar is listed in the "Calendar Type" column of Table 1, return false.
    // 3. Return an implementation-defined value.
    CALENDAR_ERA_DATA.iter().any(|row| calendar == row.calendar)
}

// 4.1.2 CanonicalizeEraInCalendar ( calendar, era ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-canonicalizeeraincalendar
pub fn canonicalize_era_in_calendar(calendar: Utf16View<'_>, era: Utf16View<'_>) -> Option<&'static str> {
    // 1. For each row of Table 2, except the header row, do
    for row in &CALENDAR_ERA_DATA {
        // a. Let cal be the Calendar value of the current row.
        // b. If cal is equal to calendar, then
        if calendar == row.calendar {
            // i. Let canonicalName be the Era value of the current row.
            let canonical_name = row.era;

            // ii. If canonicalName is equal to era, return canonicalName.
            if era == canonical_name {
                return Some(canonical_name);
            }

            // iii. Let aliases be a List whose elements are the strings given in the "Aliases" column of the row.
            // iv. If aliases contains era, return canonicalName.
            // NB: The C++ compares the era with an empty alias too, so an empty era matches eras without an alias.
            if era == row.alias.unwrap_or("") {
                return Some(canonical_name);
            }
        }
    }

    // 2. If calendar is listed in the "Calendar Type" column of Table 1, return undefined.
    // 3. Return an implementation-defined value.
    None
}

// 4.1.3 CalendarHasMidYearEras ( calendar ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-calendarhasmidyeareras
pub fn calendar_has_mid_year_eras(calendar: Utf16View<'_>) -> bool {
    // 1. If calendar is "japanese", return true.
    // 2. If calendar is listed in the "Calendar Type" column of Table 1, return false.
    // 3. Return an implementation-defined value.
    calendar == "japanese"
}

// 4.1.4 IsValidMonthCodeForCalendar ( calendar, monthCode ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-isvalidmonthcodeforcalendar
pub fn is_valid_month_code_for_calendar(calendar: Utf16View<'_>, month_code: Utf16View<'_>) -> bool {
    // 1. Let commonMonthCodes be « "M01", "M02", "M03", "M04", "M05", "M06", "M07", "M08", "M09", "M10", "M11", "M12" ».
    // 2. If commonMonthCodes contains monthCode, return true.
    const COMMON_MONTH_CODES: [&str; 12] = [
        "M01", "M02", "M03", "M04", "M05", "M06", "M07", "M08", "M09", "M10", "M11", "M12",
    ];
    if COMMON_MONTH_CODES.iter().any(|common| month_code == *common) {
        return true;
    }

    // 3. If calendar is listed in the "Calendar" column of Table 3, then
    if let Some(row) = additional_month_codes_of(calendar) {
        // a. Let r be the row in Table 3 with a value in the Calendar column matching calendar.
        // b. Let specialMonthCodes be a List whose elements are the strings given in the "Additional Month Codes" column of r.
        // c. If specialMonthCodes contains monthCode, return true.
        // d. Return false.
        return row
            .additional_month_codes
            .iter()
            .any(|additional_month_code| month_code == *additional_month_code);
    }

    // 4. If calendar is listed in the "Calendar Type" column of Table 1, return false.
    // 5. Return an implementation-defined value.
    false
}

// 4.1.5 YearContainsMonthCode ( calendar, arithmeticYear, monthCode ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-yearcontainsmonthcode
pub fn year_contains_month_code(calendar: Utf16View<'_>, arithmetic_year: i32, month_code: Utf16View<'_>) -> bool {
    // 1. Assert: IsValidMonthCodeForCalendar(calendar, monthCode) is true.
    assert!(is_valid_month_code_for_calendar(calendar, month_code));

    // 2. If ! ParseMonthCode(monthCode).[[IsLeap]] is false, return true.
    if !unicode_calendar::parse_month_code(month_code)
        .expect("the month code is valid")
        .is_leap_month
    {
        return true;
    }

    // 3. Return whether the leap month indicated by monthCode exists in the year arithmeticYear in calendar, using
    //    calendar-dependent behaviour.
    unicode_calendar::calendar_year_contains_month_code(&calendar_for_unicode(calendar), arithmetic_year, month_code)
}

// 4.1.6 ConstrainMonthCode ( calendar, arithmeticYear, monthCode, overflow ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-constrainmonthcode
pub fn constrain_month_code(
    vm: &Vm,
    calendar: Utf16View<'_>,
    arithmetic_year: i32,
    month_code: &Utf16String,
    overflow: Overflow,
) -> ThrowCompletionOr<Utf16String> {
    let month_code_view = Utf16View::of_string(month_code);

    // 1. Assert: IsValidMonthCodeForCalendar(calendar, monthCode) is true.
    assert!(is_valid_month_code_for_calendar(calendar, month_code_view));

    // 2. If YearContainsMonthCode(calendar, arithmeticYear, monthCode) is true, return monthCode.
    if year_contains_month_code(calendar, arithmetic_year, month_code_view) {
        return Ok(month_code.clone());
    }

    // 3. If overflow is REJECT, throw a RangeError exception.
    if overflow == Overflow::Reject {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidMonthCode, &[]);
    }

    // 4. Assert: calendar is listed in the "Calendar" column of Table 3.
    // 5. Let r be the row in Table 3 with a value in the Calendar column matching calendar.
    let row = additional_month_codes_of(calendar).expect("the calendar has additional month codes");

    // 6. Let shiftType be the value given in the "Leap to Common Month Transformation" column of r.
    // 7. If shiftType is SKIP-BACKWARD, then
    if row.leap_to_common_month_transformation == Some(Leap::SkipBackward) {
        // a. Return CreateMonthCode(! ParseMonthCode(monthCode).[[MonthNumber]], false).
        return Ok(month_code_view
            .trim(&[u16::from(b'L')], TrimMode::Right)
            .to_utf16_string());
    }

    // 8. Else,
    // a. Assert: monthCode is "M05L".
    assert!(month_code_view == "M05L");

    // b. Return "M06".
    Ok(Utf16String::from_utf8("M06"))
}

// 4.1.7 MonthCodeToOrdinal ( calendar, arithmeticYear, monthCode ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-monthcodetoordinal
pub fn month_code_to_ordinal(calendar: Utf16View<'_>, arithmetic_year: i32, month_code: Utf16View<'_>) -> u8 {
    // 1. Assert: YearContainsMonthCode(calendar, arithmeticYear, monthCode) is true.
    assert!(year_contains_month_code(calendar, arithmetic_year, month_code));

    // 2. Let monthsBefore be 0.
    let mut months_before = 0;

    // 3. Let number be 1.
    let mut number = 1;

    // 4. Let isLeap be false.
    let mut is_leap = false;

    // 5. Let r be the row in Table 3 which the calendar is in the Calendar column.
    let row = additional_month_codes_of(calendar);

    // 6. If the "Leap to Common Month Transformation" column of r is empty, then
    let Some(row) = row.filter(|row| row.leap_to_common_month_transformation.is_some()) else {
        // a. Return ! ParseMonthCode(monthCode).[[MonthNumber]].
        return unicode_calendar::parse_month_code(month_code)
            .expect("the month code is valid")
            .month_number;
    };

    // 7. Assert: The "Additional Month Codes" column of r does not contain "M00L" or "M13".
    assert!(!row.additional_month_codes.contains(&"M00L"));
    assert!(!row.additional_month_codes.contains(&"M13"));

    // 8. Assert: This algorithm will return before the following loop terminates by failing its condition.

    // 9. Repeat, while number ≤ 12,
    while number <= 12 {
        // a. Let currentMonthCode be CreateMonthCode(number, isLeap).
        let current_month_code = unicode_calendar::create_month_code(number, is_leap);
        let current_month_code = Utf16View::of_string(&current_month_code);

        // b. If IsValidMonthCodeForCalendar(calendar, currentMonthCode) is true and YearContainsMonthCode(calendar, arithmeticYear, currentMonthCode) is true, then
        if is_valid_month_code_for_calendar(calendar, current_month_code)
            && year_contains_month_code(calendar, arithmetic_year, current_month_code)
        {
            // i. Set monthsBefore to monthsBefore + 1.
            months_before += 1;
        }

        // c. If currentMonthCode is monthCode, then
        if current_month_code == month_code {
            // i. Return monthsBefore.
            return months_before;
        }

        // d. If isLeap is false, then
        //     i. Set isLeap to true.
        // e. Else,
        //     i. Set isLeap to false.
        //     ii. Set number to number + 1.
        let was_leap = is_leap;
        is_leap = !is_leap;
        if was_leap {
            number += 1;
        }
    }

    unreachable!()
}

// 4.1.8 CalendarDaysInMonth ( calendar, arithmeticYear, ordinalMonth ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-calendardaysinmonth
pub fn calendar_days_in_month(calendar: Utf16View<'_>, arithmetic_year: i32, ordinal_month: u8) -> u8 {
    // 1. Let isoDate be ! CalendarIntegersToISO(calendar, arithmeticYear, ordinalMonth, 1).
    // 2. Return CalendarISOToDate(calendar, isoDate).[[DaysInMonth]].
    unicode_calendar::calendar_days_in_month(&calendar_for_unicode(calendar), arithmetic_year, ordinal_month)
}

// 4.1.12 CalendarDateArithmeticYearForEraYear ( calendar, era, eraYear ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-calendardatearithmeticyearforerayear
pub fn calendar_date_arithmetic_year_for_era_year(calendar: Utf16View<'_>, era: Utf16View<'_>, era_year: i32) -> i32 {
    // 1. Let era be CanonicalizeEraInCalendar(calendar, era).
    // 2. Assert: era is not undefined.
    let canonical_era = canonicalize_era_in_calendar(calendar, era).expect("the era is valid in the calendar");

    // 3. If calendar is not listed in the "Calendar Type" column of Table 1, return an implementation-defined value.
    // 4. Let r be the row in Table 2 with a value in the Calendar column matching calendar and a value in the Era
    //    column matching era.
    let Some(row) = CALENDAR_ERA_DATA
        .iter()
        .find(|row| calendar == row.calendar && row.era == canonical_era)
    else {
        return era_year;
    };

    // 5. Let eraKind be the value given in the "Era Kind" column of r.
    // 6. Let offset be the value given in the "Offset" column of r.
    match row.kind {
        // 7. If eraKind is EPOCH, return eraYear.
        EraKind::Epoch => era_year,

        // 8. If eraKind is NEGATIVE, return 1 - eraYear.
        EraKind::Negative => 1i32.wrapping_sub(era_year),

        // 9. Assert: eraKind is OFFSET.
        // 10. Assert: offset is not undefined.
        // 11. Return offset + eraYear - 1.
        EraKind::Offset => row
            .offset
            .expect("an offset era has an offset")
            .wrapping_add(era_year)
            .wrapping_sub(1),
    }
}

// 4.1.13 CalendarIntegersToISO ( calendar, arithmeticYear, ordinalMonth, day ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-calendarintegerstoiso
pub fn calendar_integers_to_iso(
    vm: &Vm,
    calendar: Utf16View<'_>,
    arithmetic_year: i32,
    ordinal_month: u8,
    day: u8,
) -> ThrowCompletionOr<ISODate> {
    // 1. If arithmeticYear, ordinalMonth, and day do not form a valid date in calendar, throw a RangeError exception.
    // 2. Let isoDate be an ISO Date Record such that CalendarISOToDate(calendar, isoDate) returns a Calendar Date Record
    //    whose [[Year]], [[Month]], and [[Day]] field values respectively equal arithmeticYear, ordinalMonth, and day.
    // 3. NOTE: No known calendars have repeated dates that would cause isoDate to be ambiguous between two ISO Date Records.
    // 4. Return isoDate.
    if let Some(iso_date) = unicode_calendar::calendar_date_to_iso_date(
        &calendar_for_unicode(calendar),
        arithmetic_year,
        ordinal_month,
        day,
    ) {
        return Ok(create_iso_date_record(
            f64::from(iso_date.year),
            f64::from(iso_date.month),
            f64::from(iso_date.day),
        ));
    }
    vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidISODate, &[])
}

// 4.1.15 CalendarMonthsInYear ( calendar, arithmeticYear ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-calendarmonthsinyear
pub fn calendar_months_in_year(calendar: Utf16View<'_>, arithmetic_year: i32) -> u8 {
    // 1. Let isoDate be ! CalendarIntegersToISO(calendar, arithmeticYear, 1, 1).
    // 2. Return CalendarISOToDate(calendar, isoDate).[[MonthsInYear]].
    unicode_calendar::calendar_months_in_year(&calendar_for_unicode(calendar), arithmetic_year)
}

// 4.1.16 BalanceNonISODate ( calendar, arithmeticYear, ordinalMonth, day ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-balancenonisodate
pub fn balance_non_iso_date(
    calendar: Utf16View<'_>,
    arithmetic_year: i32,
    ordinal_month: i32,
    day: i32,
) -> BalancedDate {
    let days_in_month_of = |year: i32, month: i32| i32::from(calendar_days_in_month(calendar, year, month as u8));
    let months_in_year_of = |year: i32| i32::from(calendar_months_in_year(calendar, year));

    // 1. Let resolvedYear be arithmeticYear.
    let mut resolved_year = arithmetic_year;

    // 2. Let resolvedMonth be ordinalMonth.
    let mut resolved_month = ordinal_month;

    // 3. Let monthsInYear be CalendarMonthsInYear(calendar, resolvedYear).
    let mut months_in_year = months_in_year_of(resolved_year);

    // 4. Repeat, while resolvedMonth ≤ 0,
    while resolved_month <= 0 {
        // a. Set resolvedYear to resolvedYear - 1.
        resolved_year -= 1;

        // b. Set monthsInYear to CalendarMonthsInYear(calendar, resolvedYear).
        months_in_year = months_in_year_of(resolved_year);

        // c. Set resolvedMonth to resolvedMonth + monthsInYear.
        resolved_month += months_in_year;
    }

    // 5. Repeat, while resolvedMonth > monthsInYear,
    while resolved_month > months_in_year {
        // a. Set resolvedMonth to resolvedMonth - monthsInYear.
        resolved_month -= months_in_year;

        // b. Set resolvedYear to resolvedYear + 1.
        resolved_year += 1;

        // c. Set monthsInYear to CalendarMonthsInYear(calendar, resolvedYear).
        months_in_year = months_in_year_of(resolved_year);
    }

    // 6. Let resolvedDay be day.
    let mut resolved_day = day;

    // 7. Let daysInMonth be CalendarDaysInMonth(calendar, resolvedYear, resolvedMonth).
    let mut days_in_month = days_in_month_of(resolved_year, resolved_month);

    // 8. Repeat, while resolvedDay ≤ 0,
    while resolved_day <= 0 {
        // a. Set resolvedMonth to resolvedMonth - 1.
        resolved_month -= 1;

        // b. If resolvedMonth is 0, then
        if resolved_month == 0 {
            // i. Set resolvedYear to resolvedYear - 1.
            resolved_year -= 1;

            // ii. Set monthsInYear to CalendarMonthsInYear(calendar, resolvedYear).
            months_in_year = months_in_year_of(resolved_year);

            // iii. Set resolvedMonth to monthsInYear.
            resolved_month = months_in_year;
        }

        // c. Set daysInMonth to CalendarDaysInMonth(calendar, resolvedYear, resolvedMonth).
        days_in_month = days_in_month_of(resolved_year, resolved_month);

        // d. Set resolvedDay to resolvedDay + daysInMonth.
        resolved_day += days_in_month;
    }

    // 9. Repeat, while resolvedDay > daysInMonth,
    while resolved_day > days_in_month {
        // a. Set resolvedDay to resolvedDay - daysInMonth.
        resolved_day -= days_in_month;

        // b. Set resolvedMonth to resolvedMonth + 1.
        resolved_month += 1;

        // c. If resolvedMonth > monthsInYear, then
        if resolved_month > months_in_year {
            // i. Set resolvedYear to resolvedYear + 1.
            resolved_year += 1;

            // ii. Set monthsInYear to CalendarMonthsInYear(calendar, resolvedYear).
            months_in_year = months_in_year_of(resolved_year);

            // iii. Set resolvedMonth to 1.
            resolved_month = 1;
        }

        // d. Set daysInMonth to CalendarDaysInMonth(calendar, resolvedYear, resolvedMonth).
        days_in_month = days_in_month_of(resolved_year, resolved_month);
    }

    // 10. Return the Record { [[Year]]: resolvedYear, [[Month]]: resolvedMonth, [[Day]]: resolvedDay }.
    BalancedDate {
        year: resolved_year,
        month: resolved_month as u8,
        day: resolved_day as u8,
    }
}

// 4.1.17 NonISODateSurpasses ( calendar, sign, fromIsoDate, toIsoDate, years, months, weeks, days ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-nonisodatesurpasses
// NB: The only caller to this function is NonISODateUntil, which precomputes the calendar dates.
#[allow(clippy::too_many_arguments)]
pub fn non_iso_date_surpasses(
    vm: &Vm,
    calendar: Utf16View<'_>,
    sign: i8,
    from_calendar_date: &CalendarDate,
    to_calendar_date: &CalendarDate,
    years: f64,
    months: f64,
    weeks: f64,
    days: f64,
) -> bool {
    // 1. Let parts be CalendarISOToDate(calendar, fromIsoDate).
    let parts = from_calendar_date;

    // 2. Let calDate2 be CalendarISOToDate(calendar, toIsoDate).
    let calendar_date_2 = to_calendar_date;

    // 3. Let y0 be parts.[[Year]] + years.
    let y0 = parts.year.wrapping_add(years as i32);

    // 4. If CompareSurpasses(sign, y0, parts.[[MonthCode]], parts.[[Day]], calDate2) is true, return true.
    if compare_surpasses(
        sign,
        y0,
        MonthOrCode::Code(Utf16View::of_string(&parts.month_code)),
        parts.day,
        calendar_date_2,
    ) {
        return true;
    }

    // 5. Let m0 be MonthCodeToOrdinal(calendar, y0, ! ConstrainMonthCode(calendar, y0, parts.[[MonthCode]], CONSTRAIN)).
    let constrained_month_code = constrain_month_code(vm, calendar, y0, &parts.month_code, Overflow::Constrain).must();
    let m0 = month_code_to_ordinal(calendar, y0, Utf16View::of_string(&constrained_month_code));

    // 6. Let monthsAdded be BalanceNonISODate(calendar, y0, m0 + months, 1).
    let months_added = balance_non_iso_date(calendar, y0, i32::from(m0).wrapping_add(months as i32), 1);

    // 7. If CompareSurpasses(sign, monthsAdded.[[Year]], monthsAdded.[[Month]], parts.[[Day]], calDate2) is true, return true.
    if compare_surpasses(
        sign,
        months_added.year,
        MonthOrCode::Month(months_added.month),
        parts.day,
        calendar_date_2,
    ) {
        return true;
    }

    // 8. If weeks = 0 and days = 0, return false.
    if weeks == 0.0 && days == 0.0 {
        return false;
    }

    // 9. Let endOfMonth be BalanceNonISODate(calendar, monthsAdded.[[Year]], monthsAdded.[[Month]] + 1, 0).
    let end_of_month = balance_non_iso_date(calendar, months_added.year, i32::from(months_added.month) + 1, 0);

    // 10. Let baseDay be parts.[[Day]].
    let base_day = parts.day;

    // 11. If baseDay ≤ endOfMonth.[[Day]], then
    //     a. Let regulatedDay be baseDay.
    // 12. Else,
    //     a. Let regulatedDay be endOfMonth.[[Day]].
    let regulated_day = if base_day <= end_of_month.day {
        base_day
    } else {
        end_of_month.day
    };

    // 13. Let daysInWeek be 7 (the number of days in a week for all supported calendars).
    const DAYS_IN_WEEK: f64 = 7.0;

    // 14. Let balancedDate be BalanceNonISODate(calendar, endOfMonth.[[Year]], endOfMonth.[[Month]], regulatedDay + daysInWeek * weeks + days).
    let balanced_date = balance_non_iso_date(
        calendar,
        end_of_month.year,
        i32::from(end_of_month.month),
        (f64::from(regulated_day) + (DAYS_IN_WEEK * weeks) + days) as i32,
    );

    // 15. Return CompareSurpasses(sign, balancedDate.[[Year]], balancedDate.[[Month]], balancedDate.[[Day]], calDate2).
    compare_surpasses(
        sign,
        balanced_date.year,
        MonthOrCode::Month(balanced_date.month),
        balanced_date.day,
        calendar_date_2,
    )
}

/// Utf16View::trim("L"sv, TrimMode::Right) of a month code, for the month codes that end in L.
pub fn month_code_without_leap_marker(month_code: Utf16View<'_>) -> Utf16String {
    month_code.trim(&[u16::from(b'L')], TrimMode::Right).to_utf16_string()
}

/// ISO8601_CALENDAR as a view.
pub fn iso8601_calendar_view() -> Utf16View<'static> {
    ascii_view(ISO8601_CALENDAR)
}
