/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The date-time formatting of LibUnicode (Libraries/LibUnicode/DateTimeFormat.h), through the C exports of
//! Libraries/LibUnicode/DateTimeFormatExports.h.

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;

use super::intl::{UnicodeIntlText, UnicodeTextMappingOutput, text_output};
use crate::utf16::Utf16View;

/// UNICODE_CALENDAR_PATTERN_FIELD_ABSENT.
const FIELD_ABSENT: u8 = 0xff;

/// UnicodeCalendarPatternFields.
#[repr(C)]
#[derive(Clone, Copy)]
struct UnicodeCalendarPatternFields {
    hour_cycle: u8,
    hour12: u8,
    era: u8,
    year: u8,
    month: u8,
    weekday: u8,
    day: u8,
    day_period: u8,
    hour: u8,
    minute: u8,
    second: u8,
    fractional_second_digits: u8,
    time_zone_name: u8,
}

type UnicodeAppendDateTimeFormatPart = unsafe extern "C" fn(
    context: *mut c_void,
    type_: *const u16,
    type_length: usize,
    value: *const u16,
    value_length: usize,
    source: *const u16,
    source_length: usize,
);

unsafe extern "C" {
    fn unicode_default_hour_cycle(locale: UnicodeIntlText, hour_cycle: *mut u8) -> bool;

    fn unicode_date_time_format_create_for_date_and_time_style(
        locale: UnicodeIntlText,
        time_zone_identifier: UnicodeIntlText,
        hour_cycle: u8,
        hour12: u8,
        date_style: u8,
        time_style: u8,
    ) -> *mut c_void;
    fn unicode_date_time_format_create_for_pattern_options(
        locale: UnicodeIntlText,
        time_zone_identifier: UnicodeIntlText,
        options: UnicodeCalendarPatternFields,
        has_pattern: bool,
        pattern: UnicodeIntlText,
    ) -> *mut c_void;
    fn unicode_date_time_format_chosen_pattern(
        date_time_format: *const c_void,
        fields: *mut UnicodeCalendarPatternFields,
        pattern: UnicodeTextMappingOutput,
    );
    fn unicode_date_time_format_format(date_time_format: *const c_void, time: f64, output: UnicodeTextMappingOutput);
    fn unicode_date_time_format_format_to_parts(
        date_time_format: *const c_void,
        time: f64,
        context: *mut c_void,
        append_part: UnicodeAppendDateTimeFormatPart,
    );
    fn unicode_date_time_format_format_range(
        date_time_format: *const c_void,
        start: f64,
        end: f64,
        output: UnicodeTextMappingOutput,
    );
    fn unicode_date_time_format_format_range_to_parts(
        date_time_format: *const c_void,
        start: f64,
        end: f64,
        context: *mut c_void,
        append_part: UnicodeAppendDateTimeFormatPart,
    );
    fn unicode_date_time_format_destroy(date_time_format: *mut c_void);
}

/// Unicode::DateTimeStyle.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum DateTimeStyle {
    Full,
    Long,
    Medium,
    Short,
}

/// Unicode::date_time_style_from_string.
pub fn date_time_style_from_string(style: Utf16View<'_>) -> DateTimeStyle {
    if style == "full" {
        return DateTimeStyle::Full;
    }
    if style == "long" {
        return DateTimeStyle::Long;
    }
    if style == "medium" {
        return DateTimeStyle::Medium;
    }
    if style == "short" {
        return DateTimeStyle::Short;
    }
    unreachable!("the date-time style is one of the values GetOption allows")
}

/// Unicode::date_time_style_to_string.
pub fn date_time_style_to_string(style: DateTimeStyle) -> &'static str {
    match style {
        DateTimeStyle::Full => "full",
        DateTimeStyle::Long => "long",
        DateTimeStyle::Medium => "medium",
        DateTimeStyle::Short => "short",
    }
}

/// Unicode::HourCycle.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum HourCycle {
    H11,
    H12,
    H23,
    H24,
}

impl HourCycle {
    fn from_u8(hour_cycle: u8) -> Self {
        const HOUR_CYCLES: [HourCycle; 4] = [HourCycle::H11, HourCycle::H12, HourCycle::H23, HourCycle::H24];
        HOUR_CYCLES[usize::from(hour_cycle)]
    }
}

/// Unicode::hour_cycle_from_string.
pub fn hour_cycle_from_string(hour_cycle: Utf16View<'_>) -> HourCycle {
    if hour_cycle == "h11" {
        return HourCycle::H11;
    }
    if hour_cycle == "h12" {
        return HourCycle::H12;
    }
    if hour_cycle == "h23" {
        return HourCycle::H23;
    }
    if hour_cycle == "h24" {
        return HourCycle::H24;
    }
    unreachable!("the hour cycle is one of the values ResolveOptions allows")
}

/// Unicode::hour_cycle_to_string.
pub fn hour_cycle_to_string(hour_cycle: HourCycle) -> &'static str {
    match hour_cycle {
        HourCycle::H11 => "h11",
        HourCycle::H12 => "h12",
        HourCycle::H23 => "h23",
        HourCycle::H24 => "h24",
    }
}

/// Unicode::default_hour_cycle.
pub fn default_hour_cycle(locale: Utf16View<'_>) -> Option<HourCycle> {
    let mut hour_cycle = 0u8;
    // SAFETY: The locale is valid for its length, and the export writes one hour cycle.
    let found = unsafe { unicode_default_hour_cycle(UnicodeIntlText::of(locale), &raw mut hour_cycle) };
    found.then(|| HourCycle::from_u8(hour_cycle))
}

/// Unicode::CalendarPatternStyle.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum CalendarPatternStyle {
    Narrow,
    Short,
    Long,
    Numeric,
    TwoDigit,
    ShortOffset,
    LongOffset,
    ShortGeneric,
    LongGeneric,
}

impl CalendarPatternStyle {
    fn from_u8(style: u8) -> Self {
        const STYLES: [CalendarPatternStyle; 9] = [
            CalendarPatternStyle::Narrow,
            CalendarPatternStyle::Short,
            CalendarPatternStyle::Long,
            CalendarPatternStyle::Numeric,
            CalendarPatternStyle::TwoDigit,
            CalendarPatternStyle::ShortOffset,
            CalendarPatternStyle::LongOffset,
            CalendarPatternStyle::ShortGeneric,
            CalendarPatternStyle::LongGeneric,
        ];
        STYLES[usize::from(style)]
    }
}

/// Unicode::calendar_pattern_style_from_string.
pub fn calendar_pattern_style_from_string(style: Utf16View<'_>) -> CalendarPatternStyle {
    if style == "narrow" {
        return CalendarPatternStyle::Narrow;
    }
    if style == "short" {
        return CalendarPatternStyle::Short;
    }
    if style == "long" {
        return CalendarPatternStyle::Long;
    }
    if style == "numeric" {
        return CalendarPatternStyle::Numeric;
    }
    if style == "2-digit" {
        return CalendarPatternStyle::TwoDigit;
    }
    if style == "shortOffset" {
        return CalendarPatternStyle::ShortOffset;
    }
    if style == "longOffset" {
        return CalendarPatternStyle::LongOffset;
    }
    if style == "shortGeneric" {
        return CalendarPatternStyle::ShortGeneric;
    }
    if style == "longGeneric" {
        return CalendarPatternStyle::LongGeneric;
    }
    unreachable!("the calendar pattern style is one of the values GetOption allows")
}

/// Unicode::calendar_pattern_style_to_string.
pub fn calendar_pattern_style_to_string(style: CalendarPatternStyle) -> &'static str {
    match style {
        CalendarPatternStyle::Narrow => "narrow",
        CalendarPatternStyle::Short => "short",
        CalendarPatternStyle::Long => "long",
        CalendarPatternStyle::Numeric => "numeric",
        CalendarPatternStyle::TwoDigit => "2-digit",
        CalendarPatternStyle::ShortOffset => "shortOffset",
        CalendarPatternStyle::LongOffset => "longOffset",
        CalendarPatternStyle::ShortGeneric => "shortGeneric",
        CalendarPatternStyle::LongGeneric => "longGeneric",
    }
}

/// Unicode::CalendarPattern::Field.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CalendarPatternField {
    Era,
    Year,
    Month,
    Weekday,
    Day,
    DayPeriod,
    Hour,
    Minute,
    Second,
    FractionalSecondDigits,
    TimeZoneName,
}

/// The value of one field of a CalendarPattern: a style, or the number of fractional second digits.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CalendarPatternFieldValue {
    Style(CalendarPatternStyle),
    FractionalSecondDigits(u8),
}

/// Unicode::CalendarPattern.
#[derive(Clone, Default, PartialEq, Eq)]
pub struct CalendarPattern {
    pub hour_cycle: Option<HourCycle>,
    pub hour12: Option<bool>,

    // https://unicode.org/reports/tr35/tr35-dates.html#Calendar_Fields
    pub era: Option<CalendarPatternStyle>,
    pub year: Option<CalendarPatternStyle>,
    pub month: Option<CalendarPatternStyle>,
    pub weekday: Option<CalendarPatternStyle>,
    pub day: Option<CalendarPatternStyle>,
    pub day_period: Option<CalendarPatternStyle>,
    pub hour: Option<CalendarPatternStyle>,
    pub minute: Option<CalendarPatternStyle>,
    pub second: Option<CalendarPatternStyle>,
    pub fractional_second_digits: Option<u8>,
    pub time_zone_name: Option<CalendarPatternStyle>,

    pub pattern: Option<Utf16String>,
}

impl CalendarPattern {
    fn style_field_mut(&mut self, field: CalendarPatternField) -> Option<&mut Option<CalendarPatternStyle>> {
        Some(match field {
            CalendarPatternField::Era => &mut self.era,
            CalendarPatternField::Year => &mut self.year,
            CalendarPatternField::Month => &mut self.month,
            CalendarPatternField::Weekday => &mut self.weekday,
            CalendarPatternField::Day => &mut self.day,
            CalendarPatternField::DayPeriod => &mut self.day_period,
            CalendarPatternField::Hour => &mut self.hour,
            CalendarPatternField::Minute => &mut self.minute,
            CalendarPatternField::Second => &mut self.second,
            CalendarPatternField::TimeZoneName => &mut self.time_zone_name,
            CalendarPatternField::FractionalSecondDigits => return None,
        })
    }

    /// The value of `field`, if the pattern has it.
    pub fn field(&self, field: CalendarPatternField) -> Option<CalendarPatternFieldValue> {
        let style = match field {
            CalendarPatternField::Era => self.era,
            CalendarPatternField::Year => self.year,
            CalendarPatternField::Month => self.month,
            CalendarPatternField::Weekday => self.weekday,
            CalendarPatternField::Day => self.day,
            CalendarPatternField::DayPeriod => self.day_period,
            CalendarPatternField::Hour => self.hour,
            CalendarPatternField::Minute => self.minute,
            CalendarPatternField::Second => self.second,
            CalendarPatternField::TimeZoneName => self.time_zone_name,
            CalendarPatternField::FractionalSecondDigits => {
                return self
                    .fractional_second_digits
                    .map(CalendarPatternFieldValue::FractionalSecondDigits);
            }
        };
        style.map(CalendarPatternFieldValue::Style)
    }

    pub fn has_field(&self, field: CalendarPatternField) -> bool {
        self.field(field).is_some()
    }

    /// Sets `field` to `value`, or empties it when `value` is None.
    pub fn set_field(&mut self, field: CalendarPatternField, value: Option<CalendarPatternFieldValue>) {
        match value {
            None => {
                if let Some(style) = self.style_field_mut(field) {
                    *style = None;
                } else {
                    self.fractional_second_digits = None;
                }
            }
            Some(CalendarPatternFieldValue::Style(style)) => {
                *self
                    .style_field_mut(field)
                    .expect("only fractionalSecondDigits holds a number of digits") = Some(style);
            }
            Some(CalendarPatternFieldValue::FractionalSecondDigits(digits)) => {
                assert!(field == CalendarPatternField::FractionalSecondDigits);
                self.fractional_second_digits = Some(digits);
            }
        }
    }

    /// Sets `field` to `style` if it holds a style, as the C++ only assigns a CalendarPatternStyle to the fields whose
    /// type is one.
    pub fn set_style_field_if_style(&mut self, field: CalendarPatternField, style: CalendarPatternStyle) {
        if let Some(field) = self.style_field_mut(field) {
            *field = Some(style);
        }
    }

    fn to_fields(&self) -> UnicodeCalendarPatternFields {
        fn style(style: Option<CalendarPatternStyle>) -> u8 {
            style.map_or(FIELD_ABSENT, |style| style as u8)
        }

        UnicodeCalendarPatternFields {
            hour_cycle: self.hour_cycle.map_or(FIELD_ABSENT, |hour_cycle| hour_cycle as u8),
            hour12: self.hour12.map_or(FIELD_ABSENT, u8::from),
            era: style(self.era),
            year: style(self.year),
            month: style(self.month),
            weekday: style(self.weekday),
            day: style(self.day),
            day_period: style(self.day_period),
            hour: style(self.hour),
            minute: style(self.minute),
            second: style(self.second),
            fractional_second_digits: self.fractional_second_digits.unwrap_or(FIELD_ABSENT),
            time_zone_name: style(self.time_zone_name),
        }
    }

    fn from_fields(fields: &UnicodeCalendarPatternFields, pattern: Option<Utf16String>) -> Self {
        fn style(style: u8) -> Option<CalendarPatternStyle> {
            (style != FIELD_ABSENT).then(|| CalendarPatternStyle::from_u8(style))
        }

        Self {
            hour_cycle: (fields.hour_cycle != FIELD_ABSENT).then(|| HourCycle::from_u8(fields.hour_cycle)),
            hour12: (fields.hour12 != FIELD_ABSENT).then_some(fields.hour12 == 1),
            era: style(fields.era),
            year: style(fields.year),
            month: style(fields.month),
            weekday: style(fields.weekday),
            day: style(fields.day),
            day_period: style(fields.day_period),
            hour: style(fields.hour),
            minute: style(fields.minute),
            second: style(fields.second),
            fractional_second_digits: (fields.fractional_second_digits != FIELD_ABSENT)
                .then_some(fields.fractional_second_digits),
            time_zone_name: style(fields.time_zone_name),
            pattern,
        }
    }
}

/// Unicode::DateTimeFormat::Partition.
pub struct DateTimeFormatPartition {
    pub type_: Utf16String,
    pub value: Utf16String,
    pub source: Utf16String,
}

unsafe extern "C" fn append_date_time_format_part(
    context: *mut c_void,
    type_: *const u16,
    type_length: usize,
    value: *const u16,
    value_length: usize,
    source: *const u16,
    source_length: usize,
) {
    // SAFETY: The context is the list the caller passed, and the export passes valid code units for the lengths.
    let (parts, type_, value, source) = unsafe {
        (
            &mut *context.cast::<Vec<DateTimeFormatPartition>>(),
            core::slice::from_raw_parts(type_, type_length),
            core::slice::from_raw_parts(value, value_length),
            core::slice::from_raw_parts(source, source_length),
        )
    };
    parts.push(DateTimeFormatPartition {
        type_: Utf16String::from_utf16(type_),
        value: Utf16String::from_utf16(value),
        source: Utf16String::from_utf16(source),
    });
}

/// A Unicode::DateTimeFormat, which this owns.
pub struct DateTimeFormat {
    date_time_format: NonNull<c_void>,
}

impl DateTimeFormat {
    fn from_raw(date_time_format: *mut c_void) -> Self {
        Self {
            date_time_format: NonNull::new(date_time_format).expect("Unicode::DateTimeFormat::create returns a format"),
        }
    }

    /// Unicode::DateTimeFormat::create_for_date_and_time_style.
    pub fn create_for_date_and_time_style(
        locale: Utf16View<'_>,
        time_zone_identifier: Utf16View<'_>,
        hour_cycle: Option<HourCycle>,
        hour12: Option<bool>,
        date_style: Option<DateTimeStyle>,
        time_style: Option<DateTimeStyle>,
    ) -> Self {
        // SAFETY: The locale and time zone are valid for their lengths.
        let date_time_format = unsafe {
            unicode_date_time_format_create_for_date_and_time_style(
                UnicodeIntlText::of(locale),
                UnicodeIntlText::of(time_zone_identifier),
                hour_cycle.map_or(FIELD_ABSENT, |hour_cycle| hour_cycle as u8),
                hour12.map_or(FIELD_ABSENT, u8::from),
                date_style.map_or(FIELD_ABSENT, |style| style as u8),
                time_style.map_or(FIELD_ABSENT, |style| style as u8),
            )
        };
        Self::from_raw(date_time_format)
    }

    /// Unicode::DateTimeFormat::create_for_pattern_options.
    pub fn create_for_pattern_options(
        locale: Utf16View<'_>,
        time_zone_identifier: Utf16View<'_>,
        options: &CalendarPattern,
    ) -> Self {
        let pattern = options
            .pattern
            .as_ref()
            .map_or(Utf16View::Ascii(&[]), Utf16View::of_string);
        // SAFETY: The locale, time zone and pattern are valid for their lengths.
        let date_time_format = unsafe {
            unicode_date_time_format_create_for_pattern_options(
                UnicodeIntlText::of(locale),
                UnicodeIntlText::of(time_zone_identifier),
                options.to_fields(),
                options.pattern.is_some(),
                UnicodeIntlText::of(pattern),
            )
        };
        Self::from_raw(date_time_format)
    }

    /// Unicode::DateTimeFormat::chosen_pattern.
    pub fn chosen_pattern(&self) -> CalendarPattern {
        let mut fields = CalendarPattern::default().to_fields();
        // SAFETY: The format is alive, the export writes one set of fields, and the output writes into a Vec.
        let pattern = text_output(|output| unsafe {
            unicode_date_time_format_chosen_pattern(self.date_time_format.as_ptr(), &raw mut fields, output);
        });
        CalendarPattern::from_fields(&fields, Some(pattern))
    }

    pub fn format(&self, time: f64) -> Utf16String {
        // SAFETY: The format is alive, and the output writes into a Vec.
        text_output(|output| unsafe { unicode_date_time_format_format(self.date_time_format.as_ptr(), time, output) })
    }

    pub fn format_to_parts(&self, time: f64) -> Vec<DateTimeFormatPartition> {
        let mut parts: Vec<DateTimeFormatPartition> = Vec::new();
        // SAFETY: The format is alive, and the callback appends to a Vec.
        unsafe {
            unicode_date_time_format_format_to_parts(
                self.date_time_format.as_ptr(),
                time,
                (&raw mut parts).cast(),
                append_date_time_format_part,
            );
        }
        parts
    }

    pub fn format_range(&self, start: f64, end: f64) -> Utf16String {
        // SAFETY: The format is alive, and the output writes into a Vec.
        text_output(|output| unsafe {
            unicode_date_time_format_format_range(self.date_time_format.as_ptr(), start, end, output);
        })
    }

    pub fn format_range_to_parts(&self, start: f64, end: f64) -> Vec<DateTimeFormatPartition> {
        let mut parts: Vec<DateTimeFormatPartition> = Vec::new();
        // SAFETY: The format is alive, and the callback appends to a Vec.
        unsafe {
            unicode_date_time_format_format_range_to_parts(
                self.date_time_format.as_ptr(),
                start,
                end,
                (&raw mut parts).cast(),
                append_date_time_format_part,
            );
        }
        parts
    }
}

impl Drop for DateTimeFormat {
    fn drop(&mut self) {
        // SAFETY: This owns the format, which nothing uses after it is dropped.
        unsafe { unicode_date_time_format_destroy(self.date_time_format.as_ptr()) };
    }
}
