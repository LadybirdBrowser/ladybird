/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Temporal.PlainYearMonth objects and the ISO Year-Month Record operations.

use ak::Utf16String;
use libjs_runtime_macros::Trace;
use num_traits::Zero;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::function_object::FunctionObject;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{get_options_object, modulo, ordinary_create_from_constructor_of};
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::date::get_utc_epoch_nanoseconds;
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::temporal::abstract_operations::{
    ArithmeticOperation, DateType, DurationOperation, Overflow, ShowCalendar, Unit, UnitGroup, ascii_view,
    get_difference_settings, get_temporal_overflow_option, iso_date_to_fields, parse_iso_date_time,
};
use crate::runtime::temporal::calendar::{
    CalendarField, CalendarFieldListOrPartial, ISO8601_CALENDAR, calendar_date_add, calendar_date_from_fields,
    calendar_date_until, calendar_equals, calendar_year_month_from_fields, canonicalize_calendar,
    format_calendar_annotation, get_temporal_calendar_identifier_with_iso_default, prepare_calendar_fields,
};
use crate::runtime::temporal::duration::{
    Duration, DurationFields, adjust_date_duration_record, combine_date_and_time_duration,
    create_negated_temporal_duration, create_temporal_duration, round_relative_duration,
    temporal_duration_from_internal, to_internal_duration_record, to_temporal_duration,
};
use crate::runtime::temporal::iso_records::{ISODate, ISOYearMonth, TimeDuration};
use crate::runtime::temporal::iso8601::Production;
use crate::runtime::temporal::plain_date::{compare_iso_date, create_iso_date_record, pad_iso_year};
use crate::runtime::temporal::plain_date_time::combine_iso_date_and_time_record;
use crate::runtime::temporal::plain_time::midnight_time_record;
use crate::utf16::Utf16View;

// 9 Temporal.PlainYearMonth Objects, https://tc39.es/proposal-temporal/#sec-temporal-plainyearmonth-objects
#[repr(C)]
#[derive(Trace)]
pub struct PlainYearMonth {
    base: Object,
    #[gc(untraced)]
    iso_date: ISODate, // [[ISODate]]
    calendar: Utf16String, // [[Calendar]]
}

define_cell!(PlainYearMonth, Object, extends: [Object]);

impl core::ops::Deref for PlainYearMonth {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl PlainYearMonth {
    fn new(vm: &Vm, iso_date: ISODate, calendar: Utf16String, prototype: Gc<Object>) -> PlainYearMonth {
        PlainYearMonth {
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

// 9.5.2 ToTemporalYearMonth ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalyearmonth
pub fn to_temporal_year_month(vm: &Vm, item: Value, options: Value) -> ThrowCompletionOr<Gc<PlainYearMonth>> {
    // 1. If options is not present, set options to undefined.

    // 2. If item is an Object, then
    if item.is_object() {
        let object = item.as_object();

        // a. If item has an [[InitializedTemporalYearMonth]] internal slot, then
        if let Some(plain_year_month) = object.downcast::<PlainYearMonth>() {
            // i. Let resolvedOptions be ? GetOptionsObject(options).
            let resolved_options = get_options_object(vm, options)?;

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            get_temporal_overflow_option(vm, &resolved_options)?;

            // iii. Return ! CreateTemporalYearMonth(item.[[ISODate]], item.[[Calendar]]).
            return Ok(
                create_temporal_year_month(vm, plain_year_month.iso_date(), plain_year_month.calendar(), None).must(),
            );
        }

        // b. Let calendar be ? GetTemporalCalendarIdentifierWithISODefault(item).
        let calendar = get_temporal_calendar_identifier_with_iso_default(vm, &object)?;

        // c. Let fields be ? PrepareCalendarFields(calendar, item, « YEAR, MONTH, MONTH-CODE », «», «»).
        let mut fields = prepare_calendar_fields(
            vm,
            Utf16View::of_string(&calendar),
            &object,
            &[CalendarField::Year, CalendarField::Month, CalendarField::MonthCode],
            &[],
            CalendarFieldListOrPartial::List(&[]),
        )?;

        // d. Let resolvedOptions be ? GetOptionsObject(options).
        let resolved_options = get_options_object(vm, options)?;

        // e. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

        // f. Let isoDate be ? CalendarYearMonthFromFields(calendar, fields, overflow).
        let iso_date = calendar_year_month_from_fields(vm, Utf16View::of_string(&calendar), &mut fields, overflow)?;

        // g. Return ! CreateTemporalYearMonth(isoDate, calendar).
        return Ok(create_temporal_year_month(vm, iso_date, calendar, None).must());
    }

    // 3. If item is not a String, throw a TypeError exception.
    if !item.is_string() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::TemporalInvalidPlainYearMonth, &[]);
    }

    // 4. Let result be ? ParseISODateTime(item, « TemporalYearMonthString »).
    let item_string = item.as_string().utf16_string();
    let parse_result = parse_iso_date_time(
        vm,
        Utf16View::of_string(&item_string),
        &[Production::TemporalYearMonthString],
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

    // 10. Let isoDate be CreateISODateRecord(result.[[Year]], result.[[Month]], result.[[Day]]).
    let iso_date = create_iso_date_record(
        f64::from(parse_result.year.expect("a year-month string has a year")),
        f64::from(parse_result.month),
        f64::from(parse_result.day),
    );

    // 11. If ISOYearMonthWithinLimits(isoDate) is false, throw a RangeError exception.
    if !iso_year_month_within_limits(iso_date) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainYearMonth, &[]);
    }

    // 12. Set result to ISODateToFields(calendar, isoDate, YEAR-MONTH).
    let mut result = iso_date_to_fields(Utf16View::of_string(&calendar), iso_date, DateType::YearMonth);

    // 13. NOTE: The following operation is called with CONSTRAIN regardless of overflow, in order for the calendar to
    //     store a canonical value in the [[Day]] field of the [[ISODate]] internal slot of the result.
    // 14. Set isoDate to ? CalendarYearMonthFromFields(calendar, result, CONSTRAIN).
    let iso_date =
        calendar_year_month_from_fields(vm, Utf16View::of_string(&calendar), &mut result, Overflow::Constrain)?;

    // 15. Return ! CreateTemporalYearMonth(isoDate, calendar).
    Ok(create_temporal_year_month(vm, iso_date, calendar, None).must())
}

// 9.5.3 ISOYearMonthWithinLimits ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isoyearmonthwithinlimits
pub fn iso_year_month_within_limits(iso_date: ISODate) -> bool {
    // 1. If isoDate.[[Year]] < -271821 or isoDate.[[Year]] > 275760, return false.
    if iso_date.year < -271_821 || iso_date.year > 275_760 {
        return false;
    }

    // 2. If isoDate.[[Year]] = -271821 and isoDate.[[Month]] < 4, return false.
    if iso_date.year == -271_821 && iso_date.month < 4 {
        return false;
    }

    // 3. If isoDate.[[Year]] = 275760 and isoDate.[[Month]] > 9, return false.
    if iso_date.year == 275_760 && iso_date.month > 9 {
        return false;
    }

    // 4. Return true.
    true
}

// 9.5.4 BalanceISOYearMonth ( year, month ), https://tc39.es/proposal-temporal/#sec-temporal-balanceisoyearmonth
pub fn balance_iso_year_month(mut year: f64, mut month: f64) -> ISOYearMonth {
    // 1. Set year to year + floor((month - 1) / 12).
    year += ((month - 1.0) / 12.0).floor();

    // 2. Set month to ((month - 1) modulo 12) + 1.
    month = modulo(month - 1.0, 12.0) + 1.0;

    // 3. Return ISO Year-Month Record { [[Year]]: year, [[Month]]: month  }.
    ISOYearMonth {
        year: year as i32,
        month: month as u8,
    }
}

// 9.5.5 CreateTemporalYearMonth ( isoDate, calendar [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporalyearmonth
pub fn create_temporal_year_month(
    vm: &Vm,
    iso_date: ISODate,
    calendar: Utf16String,
    new_target: Option<Gc<FunctionObject>>,
) -> ThrowCompletionOr<Gc<PlainYearMonth>> {
    let realm = vm.current_realm().expect("CreateTemporalYearMonth runs in a realm");

    // 1. If ISOYearMonthWithinLimits(isoDate) is false, throw a RangeError exception.
    if !iso_year_month_within_limits(iso_date) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalInvalidPlainYearMonth, &[]);
    }

    // 2. If newTarget is not present, set newTarget to %Temporal.PlainYearMonth%.
    let new_target = new_target.unwrap_or_else(|| realm.intrinsics().temporal_plain_year_month_constructor(vm));

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainYearMonth.prototype%", « [[InitializedTemporalYearMonth]], [[ISODate]], [[Calendar]] »).
    // 4. Set object.[[ISODate]] to isoDate.
    // 5. Set object.[[Calendar]] to calendar.
    let object = ordinary_create_from_constructor_of(
        vm,
        realm,
        new_target,
        Intrinsics::temporal_plain_year_month_prototype,
        |prototype| PlainYearMonth::new(vm, iso_date, calendar, prototype),
    )?;

    // 6. Return object.
    Ok(object)
}

// 9.5.6 TemporalYearMonthToString ( yearMonth, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-temporalyearmonthtostring
pub fn temporal_year_month_to_string(year_month: &PlainYearMonth, show_calendar: ShowCalendar) -> String {
    let iso_date = year_month.iso_date();
    let calendar = year_month.calendar();

    // 1. Let year be PadISOYear(yearMonth.[[ISODate]].[[Year]]).
    let year = pad_iso_year(iso_date.year);

    // 2. Let month be ToZeroPaddedDecimalString(yearMonth.[[ISODate]].[[Month]], 2).
    // 3. Let result be the string-concatenation of year, the code unit 0x002D (HYPHEN-MINUS), and month.
    let mut result = format!("{year}-{:02}", iso_date.month);

    // 4. If showCalendar is one of always or critical, or yearMonth.[[Calendar]] is not "iso8601", then
    if show_calendar == ShowCalendar::Always
        || show_calendar == ShowCalendar::Critical
        || Utf16View::of_string(&calendar) != ISO8601_CALENDAR
    {
        // a. Let day be ToZeroPaddedDecimalString(yearMonth.[[ISODate]].[[Day]], 2).
        // b. Set result to the string-concatenation of result, the code unit 0x002D (HYPHEN-MINUS), and day.
        result = format!("{result}-{:02}", iso_date.day);
    }

    // 5. Let calendarString be FormatCalendarAnnotation(yearMonth.[[Calendar]], showCalendar).
    let calendar_string = format_calendar_annotation(Utf16View::of_string(&calendar), show_calendar);

    // 6. Set result to the string-concatenation of result and calendarString.
    result.push_str(&calendar_string);

    // 7. Return result.
    result
}

// 9.5.7 DifferenceTemporalPlainYearMonth ( operation, yearMonth, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalplainyearmonth
pub fn difference_temporal_plain_year_month(
    vm: &Vm,
    operation: DurationOperation,
    year_month: &PlainYearMonth,
    other_value: Value,
    options: Value,
) -> ThrowCompletionOr<Gc<Duration>> {
    // 1. Set other to ? ToTemporalYearMonth(other).
    let other = to_temporal_year_month(vm, other_value, Value::UNDEFINED)?;

    // 2. Let calendar be yearMonth.[[Calendar]].
    let calendar_string = year_month.calendar();
    let calendar = Utf16View::of_string(&calendar_string);

    // 3. If CalendarEquals(calendar, other.[[Calendar]]) is false, throw a RangeError exception.
    if !calendar_equals(calendar, Utf16View::of_string(&other.calendar())) {
        return vm.throw_completion(ErrorKind::RangeError, ErrorType::TemporalDifferentCalendars, &[]);
    }

    // 4. Let resolvedOptions be ? GetOptionsObject(options).
    let resolved_options = get_options_object(vm, options)?;

    // 5. Let settings be ? GetDifferenceSettings(operation, resolvedOptions, DATE, « WEEK, DAY », MONTH, YEAR).
    let settings = get_difference_settings(
        vm,
        operation,
        &resolved_options,
        UnitGroup::Date,
        &[Unit::Week, Unit::Day],
        Unit::Month,
        Unit::Year,
    )?;

    // 6. If CompareISODate(yearMonth.[[ISODate]], other.[[ISODate]]) = 0, return ! CreateTemporalDuration(0, 0, 0, 0, 0, 0, 0, 0, 0, 0).
    if compare_iso_date(year_month.iso_date(), other.iso_date()) == 0 {
        return Ok(create_temporal_duration(vm, DurationFields::default(), None).must());
    }

    // 7. Let thisFields be ISODateToFields(calendar, yearMonth.[[ISODate]], YEAR-MONTH).
    let mut this_fields = iso_date_to_fields(calendar, year_month.iso_date(), DateType::YearMonth);

    // 8. Set thisFields.[[Day]] to 1.
    this_fields.day = Some(1);

    // 9. Let thisDate be ? CalendarDateFromFields(calendar, thisFields, CONSTRAIN).
    let this_date = calendar_date_from_fields(vm, calendar, &mut this_fields, Overflow::Constrain)?;

    // 10. Let otherFields be ISODateToFields(calendar, other.[[ISODate]], YEAR-MONTH).
    let mut other_fields = iso_date_to_fields(calendar, other.iso_date(), DateType::YearMonth);

    // 11. Set otherFields.[[Day]] to 1.
    other_fields.day = Some(1);

    // 12. Let otherDate be ? CalendarDateFromFields(calendar, otherFields, CONSTRAIN).
    let other_date = calendar_date_from_fields(vm, calendar, &mut other_fields, Overflow::Constrain)?;

    // 13. Let dateDifference be CalendarDateUntil(calendar, thisDate, otherDate, settings.[[LargestUnit]]).
    let date_difference = calendar_date_until(vm, calendar, this_date, other_date, settings.largest_unit);

    // 14. Let yearsMonthsDifference be ! AdjustDateDurationRecord(dateDifference, 0, 0).
    let years_months_difference = adjust_date_duration_record(vm, &date_difference, 0.0, Some(0.0), None).must();

    // 15. Let duration be CombineDateAndTimeDuration(yearsMonthsDifference, 0).
    let mut duration = combine_date_and_time_duration(years_months_difference, TimeDuration::default());

    // 16. If settings.[[SmallestUnit]] is not MONTH or settings.[[RoundingIncrement]] ≠ 1, then
    if settings.smallest_unit != Unit::Month || settings.rounding_increment != 1 {
        // a. Let isoDateTime be CombineISODateAndTimeRecord(thisDate, MidnightTimeRecord()).
        let iso_date_time = combine_iso_date_and_time_record(this_date, midnight_time_record());

        // b. Let originEpochNs be GetUTCEpochNanoseconds(isoDateTime).
        let origin_epoch_ns = get_utc_epoch_nanoseconds(&iso_date_time);

        // c. Let isoDateTimeOther be CombineISODateAndTimeRecord(otherDate, MidnightTimeRecord()).
        let iso_date_time_other = combine_iso_date_and_time_record(other_date, midnight_time_record());

        // d. Let destEpochNs be GetUTCEpochNanoseconds(isoDateTimeOther).
        let dest_epoch_ns = get_utc_epoch_nanoseconds(&iso_date_time_other);

        // e. Set duration to ? RoundRelativeDuration(duration, originEpochNs, destEpochNs, isoDateTime, UNSET, calendar, settings.[[LargestUnit]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]]).
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

    // 17. Let result be ! TemporalDurationFromInternal(duration, DAY).
    let mut result = temporal_duration_from_internal(vm, &duration, Unit::Day).must();

    // 18. If operation is SINCE, set result to CreateNegatedTemporalDuration(result).
    if operation == DurationOperation::Since {
        result = create_negated_temporal_duration(vm, &result);
    }

    // 19. Return result.
    Ok(result)
}

// 9.5.8 AddDurationToYearMonth ( operation, yearMonth, temporalDurationLike, options ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtoyearmonth
pub fn add_duration_to_year_month(
    vm: &Vm,
    operation: ArithmeticOperation,
    year_month: &PlainYearMonth,
    temporal_duration_like: Value,
    options: Value,
) -> ThrowCompletionOr<Gc<PlainYearMonth>> {
    // 1. Let duration be ? ToTemporalDuration(temporalDurationLike).
    let mut duration = to_temporal_duration(vm, temporal_duration_like)?;

    // 2. If operation is SUBTRACT, set duration to CreateNegatedTemporalDuration(duration).
    if operation == ArithmeticOperation::Subtract {
        duration = create_negated_temporal_duration(vm, &duration);
    }

    // 3. Let internalDuration be ToInternalDurationRecord(duration).
    let internal_duration = to_internal_duration_record(vm, &duration);

    // 4. Let resolvedOptions be ? GetOptionsObject(options).
    let resolved_options = get_options_object(vm, options)?;

    // 5. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
    let overflow = get_temporal_overflow_option(vm, &resolved_options)?;

    // 6. Let durationToAdd be internalDuration.[[Date]].
    let duration_to_add = &internal_duration.date;

    // 7. If durationToAdd.[[Weeks]] ≠ 0, or durationToAdd.[[Days]] ≠ 0, or internalDuration.[[Time]] ≠ 0, throw a RangeError exception.
    if duration_to_add.weeks != 0.0 || duration_to_add.days != 0.0 || !internal_duration.time.is_zero() {
        let operation_string = if operation == ArithmeticOperation::Add {
            "added to"
        } else {
            "subtracted from"
        };
        return vm.throw_completion(
            ErrorKind::RangeError,
            ErrorType::TemporalInvalidPlainYearMonthAddition,
            &[&operation_string],
        );
    }

    // 8. Let calendar be yearMonth.[[Calendar]].
    let calendar_string = year_month.calendar();
    let calendar = Utf16View::of_string(&calendar_string);

    // 9. Let fields be ISODateToFields(calendar, yearMonth.[[ISODate]], YEAR-MONTH).
    let mut fields = iso_date_to_fields(calendar, year_month.iso_date(), DateType::YearMonth);

    // 10. Set fields.[[Day]] to 1.
    fields.day = Some(1);

    // 11. Let date be ? CalendarDateFromFields(calendar, fields, CONSTRAIN).
    let date = calendar_date_from_fields(vm, calendar, &mut fields, Overflow::Constrain)?;

    // 12. Let addedDate be ? CalendarDateAdd(calendar, date, durationToAdd, overflow).
    let added_date = calendar_date_add(vm, calendar, date, duration_to_add, overflow)?;

    // 13. Let addedDateFields be ISODateToFields(calendar, addedDate, YEAR-MONTH).
    let mut added_date_fields = iso_date_to_fields(calendar, added_date, DateType::YearMonth);

    // 14. Let isoDate be ? CalendarYearMonthFromFields(calendar, addedDateFields, overflow).
    let iso_date = calendar_year_month_from_fields(vm, calendar, &mut added_date_fields, overflow)?;

    // 15. Return ! CreateTemporalYearMonth(isoDate, calendar).
    Ok(create_temporal_year_month(vm, iso_date, calendar_string, None).must())
}
