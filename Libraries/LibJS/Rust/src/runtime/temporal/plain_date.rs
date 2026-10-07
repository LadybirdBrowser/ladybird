/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Temporal.PlainDate objects and the ISO Date Record operations.

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
use crate::runtime::date::{get_utc_epoch_nanoseconds, is_within_i32_range, is_within_u8_range};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::temporal::abstract_operations::{
    ArithmeticOperation, DurationOperation, Overflow, ShowCalendar, Unit, UnitGroup, ascii_view,
    epoch_days_to_epoch_ms, get_difference_settings, get_temporal_overflow_option, iso_date_to_epoch_days,
    parse_iso_date_time,
};
use crate::runtime::temporal::calendar::{
    CalendarDate, CalendarField, CalendarFieldListOrPartial, ISO8601_CALENDAR, calendar_date_add,
    calendar_date_from_fields, calendar_date_until, calendar_equals, calendar_iso_to_date, canonicalize_calendar,
    format_calendar_annotation, get_temporal_calendar_identifier_with_iso_default, iso_days_in_month,
    iso8601_calendar_view, prepare_calendar_fields,
};
use crate::runtime::temporal::date_equations::{
    epoch_time_to_date, epoch_time_to_epoch_year, epoch_time_to_month_in_year,
};
use crate::runtime::temporal::duration::{
    Duration, DurationFields, combine_date_and_time_duration, create_negated_temporal_duration,
    create_temporal_duration, round_relative_duration, temporal_duration_from_internal,
    to_date_duration_record_without_time, to_temporal_duration,
};
use crate::runtime::temporal::iso_records::{ISODate, TimeDuration};
use crate::runtime::temporal::iso8601::Production;
use crate::runtime::temporal::plain_date_time::{
    PlainDateTime, combine_iso_date_and_time_record, iso_date_time_within_limits,
};
use crate::runtime::temporal::plain_time::{midnight_time_record, noon_time_record};
use crate::runtime::temporal::plain_year_month::balance_iso_year_month;
use crate::runtime::temporal::time_zone::get_iso_date_time_for;
use crate::runtime::temporal::zoned_date_time::ZonedDateTime;
use crate::utf16::Utf16View;

// 3 Temporal.PlainDate Objects, https://tc39.es/proposal-temporal/#sec-temporal-plaindate-objects
#[repr(C)]
#[derive(Trace)]
pub struct PlainDate {
    base: Object,
    #[gc(untraced)]
    iso_date: ISODate, // [[ISODate]]
    calendar: Utf16String, // [[Calendar]]
}

define_cell!(PlainDate, Object, extends: [Object]);

impl core::ops::Deref for PlainDate {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl PlainDate {
    fn new(vm: &Vm, iso_date: ISODate, calendar: Utf16String, prototype: Gc<Object>) -> PlainDate {
        PlainDate {
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

// 3.5.2 CreateISODateRecord ( year, month, day ), https://tc39.es/proposal-temporal/#sec-temporal-create-iso-date-record
pub fn create_iso_date_record(year: f64, month: f64, day: f64) -> ISODate {
    // 1. Assert: IsValidISODate(year, month, day) is true.
    assert!(is_valid_iso_date(year, month, day));

    // 2. Return ISO Date Record { [[Year]]: year, [[Month]]: month, [[Day]]: day }.
    ISODate {
        year: year as i32,
        month: month as u8,
        day: day as u8,
    }
}

// 3.5.3 CreateTemporalDate ( isoDate, calendar [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporaldate
pub fn create_temporal_date(
    vm: &Vm,
    iso_date: ISODate,
    calendar: Utf16String,
    new_target: Option<Gc<FunctionObject>>,
) -> ThrowCompletionOr<Gc<PlainDate>> {
    let realm = vm.current_realm().expect("CreateTemporalDate runs in a realm");

    // 1. If ISODateWithinLimits(isoDate) is false, throw a RangeError exception.
    if !iso_date_within_limits(iso_date) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainDate, &[]);
    }

    // 2. If newTarget is not present, set newTarget to %Temporal.PlainDate%.
    let new_target = new_target.unwrap_or_else(|| realm.intrinsics().temporal_plain_date_constructor(vm));

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainDate.prototype%", « [[InitializedTemporalDate]], [[ISODate]], [[Calendar]] »).
    // 4. Set object.[[ISODate]] to isoDate.
    // 5. Set object.[[Calendar]] to calendar.
    let object = ordinary_create_from_constructor_of(
        vm,
        realm,
        new_target,
        Intrinsics::temporal_plain_date_prototype,
        |prototype| PlainDate::new(vm, iso_date, calendar, prototype),
    )?;

    // 6. Return object.
    Ok(object)
}

// 3.5.4 ToTemporalDate ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaldate
pub fn to_temporal_date(vm: &Vm, item: Value, options: Value) -> ThrowCompletionOr<Gc<PlainDate>> {
    // 1. If options is not present, set options to undefined.

    // 2. If item is an Object, then
    if item.is_object() {
        let object = item.as_object();

        // a. If item has an [[InitializedTemporalDate]] internal slot, then
        if let Some(plain_date) = object.downcast::<PlainDate>() {
            // i. Let resolvedOptions be ? GetOptionsObject(options).
            let resolved_options = get_options_object(vm, options)?;

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            get_temporal_overflow_option(vm, &resolved_options)?;

            // iii. Return ! CreateTemporalDate(item.[[ISODate]], item.[[Calendar]]).
            return Ok(create_temporal_date(vm, plain_date.iso_date(), plain_date.calendar(), None).must());
        }

        // b. If item has an [[InitializedTemporalZonedDateTime]] internal slot, then
        if let Some(zoned_date_time) = object.downcast::<ZonedDateTime>() {
            // i. Let isoDateTime be GetISODateTimeFor(item.[[TimeZone]], item.[[EpochNanoseconds]]).
            let time_zone = zoned_date_time.time_zone();
            let iso_date_time = get_iso_date_time_for(
                Utf16View::of_string(&time_zone),
                zoned_date_time.epoch_nanoseconds().big_integer(),
            );

            // ii. Let resolvedOptions be ? GetOptionsObject(options).
            let resolved_options = get_options_object(vm, options)?;

            // iii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            get_temporal_overflow_option(vm, &resolved_options)?;

            // iv. Return ! CreateTemporalDate(isoDateTime.[[ISODate]], item.[[Calendar]]).
            return Ok(create_temporal_date(vm, iso_date_time.iso_date, zoned_date_time.calendar(), None).must());
        }

        // c. If item has an [[InitializedTemporalDateTime]] internal slot, then
        if let Some(plain_date_time) = object.downcast::<PlainDateTime>() {
            // i. Let resolvedOptions be ? GetOptionsObject(options).
            let resolved_options = get_options_object(vm, options)?;

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            get_temporal_overflow_option(vm, &resolved_options)?;

            // iii. Return ! CreateTemporalDate(item.[[ISODateTime]].[[ISODate]], item.[[Calendar]]).
            return Ok(create_temporal_date(
                vm,
                plain_date_time.iso_date_time().iso_date,
                plain_date_time.calendar(),
                None,
            )
            .must());
        }

        // d. Let calendar be ? GetTemporalCalendarIdentifierWithISODefault(item).
        let calendar = get_temporal_calendar_identifier_with_iso_default(vm, &object)?;

        // e. Let fields be ? PrepareCalendarFields(calendar, item, « YEAR, MONTH, MONTH-CODE, DAY », «», «»).
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

        // f. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, options)?;

        // g. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

        // h. Let isoDate be ? CalendarDateFromFields(calendar, fields, overflow).
        let iso_date = calendar_date_from_fields(vm, Utf16View::of_string(&calendar), &mut fields, overflow)?;

        // i. Return ! CreateTemporalDate(isoDate, calendar).
        return Ok(create_temporal_date(vm, iso_date, calendar, None).must());
    }

    // 3. If item is not a String, throw a TypeError exception.
    if !item.is_string() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::TemporalInvalidPlainDate, &[]);
    }

    // 4. Let result be ? ParseISODateTime(item, « TemporalDateTimeString[~Zoned] »).
    let item_string = item.as_string().utf16_string();
    let result = parse_iso_date_time(
        vm,
        Utf16View::of_string(&item_string),
        &[Production::TemporalDateTimeString],
    )?;

    // 5. Let calendar be result.[[Calendar]].
    // 6. If calendar is empty, set calendar to "iso8601".
    let calendar = match &result.calendar {
        Some(calendar) => canonicalize_calendar(vm, Utf16View::of_string(calendar))?,
        None => canonicalize_calendar(vm, ascii_view(ISO8601_CALENDAR))?,
    };

    // 8. Let resolvedOptions be ? GetOptionsObject(options).
    let resolved_options = get_options_object(vm, options)?;

    // 9. Perform ? GetTemporalOverflowOption(resolvedOptions).
    get_temporal_overflow_option(vm, &resolved_options)?;

    // 10. Let isoDate be CreateISODateRecord(result.[[Year]], result.[[Month]], result.[[Day]]).
    let iso_date = create_iso_date_record(
        f64::from(result.year.expect("a date-time string has a year")),
        f64::from(result.month),
        f64::from(result.day),
    );

    // 11. Return ? CreateTemporalDate(isoDate, calendar).
    create_temporal_date(vm, iso_date, calendar, None)
}

/// The monthOrCode of CompareSurpasses: an ordinal month or a month code.
#[derive(Clone, Copy, Debug)]
pub enum MonthOrCode<'a> {
    Month(u8),
    Code(Utf16View<'a>),
}

// 3.5.5 CompareSurpasses ( sign, year, monthOrCode, day, target ), https://tc39.es/proposal-temporal/#sec-temporal-comparesurpasses
pub fn compare_surpasses(sign: i8, year: i32, month_or_code: MonthOrCode<'_>, day: u8, target: &CalendarDate) -> bool {
    let sign = i64::from(sign);
    let target_month_code = Utf16View::of_string(&target.month_code);

    // 1. If year ≠ target.[[Year]], then
    if year != target.year {
        // a. If sign × (year - target.[[Year]]) > 0, return true.
        if sign * (i64::from(year) - i64::from(target.year)) > 0 {
            return true;
        }
        return false;
    }

    match month_or_code {
        // 2. Else if monthOrCode is a month code and monthOrCode is not target.[[MonthCode]], then
        MonthOrCode::Code(month_code) if month_code != target_month_code => {
            // a. If sign > 0, then
            if sign > 0 {
                // i. If monthOrCode is lexicographically greater than target.[[MonthCode]], return true.
                if target_month_code.is_code_unit_less_than(month_code) {
                    return true;
                }
            }
            // b. Else,
            else {
                // i. If target.[[MonthCode]] is lexicographically greater than monthOrCode, return true.
                if month_code.is_code_unit_less_than(target_month_code) {
                    return true;
                }
            }
            return false;
        }
        // 3. Else if monthOrCode is an integer and monthOrCode ≠ target.[[Month]], then
        MonthOrCode::Month(month) if month != target.month => {
            // a. If sign × (monthOrCode - target.[[Month]]) > 0, return true.
            if sign * (i64::from(month) - i64::from(target.month)) > 0 {
                return true;
            }
            return false;
        }
        _ => {}
    }

    // 4. Else if day ≠ target.[[Day]], then
    if day != target.day {
        // a. If sign × (day - target.[[Day]]) > 0, return true.
        if sign * (i64::from(day) - i64::from(target.day)) > 0 {
            return true;
        }
    }

    // 5. Return false.
    false
}

// 3.5.5 ISODateSurpasses ( sign, baseDate, isoDate2, years, months, weeks, days ), https://tc39.es/proposal-temporal/#sec-temporal-isodatesurpasses
#[allow(clippy::too_many_arguments)]
pub fn iso_date_surpasses(
    vm: &Vm,
    sign: i8,
    base_date: ISODate,
    iso_date2: ISODate,
    years: f64,
    months: f64,
    weeks: f64,
    days: f64,
) -> bool {
    // 1. Let parts be CalendarISOToDate("iso8601", baseDate).
    let parts = calendar_iso_to_date(iso8601_calendar_view(), base_date);

    // 2. Let target be CalendarISOToDate("iso8601", isoDate2).
    let target = calendar_iso_to_date(iso8601_calendar_view(), iso_date2);

    // 3. Let y0 be parts.[[Year]] + years.
    let year0 = f64::from(parts.year) + years;

    // 4. If CompareSurpasses(sign, y0, parts.[[MonthCode]], parts.[[Day]], target) is true, return true.
    if compare_surpasses(
        sign,
        year0 as i32,
        MonthOrCode::Code(Utf16View::of_string(&parts.month_code)),
        parts.day,
        &target,
    ) {
        return true;
    }

    // 5. If months = 0, return false.
    if months == 0.0 {
        return false;
    }

    // 6. Let m0 be parts.[[Month]] + months.
    let month0 = f64::from(parts.month) + months;

    // 7. Let monthsAdded be BalanceISOYearMonth(y0, m0).
    let months_added = balance_iso_year_month(year0, month0);

    // 8. If CompareSurpasses(sign, monthsAdded.[[Year]], monthsAdded.[[Month]], parts.[[Day]], target) is true, return true.
    if compare_surpasses(
        sign,
        months_added.year,
        MonthOrCode::Month(months_added.month),
        parts.day,
        &target,
    ) {
        return true;
    }

    // 9. If weeks = 0 and days = 0, return false.
    if weeks == 0.0 && days == 0.0 {
        return false;
    }

    // 10. Let regulatedDate be ! RegulateISODate(monthsAdded.[[Year]], monthsAdded.[[Month]], parts.[[Day]], CONSTRAIN).
    let regulated_date = regulate_iso_date(
        vm,
        f64::from(months_added.year),
        f64::from(months_added.month),
        f64::from(parts.day),
        Overflow::Constrain,
    )
    .must();

    // 11. Let daysInWeek be 7.
    const DAYS_IN_WEEK: f64 = 7.0;

    // 12. Let balancedDate be AddDaysToISODate(regulatedDate, daysInWeek * weeks + days).
    let balanced_date = add_days_to_iso_date(regulated_date, (DAYS_IN_WEEK * weeks) + days);

    // 13. Return CompareSurpasses(sign, balancedDate.[[Year]], balancedDate.[[Month]], balancedDate.[[Day]], target).
    compare_surpasses(
        sign,
        balanced_date.year,
        MonthOrCode::Month(balanced_date.month),
        balanced_date.day,
        &target,
    )
}

/// AK::clamp(), which keeps a value that is neither below the minimum nor above the maximum as it is.
fn clamp(value: f64, minimum: f64, maximum: f64) -> f64 {
    if value < minimum {
        return minimum;
    }
    if value > maximum {
        return maximum;
    }
    value
}

// 3.5.6 RegulateISODate ( year, month, day, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-regulateisodate
pub fn regulate_iso_date(
    vm: &Vm,
    mut year: f64,
    mut month: f64,
    mut day: f64,
    overflow: Overflow,
) -> ThrowCompletionOr<ISODate> {
    match overflow {
        // 1. If overflow is CONSTRAIN, then
        Overflow::Constrain => {
            // a. Set month to the result of clamping month between 1 and 12.
            month = clamp(month, 1.0, 12.0);

            // b. Let daysInMonth be ISODaysInMonth(year, month).
            // c. Set day to the result of clamping day between 1 and daysInMonth.
            day = clamp(day, 1.0, f64::from(iso_days_in_month(year, month)));

            // AD-HOC: We further clamp the year to the range allowed by ISODate.year, to ensure we do not overflow when we
            //         store the year as an integer.
            year = clamp(year, f64::from(i32::MIN), f64::from(i32::MAX));
        }

        // 2. Else,
        Overflow::Reject => {
            // a. Assert: overflow is REJECT.
            // b. If IsValidISODate(year, month, day) is false, throw a RangeError exception.
            if !is_valid_iso_date(year, month, day) {
                return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidISODate, &[]);
            }
        }
    }

    // 3. Return CreateISODateRecord(year, month, day).
    Ok(create_iso_date_record(year, month, day))
}

// 3.5.7 IsValidISODate ( year, month, day ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidisodate
pub fn is_valid_iso_date(year: f64, month: f64, day: f64) -> bool {
    // AD-HOC: This is an optimization that allows us to treat these doubles as normal integers from this point onwards.
    //         This does not change the exposed behavior as the call to CreateISODateRecord will immediately check that
    //         these values are valid ISO values (years: [-271821, 275760], months: [1, 12], days: [1, 31]), all of
    //         which are subsets of this check.
    if !is_within_i32_range(year) || !is_within_u8_range(month) || !is_within_u8_range(day) {
        return false;
    }

    // 1. If month < 1 or month > 12, return false.
    if !(1.0..=12.0).contains(&month) {
        return false;
    }

    // 2. Let daysInMonth be ISODaysInMonth(year, month).
    let days_in_month = iso_days_in_month(year, month);

    // 3. If day < 1 or day > daysInMonth, return false; else return true.
    day >= 1.0 && day <= f64::from(days_in_month)
}

// 3.5.8 AddDaysToISODate ( isoDate, days ), https://tc39.es/proposal-temporal/#sec-temporal-adddaystoisodate
pub fn add_days_to_iso_date(iso_date: ISODate, days: f64) -> ISODate {
    // 1. Let epochDays be ISODateToEpochDays(isoDate.[[Year]], isoDate.[[Month]] - 1, isoDate.[[Day]]) + days.
    let epoch_days = iso_date_to_epoch_days(
        f64::from(iso_date.year),
        f64::from(iso_date.month) - 1.0,
        f64::from(iso_date.day),
    ) + days;

    // 2. Let ms be EpochDaysToEpochMs(epochDays, 0).
    let ms = epoch_days_to_epoch_ms(epoch_days, 0.0);

    // 3. Return CreateISODateRecord(EpochTimeToEpochYear(ms), EpochTimeToMonthInYear(ms) + 1, EpochTimeToDate(ms)).
    create_iso_date_record(
        f64::from(epoch_time_to_epoch_year(ms)),
        f64::from(epoch_time_to_month_in_year(ms)) + 1.0,
        f64::from(epoch_time_to_date(ms)),
    )
}

// 3.5.9 PadISOYear ( y ), https://tc39.es/proposal-temporal/#sec-temporal-padisoyear
pub fn pad_iso_year(year: i32) -> String {
    // 1. If y ≥ 0 and y ≤ 9999, return ToZeroPaddedDecimalString(y, 4).
    if (0..=9999).contains(&year) {
        return format!("{year:04}");
    }

    // 2. If y > 0, let yearSign be "+"; else, let yearSign be "-".
    let year_sign = if year > 0 { '+' } else { '-' };

    // 3. Let year be ToZeroPaddedDecimalString(abs(y), 6).
    // 4. Return the string-concatenation of yearSign and year.
    format!("{year_sign}{:06}", year.unsigned_abs())
}

// 3.5.10 TemporalDateToString ( temporalDate, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-temporaldatetostring
pub fn temporal_date_to_string(temporal_date: &PlainDate, show_calendar: ShowCalendar) -> String {
    // 1. Let year be PadISOYear(temporalDate.[[ISODate]].[[Year]]).
    let year = pad_iso_year(temporal_date.iso_date().year);

    // 2. Let month be ToZeroPaddedDecimalString(temporalDate.[[ISODate]].[[Month]], 2).
    let month = temporal_date.iso_date().month;

    // 3. Let day be ToZeroPaddedDecimalString(temporalDate.[[ISODate]].[[Day]], 2).
    let day = temporal_date.iso_date().day;

    // 4. Let calendar be FormatCalendarAnnotation(temporalDate.[[Calendar]], showCalendar).
    let calendar = format_calendar_annotation(Utf16View::of_string(&temporal_date.calendar()), show_calendar);

    // 5. Return the string-concatenation of year, the code unit 0x002D (HYPHEN-MINUS), month, the code unit 0x002D (HYPHEN-MINUS), day, and calendar.
    format!("{year}-{month:02}-{day:02}{calendar}")
}

// 3.5.11 ISODateWithinLimits ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isodatewithinlimits
pub fn iso_date_within_limits(iso_date: ISODate) -> bool {
    // 1. Let isoDateTime be CombineISODateAndTimeRecord(isoDate, NoonTimeRecord()).
    let iso_date_time = combine_iso_date_and_time_record(iso_date, noon_time_record());

    // 2. Return ISODateTimeWithinLimits(isoDateTime).
    iso_date_time_within_limits(&iso_date_time)
}

// 3.5.12 CompareISODate ( isoDate1, isoDate2 ), https://tc39.es/proposal-temporal/#sec-temporal-compareisodate
pub fn compare_iso_date(iso_date1: ISODate, iso_date2: ISODate) -> i8 {
    // 1. If isoDate1.[[Year]] > isoDate2.[[Year]], return 1.
    if iso_date1.year > iso_date2.year {
        return 1;
    }

    // 2. If isoDate1.[[Year]] < isoDate2.[[Year]], return -1.
    if iso_date1.year < iso_date2.year {
        return -1;
    }

    // 3. If isoDate1.[[Month]] > isoDate2.[[Month]], return 1.
    if iso_date1.month > iso_date2.month {
        return 1;
    }

    // 4. If isoDate1.[[Month]] < isoDate2.[[Month]], return -1.
    if iso_date1.month < iso_date2.month {
        return -1;
    }

    // 5. If isoDate1.[[Day]] > isoDate2.[[Day]], return 1.
    if iso_date1.day > iso_date2.day {
        return 1;
    }

    // 6. If isoDate1.[[Day]] < isoDate2.[[Day]], return -1.
    if iso_date1.day < iso_date2.day {
        return -1;
    }

    // 7. Return 0.
    0
}

// 3.5.13 DifferenceTemporalPlainDate ( operation, temporalDate, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalplaindate
pub fn difference_temporal_plain_date(
    vm: &Vm,
    operation: DurationOperation,
    temporal_date: &PlainDate,
    other_value: Value,
    options: Value,
) -> ThrowCompletionOr<Gc<Duration>> {
    let calendar_string = temporal_date.calendar();
    let calendar = Utf16View::of_string(&calendar_string);

    // 1. Set other to ? ToTemporalDate(other).
    let other = to_temporal_date(vm, other_value, Value::UNDEFINED)?;

    // 2. If CalendarEquals(temporalDate.[[Calendar]], other.[[Calendar]]) is false, throw a RangeError exception.
    if !calendar_equals(calendar, Utf16View::of_string(&other.calendar())) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalDifferentCalendars, &[]);
    }

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    let resolved_options = get_options_object(vm, options)?;

    // 4. Let settings be ? GetDifferenceSettings(operation, resolvedOptions, DATE, « », DAY, DAY).
    let settings = get_difference_settings(
        vm,
        operation,
        &resolved_options,
        UnitGroup::Date,
        &[],
        Unit::Day,
        Unit::Day,
    )?;

    // 5. If CompareISODate(temporalDate.[[ISODate]], other.[[ISODate]]) = 0, then
    if compare_iso_date(temporal_date.iso_date(), other.iso_date()) == 0 {
        // a. Return ! CreateTemporalDuration(0, 0, 0, 0, 0, 0, 0, 0, 0, 0).
        return Ok(create_temporal_duration(vm, DurationFields::default(), None).must());
    }

    // 6. Let dateDifference be CalendarDateUntil(temporalDate.[[Calendar]], temporalDate.[[ISODate]], other.[[ISODate]], settings.[[LargestUnit]]).
    let date_difference = calendar_date_until(
        vm,
        calendar,
        temporal_date.iso_date(),
        other.iso_date(),
        settings.largest_unit,
    );

    // 7. Let duration be CombineDateAndTimeDuration(dateDifference, 0).
    let mut duration = combine_date_and_time_duration(date_difference, TimeDuration::default());

    // 8. If settings.[[SmallestUnit]] is not DAY or settings.[[RoundingIncrement]] ≠ 1, then
    if settings.smallest_unit != Unit::Day || settings.rounding_increment != 1 {
        // a. Let isoDateTime be CombineISODateAndTimeRecord(temporalDate.[[ISODate]], MidnightTimeRecord()).
        let iso_date_time = combine_iso_date_and_time_record(temporal_date.iso_date(), midnight_time_record());

        // b. Let originEpochNs be GetUTCEpochNanoseconds(isoDateTime).
        let origin_epoch_ns = get_utc_epoch_nanoseconds(&iso_date_time);

        // c. Let isoDateTimeOther be CombineISODateAndTimeRecord(other.[[ISODate]], MidnightTimeRecord()).
        let iso_date_time_other = combine_iso_date_and_time_record(other.iso_date(), midnight_time_record());

        // d. Let destEpochNs be GetUTCEpochNanoseconds(isoDateTimeOther).
        let dest_epoch_ns = get_utc_epoch_nanoseconds(&iso_date_time_other);

        // e. Set duration to ? RoundRelativeDuration(duration, originEpochNs, destEpochNs, isoDateTime, UNSET, temporalDate.[[Calendar]], settings.[[LargestUnit]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]]).
        duration = round_relative_duration(
            vm,
            duration,
            &origin_epoch_ns,
            &dest_epoch_ns,
            &iso_date_time,
            None,
            calendar,
            settings.largest_unit,
            settings.rounding_increment,
            settings.smallest_unit,
            settings.rounding_mode,
        )?;
    }

    // 9. Let result be ! TemporalDurationFromInternal(duration, DAY).
    let mut result = temporal_duration_from_internal(vm, &duration, Unit::Day).must();

    // 10. If operation is since, set result to CreateNegatedTemporalDuration(result).
    if operation == DurationOperation::Since {
        result = create_negated_temporal_duration(vm, &result);
    }

    // 11. Return result.
    Ok(result)
}

// 3.5.14 AddDurationToDate ( operation, temporalDate, temporalDurationLike, options ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtodate
pub fn add_duration_to_date(
    vm: &Vm,
    operation: ArithmeticOperation,
    temporal_date: &PlainDate,
    temporal_duration_like: Value,
    options: Value,
) -> ThrowCompletionOr<Gc<PlainDate>> {
    // 1. Let calendar be temporalDate.[[Calendar]].
    let calendar = temporal_date.calendar();

    // 2. Let duration be ? ToTemporalDuration(temporalDurationLike).
    let mut duration = to_temporal_duration(vm, temporal_duration_like)?;

    // 3. If operation is SUBTRACT, set duration to CreateNegatedTemporalDuration(duration).
    if operation == ArithmeticOperation::Subtract {
        duration = create_negated_temporal_duration(vm, &duration);
    }

    // 4. Let dateDuration be ToDateDurationRecordWithoutTime(duration).
    let date_duration = to_date_duration_record_without_time(vm, &duration);

    // 5. Let resolvedOptions be ? GetOptionsObject(options).
    let resolved_options = get_options_object(vm, options)?;

    // 6. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
    let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

    // 7. Let result be ? CalendarDateAdd(calendar, temporalDate.[[ISODate]], dateDuration, overflow).
    let result = calendar_date_add(
        vm,
        Utf16View::of_string(&calendar),
        temporal_date.iso_date(),
        &date_duration,
        overflow,
    )?;

    // 8. Return ! CreateTemporalDate(result, calendar).
    Ok(create_temporal_date(vm, result, calendar, None).must())
}
