/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>
#include <LibJS/Runtime/Temporal/PlainYearMonthConstructor.h>
#include <LibJS/Runtime/VM.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainYearMonth);

// 9 Temporal.PlainYearMonth Objects, https://tc39.es/proposal-temporal/#sec-temporal-plainyearmonth-objects
PlainYearMonth::PlainYearMonth(ISODate iso_date, String calendar, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_iso_date(iso_date)
    , m_calendar(move(calendar))
{
}

// 9.5.2 ToTemporalYearMonth ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalyearmonth
ThrowCompletionOr<GC::Ref<PlainYearMonth>> to_temporal_year_month(VM& vm, Value item, Value options)
{
    // 1. If options is not present, set options to undefined.

    // 2. If item is an Object, then
    if (item.is_object()) {
        auto const& object = item.as_object();

        // a. If item has an [[InitializedTemporalYearMonth]] internal slot, then
        if (is<PlainYearMonth>(object)) {
            auto const& plain_year_month = static_cast<PlainYearMonth const&>(object);

            // i. Let resolvedOptions be ? GetOptionsObject(options).
            auto resolved_options = TRY(get_options_object(vm, options));

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            TRY(get_temporal_overflow_option(vm, resolved_options));

            // iii. Return ! CreateTemporalYearMonth(item.[[ISODate]], item.[[Calendar]]).
            return MUST(create_temporal_year_month(vm, plain_year_month.iso_date(), plain_year_month.calendar()));
        }

        // b. Let calendar be ? GetTemporalCalendarIdentifierWithISODefault(item).
        auto calendar = TRY(get_temporal_calendar_identifier_with_iso_default(vm, object));

        // c. Let fields be ? PrepareCalendarFields(calendar, item, « YEAR, MONTH, MONTH-CODE », «», «»).
        auto fields = TRY(prepare_calendar_fields(vm, calendar, object, { { CalendarField::Year, CalendarField::Month, CalendarField::MonthCode } }, {}, CalendarFieldList {}));

        // d. Let resolvedOptions be ? GetOptionsObject(options).
        auto resolved_options = TRY(get_options_object(vm, options));

        // e. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

        // f. Let isoDate be ? CalendarYearMonthFromFields(calendar, fields, overflow).
        auto iso_date = TRY(calendar_year_month_from_fields(vm, calendar, fields, overflow));

        // g. Return ! CreateTemporalYearMonth(isoDate, calendar).
        return MUST(create_temporal_year_month(vm, iso_date, move(calendar)));
    }

    // 3. If item is not a String, throw a TypeError exception.
    if (!item.is_string())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidPlainYearMonth);

    // 4. Let result be ? ParseISODateTime(item, « TemporalYearMonthString »).
    auto parse_result = TRY(parse_iso_date_time(vm, item.as_string().utf8_string_view(), { { Production::TemporalYearMonthString } }));

    // 5. Let calendar be result.[[Calendar]].
    // 6. If calendar is empty, set calendar to "iso8601".
    auto calendar = parse_result.calendar.value_or("iso8601"_string);

    // 7. Set calendar to ? CanonicalizeCalendar(calendar).
    calendar = TRY(canonicalize_calendar(vm, calendar));

    // 8. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 9. Perform ? GetTemporalOverflowOption(resolvedOptions).
    TRY(get_temporal_overflow_option(vm, resolved_options));

    // 10. Let isoDate be CreateISODateRecord(result.[[Year]], result.[[Month]], result.[[Day]]).
    auto iso_date = create_iso_date_record(*parse_result.year, parse_result.month, parse_result.day);

    // 11. If ISOYearMonthWithinLimits(isoDate) is false, throw a RangeError exception.
    if (!iso_year_month_within_limits(iso_date))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainYearMonth);

    // 12. Set result to ISODateToFields(calendar, isoDate, YEAR-MONTH).
    auto result = iso_date_to_fields(calendar, iso_date, DateType::YearMonth);

    // 13. NOTE: The following operation is called with CONSTRAIN regardless of the value of overflow, in order for the
    //     calendar to store a canonical value in the [[Day]] field of the [[ISODate]] internal slot of the result.
    // 14. Set isoDate to ? CalendarYearMonthFromFields(calendar, result, CONSTRAIN).
    iso_date = TRY(calendar_year_month_from_fields(vm, calendar, result, Overflow::Constrain));

    // 15. Return ! CreateTemporalYearMonth(isoDate, calendar).
    return MUST(create_temporal_year_month(vm, iso_date, move(calendar)));
}

// 9.5.3 ISOYearMonthWithinLimits ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isoyearmonthwithinlimits
bool iso_year_month_within_limits(ISODate iso_date)
{
    // 1. If isoDate.[[Year]] < -271821 or isoDate.[[Year]] > 275760, then
    if (iso_date.year < -271821 || iso_date.year > 275760) {
        // a. Return false.
        return false;
    }

    // 2. If isoDate.[[Year]] = -271821 and isoDate.[[Month]] < 4, then
    if (iso_date.year == -271821 && iso_date.month < 4) {
        // a. Return false.
        return false;
    }

    // 3. If isoDate.[[Year]] = 275760 and isoDate.[[Month]] > 9, then
    if (iso_date.year == 275760 && iso_date.month > 9) {
        // a. Return false.
        return false;
    }

    // 4. Return true.
    return true;
}

// 9.5.4 BalanceISOYearMonth ( year, month ), https://tc39.es/proposal-temporal/#sec-temporal-balanceisoyearmonth
ISOYearMonth balance_iso_year_month(double year, double month)
{
    // 1. Set year to year + floor((month - 1) / 12).
    year += floor((month - 1.0) / 12.0);

    // 2. Set month to ((month - 1) modulo 12) + 1.
    month = modulo(month - 1, 12.0) + 1.0;

    // 3. Return ISO Year-Month Record { [[Year]]: year, [[Month]]: month  }.
    return { .year = static_cast<i32>(year), .month = static_cast<u8>(month) };
}

// 9.5.5 CreateTemporalYearMonth ( isoDate, calendar [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporalyearmonth
ThrowCompletionOr<GC::Ref<PlainYearMonth>> create_temporal_year_month(VM& vm, ISODate iso_date, String calendar, GC::Ptr<FunctionObject> new_target)
{
    auto& realm = *vm.current_realm();

    // 1. If ISOYearMonthWithinLimits(isoDate) is false, throw a RangeError exception.
    if (!iso_year_month_within_limits(iso_date))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainYearMonth);

    // 2. If newTarget is not present, set newTarget to %Temporal.PlainYearMonth%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_plain_year_month_constructor();

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainYearMonth.prototype%", « [[InitializedTemporalYearMonth]], [[ISODate]], [[Calendar]] »).
    // 4. Set object.[[ISODate]] to isoDate.
    // 5. Set object.[[Calendar]] to calendar.
    auto object = TRY(ordinary_create_from_constructor<PlainYearMonth>(vm, *new_target, &Intrinsics::temporal_plain_year_month_prototype, iso_date, move(calendar)));

    // 6. Return object.
    return object;
}

// 9.5.6 TemporalYearMonthToString ( yearMonth, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-temporalyearmonthtostring
String temporal_year_month_to_string(PlainYearMonth const& year_month, ShowCalendar show_calendar)
{
    // 1. Let year be PadISOYear(yearMonth.[[ISODate]].[[Year]]).
    auto year = pad_iso_year(year_month.iso_date().year);

    // 2. Let month be ToZeroPaddedDecimalString(yearMonth.[[ISODate]].[[Month]], 2).
    // 3. Let result be the string-concatenation of year, the code unit 0x002D (HYPHEN-MINUS), and month.
    auto result = MUST(String::formatted("{}-{:02}", year, year_month.iso_date().month));

    // 4. If showCalendar is one of always or critical, or if yearMonth.[[Calendar]] is not "iso8601", then
    if (show_calendar == ShowCalendar::Always || show_calendar == ShowCalendar::Critical || year_month.calendar() != "iso8601"sv) {
        // a. Let day be ToZeroPaddedDecimalString(yearMonth.[[ISODate]].[[Day]], 2).
        // b. Set result to the string-concatenation of result, the code unit 0x002D (HYPHEN-MINUS), and day.
        result = MUST(String::formatted("{}-{:02}", result, year_month.iso_date().day));
    }

    // 5. Let calendarString be FormatCalendarAnnotation(yearMonth.[[Calendar]], showCalendar).
    auto calendar_string = format_calendar_annotation(year_month.calendar(), show_calendar);

    // 6. Set result to the string-concatenation of result and calendarString.
    result = MUST(String::formatted("{}{}", result, calendar_string));

    // 7. Return result.
    return result;
}

// 9.5.7 DifferenceTemporalPlainYearMonth ( operation, yearMonth, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalplainyearmonth
ThrowCompletionOr<GC::Ref<Duration>> difference_temporal_plain_year_month(VM& vm, DurationOperation operation, PlainYearMonth const& year_month, Value other_value, Value options)
{
    // 1. Set other to ? ToTemporalYearMonth(other).
    auto other = TRY(to_temporal_year_month(vm, other_value));

    // 2. Let calendar be yearMonth.[[Calendar]].
    auto const& calendar = year_month.calendar();

    // 3. If CalendarEquals(calendar, other.[[Calendar]]) is false, throw a RangeError exception.
    if (!calendar_equals(calendar, other->calendar()))
        return vm.throw_completion<RangeError>(ErrorType::TemporalDifferentCalendars);

    // 4. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 5. Let settings be ? GetDifferenceSettings(operation, resolvedOptions, DATE, « WEEK, DAY », MONTH, YEAR).
    auto settings = TRY(get_difference_settings(vm, operation, resolved_options, UnitGroup::Date, { { Unit::Week, Unit::Day } }, Unit::Month, Unit::Year));

    // 6. If CompareISODate(yearMonth.[[ISODate]], other.[[ISODate]]) = 0, then
    if (compare_iso_date(year_month.iso_date(), other->iso_date()) == 0) {
        // a. Return ! CreateTemporalDuration(0, 0, 0, 0, 0, 0, 0, 0, 0, 0).
        return MUST(create_temporal_duration(vm, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    }

    // 7. Let thisFields be ISODateToFields(calendar, yearMonth.[[ISODate]], YEAR-MONTH).
    auto this_fields = iso_date_to_fields(calendar, year_month.iso_date(), DateType::YearMonth);

    // 8. Set thisFields.[[Day]] to 1.
    this_fields.day = 1;

    // 9. Let thisDate be ? CalendarDateFromFields(calendar, thisFields, CONSTRAIN).
    auto this_date = TRY(calendar_date_from_fields(vm, calendar, this_fields, Overflow::Constrain));

    // 10. Let otherFields be ISODateToFields(calendar, other.[[ISODate]], YEAR-MONTH).
    auto other_fields = iso_date_to_fields(calendar, other->iso_date(), DateType::YearMonth);

    // 11. Set otherFields.[[Day]] to 1.
    other_fields.day = 1;

    // 12. Let otherDate be ? CalendarDateFromFields(calendar, otherFields, CONSTRAIN).
    auto other_date = TRY(calendar_date_from_fields(vm, calendar, other_fields, Overflow::Constrain));

    // 13. Let dateDifference be CalendarDateUntil(calendar, thisDate, otherDate, settings.[[LargestUnit]]).
    auto date_difference = calendar_date_until(vm, calendar, this_date, other_date, settings.largest_unit);

    // 14. Let yearsMonthsDifference be ! AdjustDateDurationRecord(dateDifference, 0, 0).
    auto years_months_difference = MUST(adjust_date_duration_record(vm, date_difference, 0, 0));

    // 15. Let duration be CombineDateAndTimeDuration(yearsMonthsDifference, 0).
    auto duration = combine_date_and_time_duration(years_months_difference, TimeDuration { 0 });

    // 16. If settings.[[SmallestUnit]] is not MONTH or settings.[[RoundingIncrement]] ≠ 1, then
    if (settings.smallest_unit != Unit::Month || settings.rounding_increment != 1) {
        // a. Let isoDateTime be CombineISODateAndTimeRecord(thisDate, MidnightTimeRecord()).
        auto iso_date_time = combine_iso_date_and_time_record(this_date, midnight_time_record());

        // b. Let isoDateTimeOther be CombineISODateAndTimeRecord(otherDate, MidnightTimeRecord()).
        auto iso_date_time_other = combine_iso_date_and_time_record(other_date, midnight_time_record());

        // c. Let destEpochNs be GetUTCEpochNanoseconds(isoDateTimeOther).
        auto dest_epoch_ns = get_utc_epoch_nanoseconds(iso_date_time_other);

        // d. Set duration to ? RoundRelativeDuration(duration, destEpochNs, isoDateTime, UNSET, calendar, settings.[[LargestUnit]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]]).
        duration = TRY(round_relative_duration(vm, move(duration), dest_epoch_ns, iso_date_time, {}, calendar, settings.largest_unit, settings.rounding_increment, settings.smallest_unit, settings.rounding_mode));
    }

    // 17. Let result be ! TemporalDurationFromInternal(duration, DAY).
    auto result = MUST(temporal_duration_from_internal(vm, duration, Unit::Day));

    // 18. If operation is SINCE, set result to CreateNegatedTemporalDuration(result).
    if (operation == DurationOperation::Since)
        result = create_negated_temporal_duration(vm, result);

    // 19. Return result.
    return result;
}

// 9.5.8 AddDurationToYearMonth ( operation, yearMonth, temporalDurationLike, options )
ThrowCompletionOr<GC::Ref<PlainYearMonth>> add_duration_to_year_month(VM& vm, ArithmeticOperation operation, PlainYearMonth const& year_month, Value temporal_duration_like, Value options)
{
    // 1. Let duration be ? ToTemporalDuration(temporalDurationLike).
    auto duration = TRY(to_temporal_duration(vm, temporal_duration_like));

    // 2. If operation is SUBTRACT, set duration to CreateNegatedTemporalDuration(duration).
    if (operation == ArithmeticOperation::Subtract)
        duration = create_negated_temporal_duration(vm, duration);

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 4. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
    auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

    // 5. Let sign be DurationSign(duration).
    auto sign = duration_sign(duration);

    // 6. Let calendar be yearMonth.[[Calendar]].
    auto const& calendar = year_month.calendar();

    // 7. Let fields be ISODateToFields(calendar, yearMonth.[[ISODate]], YEAR-MONTH).
    auto fields = iso_date_to_fields(calendar, year_month.iso_date(), DateType::YearMonth);

    // 8. Set fields.[[Day]] to 1.
    fields.day = 1;

    // 9. Let intermediateDate be ? CalendarDateFromFields(calendar, fields, CONSTRAIN).
    auto intermediate_date = TRY(calendar_date_from_fields(vm, calendar, fields, Overflow::Constrain));

    ISODate date;

    // 10. If sign < 0, then
    if (sign < 0) {
        // a. Let oneMonthDuration be ! CreateDateDurationRecord(0, 1, 0, 0).
        auto one_month_duration = MUST(create_date_duration_record(vm, 0, 1, 0, 0));

        // b. Let nextMonth be ? CalendarDateAdd(calendar, intermediateDate, oneMonthDuration, CONSTRAIN).
        auto next_month = TRY(calendar_date_add(vm, calendar, intermediate_date, one_month_duration, Overflow::Constrain));

        // c. Let date be BalanceISODate(nextMonth.[[Year]], nextMonth.[[Month]], nextMonth.[[Day]] - 1).
        date = balance_iso_date(next_month.year, next_month.month, next_month.day - 1);

        // d. Assert: ISODateWithinLimits(date) is true.
        VERIFY(iso_date_within_limits(date));
    }
    // 11. Else,
    else {
        // a. Let date be intermediateDate.
        date = intermediate_date;
    }

    // 12. Let durationToAdd be ToDateDurationRecordWithoutTime(duration).
    auto duration_to_add = to_date_duration_record_without_time(vm, duration);

    // 13. Let addedDate be ? CalendarDateAdd(calendar, date, durationToAdd, overflow).
    auto added_date = TRY(calendar_date_add(vm, calendar, date, duration_to_add, overflow));

    // 14. Let addedDateFields be ISODateToFields(calendar, addedDate, YEAR-MONTH).
    auto added_date_fields = iso_date_to_fields(calendar, added_date, DateType::YearMonth);

    // 15. Let isoDate be ? CalendarYearMonthFromFields(calendar, addedDateFields, overflow).
    auto iso_date = TRY(calendar_year_month_from_fields(vm, calendar, added_date_fields, overflow));

    // 16. Return ! CreateTemporalYearMonth(isoDate, calendar).
    return MUST(create_temporal_year_month(vm, iso_date, calendar));
}

}
