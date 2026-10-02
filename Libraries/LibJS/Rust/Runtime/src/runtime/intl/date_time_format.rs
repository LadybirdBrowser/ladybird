/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{OptionType, big_floor};
use crate::runtime::array::Array;
use crate::runtime::big_int::SignedBigInteger;
use crate::runtime::big_int_algorithms;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::date::{get_utc_epoch_nanoseconds, time_clip};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intl::date_time_format_constructor::{OptionDefaults, OptionInherit, OptionRequired};
use crate::runtime::intl::date_time_format_function::DateTimeFormatFunction;
use crate::runtime::intl::intl_object::{IntlObject, ResolutionOptionDescriptor};
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::temporal::instant::{Instant, NANOSECONDS_PER_MILLISECOND};
use crate::runtime::temporal::plain_date::{PlainDate, create_iso_date_record};
use crate::runtime::temporal::plain_date_time::{PlainDateTime, combine_iso_date_and_time_record};
use crate::runtime::temporal::plain_month_day::PlainMonthDay;
use crate::runtime::temporal::plain_time::{PlainTime, noon_time_record};
use crate::runtime::temporal::plain_year_month::PlainYearMonth;
use crate::runtime::temporal::zoned_date_time::ZonedDateTime;
use crate::unicode::date_time_format::{
    self as unicode, CalendarPattern, CalendarPatternField, CalendarPatternStyle, DateTimeFormatPartition,
    DateTimeStyle,
};
use crate::utf16::Utf16View;

/// The formatter a Value Format Record formats with: [[DateTimeFormat]], or one of the formats of the Temporal types.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ValueFormatter {
    DateTimeFormat,
    TemporalPlainDate,
    TemporalPlainYearMonth,
    TemporalPlainMonthDay,
    TemporalPlainTime,
    TemporalPlainDateTime,
    TemporalInstant,
}

/// 11 DateTimeFormat Objects, https://tc39.es/ecma402/#datetimeformat-objects
#[repr(C)]
#[derive(Trace)]
pub struct DateTimeFormat {
    base: Object,
    #[gc(untraced)]
    locale: GcRefCell<Utf16String>, // [[Locale]]
    #[gc(untraced)]
    calendar: GcRefCell<Utf16String>, // [[Calendar]]
    #[gc(untraced)]
    numbering_system: GcRefCell<Utf16String>, // [[NumberingSystem]]
    #[gc(untraced)]
    time_zone: GcRefCell<Utf16String>, // [[TimeZone]]
    #[gc(untraced)]
    date_style: Cell<Option<DateTimeStyle>>, // [[DateStyle]]
    #[gc(untraced)]
    time_style: Cell<Option<DateTimeStyle>>, // [[TimeStyle]]
    #[gc(untraced)]
    date_time_format: GcRefCell<CalendarPattern>, // [[DateTimeFormat]]
    #[gc(untraced)]
    temporal_plain_date_format: GcRefCell<Option<CalendarPattern>>, // [[TemporalPlainDateFormat]]
    #[gc(untraced)]
    temporal_plain_year_month_format: GcRefCell<Option<CalendarPattern>>, // [[TemporalPlainYearMonthFormat]]
    #[gc(untraced)]
    temporal_plain_month_day_format: GcRefCell<Option<CalendarPattern>>, // [[TemporalPlainMonthDayFormat]]
    #[gc(untraced)]
    temporal_plain_time_format: GcRefCell<Option<CalendarPattern>>, // [[TemporalPlainTimeFormat]]
    #[gc(untraced)]
    temporal_plain_date_time_format: GcRefCell<Option<CalendarPattern>>, // [[TemporalPlainDateTimeFormat]]
    #[gc(untraced)]
    temporal_instant_format: GcRefCell<Option<CalendarPattern>>, // [[TemporalInstantFormat]]
    bound_format: Cell<Option<Gc<DateTimeFormatFunction>>>, // [[BoundFormat]]

    // Non-standard. Stores the ICU date-time formatters for the Intl object's formatting options.
    #[gc(untraced)]
    icu_locale: GcRefCell<Utf16String>,
    #[gc(untraced)]
    formatter: GcRefCell<Option<unicode::DateTimeFormat>>,
    #[gc(untraced)]
    temporal_plain_date_formatter: GcRefCell<Option<unicode::DateTimeFormat>>,
    #[gc(untraced)]
    temporal_plain_year_month_formatter: GcRefCell<Option<unicode::DateTimeFormat>>,
    #[gc(untraced)]
    temporal_plain_month_day_formatter: GcRefCell<Option<unicode::DateTimeFormat>>,
    #[gc(untraced)]
    temporal_plain_time_formatter: GcRefCell<Option<unicode::DateTimeFormat>>,
    #[gc(untraced)]
    temporal_plain_date_time_formatter: GcRefCell<Option<unicode::DateTimeFormat>>,
    #[gc(untraced)]
    temporal_instant_formatter: GcRefCell<Option<unicode::DateTimeFormat>>,
    #[gc(untraced)]
    temporal_time_zone: GcRefCell<Utf16String>,
}

define_cell!(DateTimeFormat, Object, extends: [Object], finalize: finalize);

impl Deref for DateTimeFormat {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for DateTimeFormat {
    fn finalize(&self) {
        for formatter in [
            &self.formatter,
            &self.temporal_plain_date_formatter,
            &self.temporal_plain_year_month_formatter,
            &self.temporal_plain_month_day_formatter,
            &self.temporal_plain_time_formatter,
            &self.temporal_plain_date_time_formatter,
            &self.temporal_instant_formatter,
        ] {
            drop(formatter.replace(None));
        }
    }
}

impl DateTimeFormat {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> DateTimeFormat {
        DateTimeFormat {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            locale: GcRefCell::default(),
            calendar: GcRefCell::default(),
            numbering_system: GcRefCell::default(),
            time_zone: GcRefCell::default(),
            date_style: Cell::new(None),
            time_style: Cell::new(None),
            date_time_format: GcRefCell::default(),
            temporal_plain_date_format: GcRefCell::new(None),
            temporal_plain_year_month_format: GcRefCell::new(None),
            temporal_plain_month_day_format: GcRefCell::new(None),
            temporal_plain_time_format: GcRefCell::new(None),
            temporal_plain_date_time_format: GcRefCell::new(None),
            temporal_instant_format: GcRefCell::new(None),
            bound_format: Cell::new(None),
            icu_locale: GcRefCell::default(),
            formatter: GcRefCell::new(None),
            temporal_plain_date_formatter: GcRefCell::new(None),
            temporal_plain_year_month_formatter: GcRefCell::new(None),
            temporal_plain_month_day_formatter: GcRefCell::new(None),
            temporal_plain_time_formatter: GcRefCell::new(None),
            temporal_plain_date_time_formatter: GcRefCell::new(None),
            temporal_instant_formatter: GcRefCell::new(None),
            temporal_time_zone: GcRefCell::default(),
        }
    }

    pub fn locale(&self) -> Utf16String {
        self.locale.borrow().clone()
    }

    pub fn set_locale(&self, locale: Utf16String) {
        self.locale.replace(locale);
    }

    pub fn icu_locale(&self) -> Utf16String {
        self.icu_locale.borrow().clone()
    }

    pub fn set_icu_locale(&self, icu_locale: Utf16String) {
        self.icu_locale.replace(icu_locale);
    }

    pub fn calendar(&self) -> Utf16String {
        self.calendar.borrow().clone()
    }

    pub fn set_calendar(&self, calendar: Utf16String) {
        self.calendar.replace(calendar);
    }

    pub fn numbering_system(&self) -> Utf16String {
        self.numbering_system.borrow().clone()
    }

    pub fn set_numbering_system(&self, numbering_system: Utf16String) {
        self.numbering_system.replace(numbering_system);
    }

    pub fn time_zone(&self) -> Utf16String {
        self.time_zone.borrow().clone()
    }

    pub fn set_time_zone(&self, time_zone: Utf16String) {
        self.time_zone.replace(time_zone);
    }

    pub fn has_date_style(&self) -> bool {
        self.date_style.get().is_some()
    }

    pub fn date_style(&self) -> Option<DateTimeStyle> {
        self.date_style.get()
    }

    pub fn date_style_string(&self) -> &'static str {
        unicode::date_time_style_to_string(self.date_style.get().expect("the format has a date style"))
    }

    pub fn set_date_style(&self, style: Utf16View<'_>) {
        self.date_style.set(Some(unicode::date_time_style_from_string(style)));
    }

    pub fn has_time_style(&self) -> bool {
        self.time_style.get().is_some()
    }

    pub fn time_style(&self) -> Option<DateTimeStyle> {
        self.time_style.get()
    }

    pub fn time_style_string(&self) -> &'static str {
        unicode::date_time_style_to_string(self.time_style.get().expect("the format has a time style"))
    }

    pub fn set_time_style(&self, style: Utf16View<'_>) {
        self.time_style.set(Some(unicode::date_time_style_from_string(style)));
    }

    pub fn date_time_format(&self) -> CalendarPattern {
        self.date_time_format.borrow().clone()
    }

    pub fn set_date_time_format(&self, date_time_format: CalendarPattern) {
        self.date_time_format.replace(date_time_format);
    }

    pub fn bound_format(&self) -> Option<Gc<DateTimeFormatFunction>> {
        self.bound_format.get()
    }

    pub fn set_bound_format(&self, bound_format: Option<Gc<DateTimeFormatFunction>>) {
        self.bound_format.set(bound_format);
    }

    pub fn set_formatter(&self, formatter: unicode::DateTimeFormat) {
        self.formatter.replace(Some(formatter));
    }

    pub fn set_temporal_plain_date_format(&self, format: Option<CalendarPattern>) {
        self.temporal_plain_date_format.replace(format);
    }

    pub fn set_temporal_plain_year_month_format(&self, format: Option<CalendarPattern>) {
        self.temporal_plain_year_month_format.replace(format);
    }

    pub fn set_temporal_plain_month_day_format(&self, format: Option<CalendarPattern>) {
        self.temporal_plain_month_day_format.replace(format);
    }

    pub fn set_temporal_plain_time_format(&self, format: Option<CalendarPattern>) {
        self.temporal_plain_time_format.replace(format);
    }

    pub fn set_temporal_plain_date_time_format(&self, format: Option<CalendarPattern>) {
        self.temporal_plain_date_time_format.replace(format);
    }

    pub fn set_temporal_instant_format(&self, format: Option<CalendarPattern>) {
        self.temporal_instant_format.replace(format);
    }

    pub fn set_temporal_time_zone(&self, temporal_time_zone: Utf16String) {
        self.temporal_time_zone.replace(temporal_time_zone);
    }

    fn temporal_format_and_formatter(
        &self,
        value_formatter: ValueFormatter,
    ) -> (
        &GcRefCell<Option<CalendarPattern>>,
        &GcRefCell<Option<unicode::DateTimeFormat>>,
    ) {
        match value_formatter {
            ValueFormatter::DateTimeFormat => unreachable!("[[DateTimeFormat]] is not the format of a Temporal type"),
            ValueFormatter::TemporalPlainDate => {
                (&self.temporal_plain_date_format, &self.temporal_plain_date_formatter)
            }
            ValueFormatter::TemporalPlainYearMonth => (
                &self.temporal_plain_year_month_format,
                &self.temporal_plain_year_month_formatter,
            ),
            ValueFormatter::TemporalPlainMonthDay => (
                &self.temporal_plain_month_day_format,
                &self.temporal_plain_month_day_formatter,
            ),
            ValueFormatter::TemporalPlainTime => {
                (&self.temporal_plain_time_format, &self.temporal_plain_time_formatter)
            }
            ValueFormatter::TemporalPlainDateTime => (
                &self.temporal_plain_date_time_format,
                &self.temporal_plain_date_time_formatter,
            ),
            ValueFormatter::TemporalInstant => (&self.temporal_instant_format, &self.temporal_instant_formatter),
        }
    }

    /// The C++ get_or_create_formatter() behind temporal_plain_date_formatter() and the other accessors of the
    /// formatters of the Temporal types: creates the formatter of `value_formatter` the first time it is needed, and
    /// returns whether there is one, which there is not when the type's format is null.
    fn get_or_create_temporal_formatter(&self, value_formatter: ValueFormatter) -> bool {
        let (format, formatter) = self.temporal_format_and_formatter(value_formatter);
        if formatter.borrow().is_some() {
            return true;
        }
        let format = format.borrow();
        let Some(format) = format.as_ref() else {
            return false;
        };

        // The formats of the plain Temporal types format their epoch nanoseconds in UTC.
        let time_zone = if value_formatter == ValueFormatter::TemporalInstant {
            self.temporal_time_zone.borrow().clone()
        } else {
            Utf16String::from_utf8("GMT+00:00")
        };
        let icu_locale = self.icu_locale.borrow();
        formatter.replace(Some(unicode::DateTimeFormat::create_for_pattern_options(
            Utf16View::of_string(&icu_locale),
            Utf16View::of_string(&time_zone),
            format,
        )));
        true
    }

    /// Calls `callback` with the ICU formatter of `value_formatter`, which must not allocate or call into the VM.
    pub fn with_formatter<R>(
        &self,
        value_formatter: ValueFormatter,
        callback: impl FnOnce(&unicode::DateTimeFormat) -> R,
    ) -> R {
        let formatter = if value_formatter == ValueFormatter::DateTimeFormat {
            &self.formatter
        } else {
            self.temporal_format_and_formatter(value_formatter).1
        };
        let formatter = formatter.borrow();
        callback(
            formatter
                .as_ref()
                .expect("a Value Format Record names a formatter the format has"),
        )
    }
}

impl IntlObject for DateTimeFormat {
    // 11.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl.datetimeformat-internal-slots
    fn relevant_extension_keys(&self) -> &'static [&'static str] {
        // The value of the [[RelevantExtensionKeys]] internal slot is « "ca", "hc", "nu" ».
        &["ca", "hc", "nu"]
    }

    // 11.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl.datetimeformat-internal-slots
    fn resolution_option_descriptors<'vm>(&self, vm: &'vm Vm) -> Vec<ResolutionOptionDescriptor<'vm>> {
        // The value of the [[ResolutionOptionDescriptors]] internal slot is « { [[Key]]: "ca", [[Property]]: "calendar" }, { [[Key]]: "nu", [[Property]]: "numberingSystem" }, { [[Key]]: "hour12", [[Property]]: "hour12", [[Type]]: boolean }, { [[Key]]: "hc", [[Property]]: "hourCycle", [[Values]]: « "h11", "h12", "h23", "h24" » } ».
        vec![
            ResolutionOptionDescriptor::string("ca", &vm.names.calendar),
            ResolutionOptionDescriptor::string("nu", &vm.names.numberingSystem),
            ResolutionOptionDescriptor {
                key: "hour12",
                property: &vm.names.hour12,
                type_: OptionType::Boolean,
                values: &[],
            },
            ResolutionOptionDescriptor {
                key: "hc",
                property: &vm.names.hourCycle,
                type_: OptionType::String,
                values: &["h11", "h12", "h23", "h24"],
            },
        ]
    }
}

/// The values FormatDateTime and its range variants format: a Number, or one of the Temporal objects. The
/// toLocaleString methods of the Temporal types pass their object to format_date_time() with the format that
/// create_date_time_format() creates for their locales and options.
#[derive(Clone, Copy)]
pub enum FormattableDateTime {
    Number(f64),
    PlainDate(Gc<PlainDate>),
    PlainYearMonth(Gc<PlainYearMonth>),
    PlainMonthDay(Gc<PlainMonthDay>),
    PlainTime(Gc<PlainTime>),
    PlainDateTime(Gc<PlainDateTime>),
    ZonedDateTime(Gc<ZonedDateTime>),
    Instant(Gc<Instant>),
}

// 15.6.14 Value Format Records, https://tc39.es/proposal-temporal/#datetimeformat-value-format-record
// NB: ICU does not support nanoseconds in its date-time formatter. Thus, we do do not store the epoch nanoseconds as a
//     BigInt here. Instead, we store the epoch in milliseconds as a double.
// NB: We do not create an [[IsPlain]] internal slot. The spec assumes we have a single formatter, and re-use that
//     formatter for each format invocation. Instead, we have separate formatters for each formattable type. So we bake
//     the [[IsPlain]] aspect into each formatter by using UTC as the formatter's time zone as appropriate.
#[derive(Clone, Copy)]
pub struct ValueFormat {
    pub formatter: ValueFormatter, // [[Format]]
    pub epoch_milliseconds: f64,   // [[EpochNanoseconds]]
}

/// A row of Table 16: the field, the property that holds it, and the values GetOption allows for it, which
/// fractionalSecondDigits, a number, has none of.
pub struct CalendarFieldRow<'vm> {
    pub field: CalendarPatternField,
    pub property: &'vm PropertyKey,
    pub values: &'static [&'static str],
}

/// The C++ for_each_calendar_field(): calls `callback` with each row of Table 16, in table order.
pub fn for_each_calendar_field<'vm>(
    vm: &'vm Vm,
    mut callback: impl FnMut(&CalendarFieldRow<'vm>) -> ThrowCompletionOr<()>,
) -> ThrowCompletionOr<()> {
    const NARROW_SHORT_LONG: &[&str] = &["narrow", "short", "long"];
    const TWO_DIGIT_NUMERIC: &[&str] = &["2-digit", "numeric"];
    const TWO_DIGIT_NUMERIC_NARROW_SHORT_LONG: &[&str] = &["2-digit", "numeric", "narrow", "short", "long"];
    const TIME_ZONE: &[&str] = &[
        "short",
        "long",
        "shortOffset",
        "longOffset",
        "shortGeneric",
        "longGeneric",
    ];

    let names = &vm.names;
    let row = |field, property, values| CalendarFieldRow {
        field,
        property,
        values,
    };

    // Table 16: Components of date and time formats, https://tc39.es/ecma402/#table-datetimeformat-components
    for row in [
        row(CalendarPatternField::Weekday, &names.weekday, NARROW_SHORT_LONG),
        row(CalendarPatternField::Era, &names.era, NARROW_SHORT_LONG),
        row(CalendarPatternField::Year, &names.year, TWO_DIGIT_NUMERIC),
        row(
            CalendarPatternField::Month,
            &names.month,
            TWO_DIGIT_NUMERIC_NARROW_SHORT_LONG,
        ),
        row(CalendarPatternField::Day, &names.day, TWO_DIGIT_NUMERIC),
        row(CalendarPatternField::DayPeriod, &names.dayPeriod, NARROW_SHORT_LONG),
        row(CalendarPatternField::Hour, &names.hour, TWO_DIGIT_NUMERIC),
        row(CalendarPatternField::Minute, &names.minute, TWO_DIGIT_NUMERIC),
        row(CalendarPatternField::Second, &names.second, TWO_DIGIT_NUMERIC),
        row(
            CalendarPatternField::FractionalSecondDigits,
            &names.fractionalSecondDigits,
            &[],
        ),
        row(CalendarPatternField::TimeZoneName, &names.timeZoneName, TIME_ZONE),
    ] {
        callback(&row)?;
    }

    Ok(())
}

// 11.5.5 FormatDateTimePattern ( dateTimeFormat, patternParts, x, rangeFormatOptions ), https://tc39.es/ecma402/#sec-formatdatetimepattern
// 15.6.4 FormatDateTimePattern ( dateTimeFormat, format, pattern, epochNanoseconds, isPlain ), https://tc39.es/proposal-temporal/#sec-formatdatetimepattern
pub fn format_date_time_pattern(
    date_time_format: &DateTimeFormat,
    format_record: &ValueFormat,
) -> Vec<DateTimeFormatPartition> {
    date_time_format.with_formatter(format_record.formatter, |formatter| {
        formatter.format_to_parts(format_record.epoch_milliseconds)
    })
}

// 11.5.6 PartitionDateTimePattern ( dateTimeFormat, x ), https://tc39.es/ecma402/#sec-partitiondatetimepattern
// 15.6.5 PartitionDateTimePattern ( dateTimeFormat, x ), https://tc39.es/proposal-temporal/#sec-partitiondatetimepattern
pub fn partition_date_time_pattern(
    vm: &Vm,
    date_time_format: &DateTimeFormat,
    time: &FormattableDateTime,
) -> ThrowCompletionOr<Vec<DateTimeFormatPartition>> {
    // 1. Let xFormatRecord be ? HandleDateTimeValue(dateTimeFormat, x).
    let format_record = handle_date_time_value(vm, date_time_format, time)?;

    // 5. Let result be FormatDateTimePattern(dateTimeFormat, format, pattern, epochNanoseconds, formatRecord.[[IsPlain]]).
    Ok(format_date_time_pattern(date_time_format, &format_record))
}

// 11.5.7 FormatDateTime ( dateTimeFormat, x ), https://tc39.es/ecma402/#sec-formatdatetime
// 15.6.6 FormatDateTime ( dateTimeFormat, x ), https://tc39.es/proposal-temporal/#sec-formatdatetime
pub fn format_date_time(
    vm: &Vm,
    date_time_format: &DateTimeFormat,
    time: &FormattableDateTime,
) -> ThrowCompletionOr<Utf16String> {
    // 1. Let parts be ? PartitionDateTimePattern(dateTimeFormat, x).
    // 2. Let result be the empty String.
    // NOTE: We short-circuit PartitionDateTimePattern as we do not need individual partitions.

    // 1. Let xFormatRecord be ? HandleDateTimeValue(dateTimeFormat, x).
    let format_record = handle_date_time_value(vm, date_time_format, time)?;

    let result = date_time_format.with_formatter(format_record.formatter, |formatter| {
        formatter.format(format_record.epoch_milliseconds)
    });

    // 4. Return result.
    Ok(result)
}

// 11.5.8 FormatDateTimeToParts ( dateTimeFormat, x ), https://tc39.es/ecma402/#sec-formatdatetimetoparts
// 15.6.7 FormatDateTimeToParts ( dateTimeFormat, x ), https://tc39.es/proposal-temporal/#sec-formatdatetimetoparts
pub fn format_date_time_to_parts(
    vm: &Vm,
    date_time_format: &DateTimeFormat,
    time: &FormattableDateTime,
) -> ThrowCompletionOr<Gc<Array>> {
    let realm = vm.current_realm().expect("FormatDateTimeToParts runs in a realm");

    // 1. Let parts be ? PartitionDateTimePattern(dateTimeFormat, x).
    let parts = partition_date_time_pattern(vm, date_time_format, time)?;

    // 2. Let result be ! ArrayCreate(0).
    let result = Array::create(vm, realm, 0, None).must();

    // 3. Let n be 0.
    // 4. For each Record { [[Type]], [[Value]] } part in parts, do
    for (n, part) in parts.into_iter().enumerate() {
        // a. Let O be OrdinaryObjectCreate(%Object.prototype%).
        let object = Object::create(vm, realm, Some(realm.object_prototype()));

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.type_,
                Value::from_string(PrimitiveString::create(vm, part.type_)),
            )
            .must();

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.value,
                Value::from_string(PrimitiveString::create(vm, part.value)),
            )
            .must();

        // d. Perform ! CreateDataProperty(result, ! ToString(n), O).
        result
            .create_data_property_or_throw(vm, &PropertyKey::from_number(n as u64), Value::from_object(object))
            .must();

        // e. Increment n by 1.
    }

    // 5. Return result.
    Ok(result)
}

// 11.5.9 PartitionDateTimeRangePattern ( dateTimeFormat, x, y ), https://tc39.es/ecma402/#sec-partitiondatetimerangepattern
// 15.6.8 PartitionDateTimeRangePattern ( dateTimeFormat, x, y ), https://tc39.es/proposal-temporal/#sec-partitiondatetimerangepattern
pub fn partition_date_time_range_pattern(
    vm: &Vm,
    date_time_format: &DateTimeFormat,
    start: &FormattableDateTime,
    end: &FormattableDateTime,
) -> ThrowCompletionOr<Vec<DateTimeFormatPartition>> {
    // 1. If IsTemporalObject(x) is true or IsTemporalObject(y) is true, then
    if is_temporal_object(start) || is_temporal_object(end) {
        // a. If SameTemporalType(x, y) is false, throw a TypeError exception.
        if !same_temporal_type(start, end) {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::IntlTemporalFormatRangeTypeMismatch,
                &[],
            );
        }
    }

    // 2. Let xFormatRecord be ? HandleDateTimeValue(dateTimeFormat, x).
    let start_format_record = handle_date_time_value(vm, date_time_format, start)?;

    // 3. Let yFormatRecord be ? HandleDateTimeValue(dateTimeFormat, y).
    let end_format_record = handle_date_time_value(vm, date_time_format, end)?;

    Ok(
        date_time_format.with_formatter(start_format_record.formatter, |formatter| {
            formatter.format_range_to_parts(
                start_format_record.epoch_milliseconds,
                end_format_record.epoch_milliseconds,
            )
        }),
    )
}

// 11.5.10 FormatDateTimeRange ( dateTimeFormat, x, y ), https://tc39.es/ecma402/#sec-formatdatetimerange
// 15.6.9 FormatDateTimeRange ( dateTimeFormat, x, y ), https://tc39.es/proposal-temporal/#sec-formatdatetimerange
pub fn format_date_time_range(
    vm: &Vm,
    date_time_format: &DateTimeFormat,
    start: &FormattableDateTime,
    end: &FormattableDateTime,
) -> ThrowCompletionOr<Utf16String> {
    // 1. Let parts be ? PartitionDateTimeRangePattern(dateTimeFormat, x, y).
    // 2. Let result be the empty String.
    // NOTE: We short-circuit PartitionDateTimeRangePattern as we do not need individual partitions.

    // 1. If IsTemporalObject(x) is true or IsTemporalObject(y) is true, then
    if is_temporal_object(start) || is_temporal_object(end) {
        // a. If SameTemporalType(x, y) is false, throw a TypeError exception.
        if !same_temporal_type(start, end) {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::IntlTemporalFormatRangeTypeMismatch,
                &[],
            );
        }
    }

    // 2. Let xFormatRecord be ? HandleDateTimeValue(dateTimeFormat, x).
    let start_format_record = handle_date_time_value(vm, date_time_format, start)?;

    // 3. Let yFormatRecord be ? HandleDateTimeValue(dateTimeFormat, y).
    let end_format_record = handle_date_time_value(vm, date_time_format, end)?;

    let result = date_time_format.with_formatter(start_format_record.formatter, |formatter| {
        formatter.format_range(
            start_format_record.epoch_milliseconds,
            end_format_record.epoch_milliseconds,
        )
    });

    // 4. Return result.
    Ok(result)
}

// 11.5.11 FormatDateTimeRangeToParts ( dateTimeFormat, x, y ), https://tc39.es/ecma402/#sec-formatdatetimerangetoparts
// 15.6.10 FormatDateTimeRangeToParts ( dateTimeFormat, x, y ), https://tc39.es/proposal-temporal/#sec-formatdatetimerangetoparts
pub fn format_date_time_range_to_parts(
    vm: &Vm,
    date_time_format: &DateTimeFormat,
    start: &FormattableDateTime,
    end: &FormattableDateTime,
) -> ThrowCompletionOr<Gc<Array>> {
    let realm = vm.current_realm().expect("FormatDateTimeRangeToParts runs in a realm");

    // 1. Let parts be ? PartitionDateTimeRangePattern(dateTimeFormat, x, y).
    let parts = partition_date_time_range_pattern(vm, date_time_format, start, end)?;

    // 2. Let result be ! ArrayCreate(0).
    let result = Array::create(vm, realm, 0, None).must();

    // 3. Let n be 0.
    // 4. For each Record { [[Type]], [[Value]], [[Source]] } part in parts, do
    for (n, part) in parts.into_iter().enumerate() {
        // a. Let O be OrdinaryObjectCreate(%ObjectPrototype%).
        let object = Object::create(vm, realm, Some(realm.object_prototype()));

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.type_,
                Value::from_string(PrimitiveString::create(vm, part.type_)),
            )
            .must();

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.value,
                Value::from_string(PrimitiveString::create(vm, part.value)),
            )
            .must();

        // d. Perform ! CreateDataPropertyOrThrow(O, "source", part.[[Source]]).
        object
            .create_data_property_or_throw(
                vm,
                &vm.names.source,
                Value::from_string(PrimitiveString::create(vm, part.source)),
            )
            .must();

        // e. Perform ! CreateDataProperty(result, ! ToString(n), O).
        result
            .create_data_property_or_throw(vm, &PropertyKey::from_number(n as u64), Value::from_object(object))
            .must();

        // f. Increment n by 1.
    }

    // 5. Return result.
    Ok(result)
}

// 15.6.1 GetDateTimeFormat ( formats, matcher, options, required, defaults, inherit ), https://tc39.es/proposal-temporal/#sec-getdatetimeformat
pub fn get_date_time_format(
    options: &CalendarPattern,
    required: OptionRequired,
    defaults: OptionDefaults,
    inherit: OptionInherit,
) -> Option<CalendarPattern> {
    use CalendarPatternField::{Day, DayPeriod, FractionalSecondDigits, Hour, Minute, Month, Second, Weekday, Year};

    let required_options: &[CalendarPatternField] = match required {
        // 1. If required is DATE, then
        OptionRequired::Date => {
            // a. Let requiredOptions be « "weekday", "year", "month", "day" ».
            &[Weekday, Year, Month, Day]
        }
        // 2. Else if required is TIME, then
        OptionRequired::Time => {
            // a. Let requiredOptions be « "dayPeriod", "hour", "minute", "second", "fractionalSecondDigits" ».
            &[DayPeriod, Hour, Minute, Second, FractionalSecondDigits]
        }
        // 3. Else if required is YEAR-MONTH, then
        OptionRequired::YearMonth => {
            // a. Let requiredOptions be « "year", "month" ».
            &[Year, Month]
        }
        // 4. Else if required is MONTH-DAY, then
        OptionRequired::MonthDay => {
            // a. Let requiredOptions be « "month", "day" ».
            &[Month, Day]
        }
        // 5. Else,
        OptionRequired::Any => {
            // a. Assert: required is ANY.
            // b. Let requiredOptions be « "weekday", "year", "month", "day", "dayPeriod", "hour", "minute", "second", "fractionalSecondDigits" ».
            &[
                Weekday,
                Year,
                Month,
                Day,
                DayPeriod,
                Hour,
                Minute,
                Second,
                FractionalSecondDigits,
            ]
        }
    };

    let default_options: &[CalendarPatternField] = match defaults {
        // 6. If defaults is DATE, then
        OptionDefaults::Date => {
            // a. Let defaultOptions be « "year", "month", "day" ».
            &[Year, Month, Day]
        }
        // 7. Else if defaults is TIME, then
        OptionDefaults::Time => {
            // a. Let defaultOptions be « "hour", "minute", "second" ».
            &[Hour, Minute, Second]
        }
        // 8. Else if defaults is YEAR-MONTH, then
        OptionDefaults::YearMonth => {
            // a. Let defaultOptions be « "year", "month" ».
            &[Year, Month]
        }
        // 9. Else if defaults is MONTH-DAY, then
        OptionDefaults::MonthDay => {
            // a. Let defaultOptions be « "month", "day" ».
            &[Month, Day]
        }
        // 10. Else,
        OptionDefaults::ZonedDateTime | OptionDefaults::All => {
            // a. Assert: defaults is ZONED-DATE-TIME or ALL.
            // b. Let defaultOptions be « "year", "month", "day", "hour", "minute", "second" ».
            &[Year, Month, Day, Hour, Minute, Second]
        }
    };

    let mut format_options = CalendarPattern::default();

    // 11. If inherit is ALL, then
    if inherit == OptionInherit::All {
        // a. Let formatOptions be a copy of options.
        format_options = options.clone();
    }
    // 12. Else,
    else {
        // a. Let formatOptions be a new Record.

        // b. If required is one of DATE, YEAR-MONTH, or ANY, then
        if matches!(
            required,
            OptionRequired::Date | OptionRequired::YearMonth | OptionRequired::Any
        ) {
            // i. Set formatOptions.[[era]] to options.[[era]].
            format_options.era = options.era;
        }

        // c. If required is either TIME or ANY, then
        if matches!(required, OptionRequired::Time | OptionRequired::Any) {
            // i. Set formatOptions.[[hourCycle]] to options.[[hourCycle]].
            format_options.hour_cycle = options.hour_cycle;
            format_options.hour12 = options.hour12;
        }
    }

    // 13. Let anyPresent be false.
    // 14. For each property name prop of « "weekday", "year", "month", "day", "dayPeriod", "hour", "minute", "second", "fractionalSecondDigits" », do
    //     a. If options.[[<prop>]] is not undefined, set anyPresent to true.
    let any_present = [
        Weekday,
        Year,
        Month,
        Day,
        DayPeriod,
        Hour,
        Minute,
        Second,
        FractionalSecondDigits,
    ]
    .into_iter()
    .any(|field| options.has_field(field));

    // 15. Let needDefaults be true.
    let mut need_defaults = true;

    // 16. For each property name prop of requiredOptions, do
    for &field in required_options {
        // a. Let value be options.[[<prop>]].
        let value = options.field(field);

        // b. If value is not undefined, then
        if value.is_some() {
            // i. Set formatOptions.[[<prop>]] to value.
            format_options.set_field(field, value);

            // ii. Set needDefaults to false.
            need_defaults = false;
        }
    }

    // 17. If needDefaults is true, then
    if need_defaults {
        // a. If anyPresent is true and inherit is RELEVANT, return null.
        if any_present && inherit == OptionInherit::Relevant {
            return None;
        }

        // b. For each property name prop of defaultOptions, do
        for &field in default_options {
            // i. Set formatOptions.[[<prop>]] to "numeric".
            format_options.set_style_field_if_style(field, CalendarPatternStyle::Numeric);
        }

        // c. If defaults is ZONED-DATE-TIME and formatOptions.[[timeZoneName]] is undefined, then
        if defaults == OptionDefaults::ZonedDateTime && format_options.time_zone_name.is_none() {
            // i. Set formatOptions.[[timeZoneName]] to "short".
            format_options.time_zone_name = Some(CalendarPatternStyle::Short);
        }
    }

    // 18. If matcher is "basic", then
    //     a. Let bestFormat be BasicFormatMatcher(formatOptions, formats).
    // 19. Else,
    //     a. Let bestFormat be BestFitFormatMatcher(formatOptions, formats).
    // 20. Return bestFormat.
    Some(format_options)
}

// 15.6.2 AdjustDateTimeStyleFormat ( formats, baseFormat, matcher, allowedOptions ), https://tc39.es/proposal-temporal/#sec-adjustdatetimestyleformat
pub fn adjust_date_time_style_format(
    vm: &Vm,
    base_format: &CalendarPattern,
    allowed_options: &[CalendarPatternField],
) -> CalendarPattern {
    // 1. Let anyConflictingFields be false.
    let mut any_conflicted_fields = false;

    // 2. For each row of Table 16, except the header row, in table order, do
    for_each_calendar_field(vm, |row| {
        // a. Let prop be the name given in the "Property" column of the current row.
        // b. If baseFormat has a [[<prop>]] field and allowedOptions does not contain prop, set anyConflictingFields to true.
        if base_format.has_field(row.field) && !allowed_options.contains(&row.field) {
            any_conflicted_fields = true;
        }
        Ok(())
    })
    .must();

    // 3. If anyConflictingFields is false, return baseFormat.
    if !any_conflicted_fields {
        return base_format.clone();
    }

    // 4. NOTE: The above steps prevent the operation from returning an altered format when baseFormat would be sufficient.
    //    This should be unnecessary, but exists because the ECMA-402 specification does not guarantee that a format
    //    returned from DateTimeStyleFormat can also be returned from BasicFormatMatcher or BestFitFormatMatcher.

    // 5. Let formatOptions be a new Record.
    let mut format_options = CalendarPattern::default();

    // 6. For each property name prop of allowedOptions, do
    for &field in allowed_options {
        // a. If baseFormat has a [[<prop>]] field, set formatOptions.[[<prop>]] to baseFormat.[[<prop>]].
        if let Some(value) = base_format.field(field) {
            format_options.set_field(field, Some(value));
        }
    }

    // 7. If matcher is "basic", then
    //     a. Let bestFormat be BasicFormatMatcher(formatOptions, formats).
    // 8. Else,
    //     a. Let bestFormat be BestFitFormatMatcher(formatOptions, formats).
    // 9. Return bestFormat.
    format_options
}

// 15.6.11 ToDateTimeFormattable ( value ), https://tc39.es/proposal-temporal/#sec-todatetimeformattable
pub fn to_date_time_formattable(vm: &Vm, value: Value) -> ThrowCompletionOr<FormattableDateTime> {
    // 1. If IsTemporalObject(value) is true, return value.
    if value.is_object() {
        let object = value.as_object();
        if let Some(instant) = object.downcast::<Instant>() {
            return Ok(FormattableDateTime::Instant(instant));
        }
        if let Some(plain_date) = object.downcast::<PlainDate>() {
            return Ok(FormattableDateTime::PlainDate(plain_date));
        }
        if let Some(plain_date_time) = object.downcast::<PlainDateTime>() {
            return Ok(FormattableDateTime::PlainDateTime(plain_date_time));
        }
        if let Some(plain_month_day) = object.downcast::<PlainMonthDay>() {
            return Ok(FormattableDateTime::PlainMonthDay(plain_month_day));
        }
        if let Some(plain_time) = object.downcast::<PlainTime>() {
            return Ok(FormattableDateTime::PlainTime(plain_time));
        }
        if let Some(plain_year_month) = object.downcast::<PlainYearMonth>() {
            return Ok(FormattableDateTime::PlainYearMonth(plain_year_month));
        }
        if let Some(zoned_date_time) = object.downcast::<ZonedDateTime>() {
            return Ok(FormattableDateTime::ZonedDateTime(zoned_date_time));
        }
    }

    // 2. Return ? ToNumber(value).
    Ok(FormattableDateTime::Number(value.to_number(vm)?.as_f64()))
}

// 15.6.12 IsTemporalObject ( value ), https://tc39.es/proposal-temporal/#sec-temporal-istemporalobject
pub fn is_temporal_object(value: &FormattableDateTime) -> bool {
    // 1. If value is not an Object, return false.
    // 2. If value does not have an [[InitializedTemporalDate]], [[InitializedTemporalTime]], [[InitializedTemporalDateTime]],
    //    [[InitializedTemporalZonedDateTime]], [[InitializedTemporalYearMonth]], [[InitializedTemporalMonthDay]], or
    //    [[InitializedTemporalInstant]] internal slot, return false.
    // 3. Return true.
    !matches!(value, FormattableDateTime::Number(_))
}

// 15.6.13 SameTemporalType ( x, y ), https://tc39.es/proposal-temporal/#sec-temporal-istemporalobject
pub fn same_temporal_type(x: &FormattableDateTime, y: &FormattableDateTime) -> bool {
    // 1. If either of IsTemporalObject(x) or IsTemporalObject(y) is false, return false.
    if !is_temporal_object(x) || !is_temporal_object(y) {
        return false;
    }

    // 2. If x has an [[InitializedTemporalDate]] internal slot and y does not, return false.
    // 3. If x has an [[InitializedTemporalTime]] internal slot and y does not, return false.
    // 4. If x has an [[InitializedTemporalDateTime]] internal slot and y does not, return false.
    // 5. If x has an [[InitializedTemporalZonedDateTime]] internal slot and y does not, return false.
    // 6. If x has an [[InitializedTemporalYearMonth]] internal slot and y does not, return false.
    // 7. If x has an [[InitializedTemporalMonthDay]] internal slot and y does not, return false.
    // 8. If x has an [[InitializedTemporalInstant]] internal slot and y does not, return false.
    // 9. Return true.
    core::mem::discriminant(x) == core::mem::discriminant(y)
}

fn to_epoch_milliseconds(epoch_nanoseconds: &SignedBigInteger) -> f64 {
    big_int_algorithms::to_double(&big_floor(epoch_nanoseconds, &NANOSECONDS_PER_MILLISECOND))
}

fn throw_invalid_calendar<T>(
    vm: &Vm,
    type_name: &str,
    calendar: &Utf16String,
    date_time_format_calendar: &Utf16String,
) -> ThrowCompletionOr<T> {
    vm.throw_completion(
        ErrorKind::RangeError,
        ErrorType::IntlTemporalInvalidCalendar,
        &[&type_name, calendar, date_time_format_calendar],
    )
}

/// Steps 4 to 6 of the HandleDateTimeTemporal operations of the plain types that have a format that can be null.
fn plain_value_format(
    vm: &Vm,
    date_time_format: &DateTimeFormat,
    value_formatter: ValueFormatter,
    type_name: &str,
    epoch_nanoseconds: &SignedBigInteger,
) -> ThrowCompletionOr<ValueFormat> {
    // 4. Let format be dateTimeFormat.[[Temporal<Type>Format]].
    // 5. If format is null, throw a TypeError exception.
    if !date_time_format.get_or_create_temporal_formatter(value_formatter) {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::IntlTemporalFormatIsNull, &[&type_name]);
    }

    // 6. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: epochNs, [[IsPlain]]: true  }.
    Ok(ValueFormat {
        formatter: value_formatter,
        epoch_milliseconds: to_epoch_milliseconds(epoch_nanoseconds),
    })
}

// 15.6.15 HandleDateTimeTemporalDate ( dateTimeFormat, temporalDate ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporaldate
pub fn handle_date_time_temporal_date(
    vm: &Vm,
    date_time_format: &DateTimeFormat,
    temporal_date: &PlainDate,
) -> ThrowCompletionOr<ValueFormat> {
    // 1. If temporalDate.[[Calendar]] is not either dateTimeFormat.[[Calendar]] or "iso8601", throw a RangeError exception.
    let date_time_format_calendar = date_time_format.calendar();
    let calendar = temporal_date.calendar();
    if calendar != date_time_format_calendar && Utf16View::of_string(&calendar) != "iso8601" {
        return throw_invalid_calendar(vm, "Temporal.PlainDate", &calendar, &date_time_format_calendar);
    }

    // 2. Let isoDateTime be CombineISODateAndTimeRecord(temporalDate.[[ISODate]], NoonTimeRecord()).
    let iso_date_time = combine_iso_date_and_time_record(temporal_date.iso_date(), noon_time_record());

    // 3. Let epochNs be GetUTCEpochNanoseconds(isoDateTime).
    let epoch_nanoseconds = get_utc_epoch_nanoseconds(&iso_date_time);

    // 4. Let format be dateTimeFormat.[[TemporalPlainDateFormat]].
    // 5. If format is null, throw a TypeError exception.
    // 6. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: epochNs, [[IsPlain]]: true  }.
    plain_value_format(
        vm,
        date_time_format,
        ValueFormatter::TemporalPlainDate,
        "Temporal.PlainDate",
        &epoch_nanoseconds,
    )
}

// 15.6.16 HandleDateTimeTemporalYearMonth ( dateTimeFormat, temporalYearMonth ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporalyearmonth
pub fn handle_date_time_temporal_year_month(
    vm: &Vm,
    date_time_format: &DateTimeFormat,
    temporal_year_month: &PlainYearMonth,
) -> ThrowCompletionOr<ValueFormat> {
    // 1. If temporalYearMonth.[[Calendar]] is not equal to dateTimeFormat.[[Calendar]], throw a RangeError exception.
    let date_time_format_calendar = date_time_format.calendar();
    let calendar = temporal_year_month.calendar();
    if calendar != date_time_format_calendar {
        return throw_invalid_calendar(vm, "Temporal.PlainYearMonth", &calendar, &date_time_format_calendar);
    }

    // 2. Let isoDateTime be CombineISODateAndTimeRecord(temporalYearMonth.[[ISODate]], NoonTimeRecord()).
    let iso_date_time = combine_iso_date_and_time_record(temporal_year_month.iso_date(), noon_time_record());

    // 3. Let epochNs be GetUTCEpochNanoseconds(isoDateTime).
    let epoch_nanoseconds = get_utc_epoch_nanoseconds(&iso_date_time);

    // 4. Let format be dateTimeFormat.[[TemporalPlainYearMonthFormat]].
    // 5. If format is null, throw a TypeError exception.
    // 6. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: epochNs, [[IsPlain]]: true  }.
    plain_value_format(
        vm,
        date_time_format,
        ValueFormatter::TemporalPlainYearMonth,
        "Temporal.PlainYearMonth",
        &epoch_nanoseconds,
    )
}

// 15.6.17 HandleDateTimeTemporalMonthDay ( dateTimeFormat, temporalMonthDay ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporalmonthday
pub fn handle_date_time_temporal_month_day(
    vm: &Vm,
    date_time_format: &DateTimeFormat,
    temporal_month_day: &PlainMonthDay,
) -> ThrowCompletionOr<ValueFormat> {
    // 1. If temporalMonthDay.[[Calendar]] is not equal to dateTimeFormat.[[Calendar]], throw a RangeError exception.
    let date_time_format_calendar = date_time_format.calendar();
    let calendar = temporal_month_day.calendar();
    if calendar != date_time_format_calendar {
        return throw_invalid_calendar(vm, "Temporal.PlainMonthDay", &calendar, &date_time_format_calendar);
    }

    // 2. Let isoDateTime be CombineISODateAndTimeRecord(temporalMonthDay.[[ISODate]], NoonTimeRecord()).
    let iso_date_time = combine_iso_date_and_time_record(temporal_month_day.iso_date(), noon_time_record());

    // 3. Let epochNs be GetUTCEpochNanoseconds(isoDateTime).
    let epoch_nanoseconds = get_utc_epoch_nanoseconds(&iso_date_time);

    // 4. Let format be dateTimeFormat.[[TemporalPlainMonthDayFormat]].
    // 5. If format is null, throw a TypeError exception.
    // 6. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: epochNs, [[IsPlain]]: true  }.
    plain_value_format(
        vm,
        date_time_format,
        ValueFormatter::TemporalPlainMonthDay,
        "Temporal.PlainMonthDay",
        &epoch_nanoseconds,
    )
}

// 15.6.18 HandleDateTimeTemporalTime ( dateTimeFormat, temporalTime ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporaltime
pub fn handle_date_time_temporal_time(
    vm: &Vm,
    date_time_format: &DateTimeFormat,
    temporal_time: &PlainTime,
) -> ThrowCompletionOr<ValueFormat> {
    // 1. Let isoDate be CreateISODateRecord(1970, 1, 1).
    let iso_date = create_iso_date_record(1970.0, 1.0, 1.0);

    // 2. Let isoDateTime be CombineISODateAndTimeRecord(isoDate, temporalTime.[[Time]]).
    let iso_date_time = combine_iso_date_and_time_record(iso_date, temporal_time.time());

    // 3. Let epochNs be GetUTCEpochNanoseconds(isoDateTime).
    let epoch_nanoseconds = get_utc_epoch_nanoseconds(&iso_date_time);

    // 4. Let format be dateTimeFormat.[[TemporalPlainTimeFormat]].
    // 5. If format is null, throw a TypeError exception.
    // 6. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: epochNs, [[IsPlain]]: true  }.
    plain_value_format(
        vm,
        date_time_format,
        ValueFormatter::TemporalPlainTime,
        "Temporal.PlainTime",
        &epoch_nanoseconds,
    )
}

// 15.6.19 HandleDateTimeTemporalDateTime ( dateTimeFormat, dateTime ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporaldatetime
pub fn handle_date_time_temporal_date_time(
    vm: &Vm,
    date_time_format: &DateTimeFormat,
    date_time: &PlainDateTime,
) -> ThrowCompletionOr<ValueFormat> {
    // 1. If dateTime.[[Calendar]] is not "iso8601" and not equal to dateTimeFormat.[[Calendar]], throw a RangeError exception.
    let date_time_format_calendar = date_time_format.calendar();
    let calendar = date_time.calendar();
    if calendar != date_time_format_calendar && Utf16View::of_string(&calendar) != "iso8601" {
        return throw_invalid_calendar(vm, "Temporal.PlainDateTime", &calendar, &date_time_format_calendar);
    }

    // 2. Let epochNs be GetUTCEpochNanoseconds(dateTime.[[ISODateTime]]).
    let epoch_nanoseconds = get_utc_epoch_nanoseconds(&date_time.iso_date_time());

    // 3. Let format be dateTimeFormat.[[TemporalPlainDateTimeFormat]].
    let has_formatter = date_time_format.get_or_create_temporal_formatter(ValueFormatter::TemporalPlainDateTime);
    assert!(has_formatter);

    // 4. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: epochNs, [[IsPlain]]: true  }.
    Ok(ValueFormat {
        formatter: ValueFormatter::TemporalPlainDateTime,
        epoch_milliseconds: to_epoch_milliseconds(&epoch_nanoseconds),
    })
}

// 15.6.20 HandleDateTimeTemporalInstant ( dateTimeFormat, instant ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporalinstant
pub fn handle_date_time_temporal_instant(date_time_format: &DateTimeFormat, instant: &Instant) -> ValueFormat {
    // 1. Let format be dateTimeFormat.[[TemporalInstantFormat]].
    let has_formatter = date_time_format.get_or_create_temporal_formatter(ValueFormatter::TemporalInstant);
    assert!(has_formatter);

    // 2. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: instant.[[EpochNanoseconds]], [[IsPlain]]: false  }.
    ValueFormat {
        formatter: ValueFormatter::TemporalInstant,
        epoch_milliseconds: to_epoch_milliseconds(instant.epoch_nanoseconds().big_integer()),
    }
}

// 15.6.21 HandleDateTimeOthers ( dateTimeFormat, x ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimeothers
pub fn handle_date_time_others(
    vm: &Vm,
    _date_time_format: &DateTimeFormat,
    time: f64,
) -> ThrowCompletionOr<ValueFormat> {
    // 1. Set x to TimeClip(x).
    let time = time_clip(time);

    // 2. If x is NaN, throw a RangeError exception.
    if time.is_nan() {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::IntlInvalidTime, &[]);
    }

    // 3. Let epochNanoseconds be ℤ(ℝ(x) × 10**6).

    // 4. Let format be dateTimeFormat.[[DateTimeFormat]].
    // 5. Return Value Format Record { [[Format]]: format, [[EpochNanoseconds]]: epochNanoseconds, [[IsPlain]]: false  }.
    Ok(ValueFormat {
        formatter: ValueFormatter::DateTimeFormat,
        epoch_milliseconds: time,
    })
}

// 15.6.22 HandleDateTimeValue ( dateTimeFormat, x ), https://tc39.es/proposal-temporal/#sec-temporal-handledatetimevalue
pub fn handle_date_time_value(
    vm: &Vm,
    date_time_format: &DateTimeFormat,
    formattable: &FormattableDateTime,
) -> ThrowCompletionOr<ValueFormat> {
    match *formattable {
        // 1. If x is a Number, return ? HandleDateTimeOthers(dateTimeFormat, x).
        FormattableDateTime::Number(time) => handle_date_time_others(vm, date_time_format, time),
        // 2. If x has an [[InitializedTemporalDate]] internal slot, return ? HandleDateTimeTemporalDate(dateTimeFormat, x).
        FormattableDateTime::PlainDate(temporal_date) => {
            handle_date_time_temporal_date(vm, date_time_format, &temporal_date)
        }
        // 3. If x has an [[InitializedTemporalYearMonth]] internal slot, return ? HandleDateTimeTemporalYearMonth(dateTimeFormat, x).
        FormattableDateTime::PlainYearMonth(temporal_year_month) => {
            handle_date_time_temporal_year_month(vm, date_time_format, &temporal_year_month)
        }
        // 4. If x has an [[InitializedTemporalMonthDay]] internal slot, return ? HandleDateTimeTemporalMonthDay(dateTimeFormat, x).
        FormattableDateTime::PlainMonthDay(temporal_month_day) => {
            handle_date_time_temporal_month_day(vm, date_time_format, &temporal_month_day)
        }
        // 5. If x has an [[InitializedTemporalTime]] internal slot, return ? HandleDateTimeTemporalTime(dateTimeFormat, x).
        FormattableDateTime::PlainTime(temporal_time) => {
            handle_date_time_temporal_time(vm, date_time_format, &temporal_time)
        }
        // 6. If x has an [[InitializedTemporalDateTime]] internal slot, return ? HandleDateTimeTemporalDateTime(dateTimeFormat, x).
        FormattableDateTime::PlainDateTime(date_time) => {
            handle_date_time_temporal_date_time(vm, date_time_format, &date_time)
        }
        // 7. If x has an [[InitializedTemporalInstant]] internal slot, return HandleDateTimeTemporalInstant(dateTimeFormat, x).
        FormattableDateTime::Instant(instant) => Ok(handle_date_time_temporal_instant(date_time_format, &instant)),
        // 8. Assert: x has an [[InitializedTemporalZonedDateTime]] internal slot.
        FormattableDateTime::ZonedDateTime(_) => {
            // 9. Throw a TypeError exception.
            vm.throw_completion(ErrorKind::TypeError, ErrorType::IntlTemporalZonedDateTime, &[])
        }
    }
}
