/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Libraries/LibJS/Runtime/Temporal/PlainMonthDay.cpp: Temporal.PlainMonthDay objects.

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{get_options_object, ordinary_create_from_constructor_of};
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::temporal::abstract_operations::{
    DateType, Overflow, ShowCalendar, ascii_view, get_temporal_overflow_option, iso_date_to_fields, parse_iso_date_time,
};
use crate::runtime::temporal::calendar::{
    CalendarField, CalendarFieldListOrPartial, ISO8601_CALENDAR, calendar_month_day_from_fields, canonicalize_calendar,
    format_calendar_annotation, get_temporal_calendar_identifier_with_iso_default, prepare_calendar_fields,
};
use crate::runtime::temporal::iso_records::ISODate;
use crate::runtime::temporal::iso8601::Production;
use crate::runtime::temporal::plain_date::{create_iso_date_record, iso_date_within_limits, pad_iso_year};
use crate::utf16::Utf16View;

// 10 Temporal.PlainMonthDay Objects, https://tc39.es/proposal-temporal/#sec-temporal-plainmonthday-objects
#[repr(C)]
#[derive(Trace)]
pub struct PlainMonthDay {
    base: Object,
    #[gc(untraced)]
    iso_date: ISODate, // [[ISODate]]
    calendar: Utf16String, // [[Calendar]]
}

define_cell!(PlainMonthDay, Object, extends: [Object]);

impl core::ops::Deref for PlainMonthDay {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl PlainMonthDay {
    fn new(vm: &Vm, iso_date: ISODate, calendar: Utf16String, prototype: Gc<Object>) -> PlainMonthDay {
        PlainMonthDay {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            iso_date,
            calendar,
        }
    }

    pub fn iso_date(&self) -> ISODate {
        self.iso_date
    }

    pub fn calendar(&self) -> Utf16String {
        self.calendar.clone()
    }
}

// 10.5.1 ToTemporalMonthDay ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalmonthday
pub fn to_temporal_month_day(vm: &Vm, item: Value, options: Value) -> ThrowCompletionOr<Gc<PlainMonthDay>> {
    // 1. If options is not present, set options to undefined.

    // 2. If item is an Object, then
    if item.is_object() {
        let object = item.as_object();

        // a. If item has an [[InitializedTemporalMonthDay]] internal slot, then
        if let Some(plain_month_day) = object.downcast::<PlainMonthDay>() {
            // i. Let resolvedOptions be ? GetOptionsObject(options).
            let resolved_options = get_options_object(vm, options)?;

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            get_temporal_overflow_option(vm, &resolved_options)?;

            // iii. Return ! CreateTemporalMonthDay(item.[[ISODate]], item.[[Calendar]]).
            return Ok(
                create_temporal_month_day(vm, plain_month_day.iso_date(), plain_month_day.calendar(), None).must(),
            );
        }

        // b. Let calendar be ? GetTemporalCalendarIdentifierWithISODefault(item).
        let calendar = get_temporal_calendar_identifier_with_iso_default(vm, &object)?;

        // c. Let fields be ? PrepareCalendarFields(calendar, item, « YEAR, MONTH, MONTH-CODE, DAY », «», «»).
        let mut fields = prepare_calendar_fields(
            vm,
            Utf16View::of_string(&calendar),
            &object,
            &[
                CalendarField::Year,
                CalendarField::Month,
                CalendarField::MonthCode,
                CalendarField::Day,
            ],
            &[],
            CalendarFieldListOrPartial::List(&[]),
        )?;

        // d. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, options)?;

        // e. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

        // f. Let isoDate be ? CalendarMonthDayFromFields(calendar, fields, overflow).
        let iso_date = calendar_month_day_from_fields(vm, Utf16View::of_string(&calendar), &mut fields, overflow)?;

        // g. Return ! CreateTemporalMonthDay(isoDate, calendar).
        return Ok(create_temporal_month_day(vm, iso_date, calendar, None).must());
    }

    // 3. If item is not a String, throw a TypeError exception.
    if !item.is_string() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::TemporalInvalidPlainMonthDay, &[]);
    }

    // 4. Let result be ? ParseISODateTime(item, « TemporalMonthDayString »).
    let item_string = item.as_string().utf16_string();
    let parse_result = parse_iso_date_time(
        vm,
        Utf16View::of_string(&item_string),
        &[Production::TemporalMonthDayString],
    )?;

    // 5. Let calendar be result.[[Calendar]].
    // 6. If calendar is empty, set calendar to "iso8601".
    let calendar = match &parse_result.calendar {
        Some(calendar) => canonicalize_calendar(vm, Utf16View::of_string(calendar))?,
        None => canonicalize_calendar(vm, ascii_view(ISO8601_CALENDAR))?,
    };

    // 8. Let resolvedOptions be ? GetOptionsObject(options).
    let resolved_options = get_options_object(vm, options)?;

    // 9. Perform ? GetTemporalOverflowOption(resolvedOptions).
    get_temporal_overflow_option(vm, &resolved_options)?;

    // 10. If calendar is "iso8601", then
    if Utf16View::of_string(&calendar) == ISO8601_CALENDAR {
        // a. Let referenceISOYear be 1972 (the first ISO 8601 leap year after the epoch).
        const REFERENCE_ISO_YEAR: f64 = 1972.0;

        // b. Let isoDate be CreateISODateRecord(referenceISOYear, result.[[Month]], result.[[Day]]).
        let iso_date = create_iso_date_record(
            REFERENCE_ISO_YEAR,
            f64::from(parse_result.month),
            f64::from(parse_result.day),
        );

        // c. Return ! CreateTemporalMonthDay(isoDate, calendar).
        return Ok(create_temporal_month_day(vm, iso_date, calendar, None).must());
    }

    // 11. Let isoDate be CreateISODateRecord(result.[[Year]], result.[[Month]], result.[[Day]]).
    let iso_date = create_iso_date_record(
        f64::from(
            parse_result
                .year
                .expect("a month-day string with a calendar other than iso8601 has a year"),
        ),
        f64::from(parse_result.month),
        f64::from(parse_result.day),
    );

    // 12. If ISODateWithinLimits(isoDate) is false, throw a RangeError exception.
    if !iso_date_within_limits(iso_date) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainMonthDay, &[]);
    }

    // 13. Set result to ISODateToFields(calendar, isoDate, MONTH-DAY).
    let mut result = iso_date_to_fields(Utf16View::of_string(&calendar), iso_date, DateType::MonthDay);

    // 14. NOTE: The following operation is called with CONSTRAIN regardless of overflow, in order for the calendar to
    //     store a canonical value in the [[Year]] field of the [[ISODate]] internal slot of the result.
    // 15. Set isoDate to ? CalendarMonthDayFromFields(calendar, result, CONSTRAIN).
    let iso_date =
        calendar_month_day_from_fields(vm, Utf16View::of_string(&calendar), &mut result, Overflow::Constrain)?;

    // 16. Return ! CreateTemporalMonthDay(isoDate, calendar).
    Ok(create_temporal_month_day(vm, iso_date, calendar, None).must())
}

// 10.5.2 CreateTemporalMonthDay ( isoDate, calendar [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporalmonthday
pub fn create_temporal_month_day(
    vm: &Vm,
    iso_date: ISODate,
    calendar: Utf16String,
    new_target: Option<Gc<FunctionObject>>,
) -> ThrowCompletionOr<Gc<PlainMonthDay>> {
    let realm = vm.current_realm().expect("CreateTemporalMonthDay runs in a realm");

    // 1. If ISODateWithinLimits(isoDate) is false, throw a RangeError exception.
    if !iso_date_within_limits(iso_date) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainMonthDay, &[]);
    }

    // 2. If newTarget is not present, set newTarget to %Temporal.PlainMonthDay%.
    let new_target = new_target.unwrap_or_else(|| realm.intrinsics().temporal_plain_month_day_constructor(vm));

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainMonthDay.prototype%", « [[InitializedTemporalMonthDay]], [[ISODate]], [[Calendar]] »).
    // 4. Set object.[[ISODate]] to isoDate.
    // 5. Set object.[[Calendar]] to calendar.
    let object = ordinary_create_from_constructor_of(
        vm,
        realm,
        new_target,
        Intrinsics::temporal_plain_month_day_prototype,
        |prototype| PlainMonthDay::new(vm, iso_date, calendar, prototype),
    )?;

    // 6. Return object.
    Ok(object)
}

// 10.5.3 TemporalMonthDayToString ( monthDay, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-temporalmonthdaytostring
pub fn temporal_month_day_to_string(month_day: &PlainMonthDay, show_calendar: ShowCalendar) -> String {
    let iso_date = month_day.iso_date();
    let calendar = month_day.calendar();

    // 1. Let month be ToZeroPaddedDecimalString(monthDay.[[ISODate]].[[Month]], 2).
    // 2. Let day be ToZeroPaddedDecimalString(monthDay.[[ISODate]].[[Day]], 2).
    // 3. Let result be the string-concatenation of month, the code unit 0x002D (HYPHEN-MINUS), and day.
    let mut result = format!("{:02}-{:02}", iso_date.month, iso_date.day);

    // 4. If showCalendar is one of ALWAYS or CRITICAL, or monthDay.[[Calendar]] is not "iso8601", then
    if show_calendar == ShowCalendar::Always
        || show_calendar == ShowCalendar::Critical
        || Utf16View::of_string(&calendar) != ISO8601_CALENDAR
    {
        // a. Let year be PadISOYear(monthDay.[[ISODate]].[[Year]]).
        let year = pad_iso_year(iso_date.year);

        // b. Set result to the string-concatenation of year, the code unit 0x002D (HYPHEN-MINUS), and result.
        result = format!("{year}-{result}");
    }

    // 5. Let calendarString be FormatCalendarAnnotation(monthDay.[[Calendar]], showCalendar).
    let calendar_string = format_calendar_annotation(Utf16View::of_string(&calendar), show_calendar);

    // 6. Set result to the string-concatenation of result and calendarString.
    result.push_str(&calendar_string);

    // 7. Return result.
    result
}
