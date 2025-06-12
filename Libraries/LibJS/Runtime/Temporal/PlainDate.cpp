/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/NumericLimits.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/DateEquations.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateConstructor.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainDate);

// 3 Temporal.PlainDate Objects, https://tc39.es/proposal-temporal/#sec-temporal-plaindate-objects
PlainDate::PlainDate(ISODate iso_date, String calendar, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_iso_date(iso_date)
    , m_calendar(move(calendar))
{
}

// 3.5.2 CreateISODateRecord ( year, month, day ), https://tc39.es/proposal-temporal/#sec-temporal-create-iso-date-record
ISODate create_iso_date_record(double year, double month, double day)
{
    // 1. Assert: IsValidISODate(year, month, day) is true.
    VERIFY(is_valid_iso_date(year, month, day));

    // 2. Return ISO Date Record { [[Year]]: year, [[Month]]: month, [[Day]]: day }.
    return { .year = static_cast<i32>(year), .month = static_cast<u8>(month), .day = static_cast<u8>(day) };
}

// 3.5.3 CreateTemporalDate ( isoDate, calendar [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporaldate
ThrowCompletionOr<GC::Ref<PlainDate>> create_temporal_date(VM& vm, ISODate iso_date, String calendar, GC::Ptr<FunctionObject> new_target)
{
    auto& realm = *vm.current_realm();

    // 1. If ISODateWithinLimits(isoDate) is false, throw a RangeError exception.
    if (!iso_date_within_limits(iso_date))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDate);

    // 2. If newTarget is not present, set newTarget to %Temporal.PlainDate%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_plain_date_constructor();

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainDate.prototype%", « [[InitializedTemporalDate]], [[ISODate]], [[Calendar]] »).
    // 4. Set object.[[ISODate]] to isoDate.
    // 5. Set object.[[Calendar]] to calendar.
    auto object = TRY(ordinary_create_from_constructor<PlainDate>(vm, *new_target, &Intrinsics::temporal_plain_date_prototype, iso_date, move(calendar)));

    // 6. Return object.
    return object;
}

// 3.5.4 ToTemporalDate ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaldate
ThrowCompletionOr<GC::Ref<PlainDate>> to_temporal_date(VM& vm, Value item, Value options)
{
    // 1. If options is not present, set options to undefined.

    // 2. If item is an Object, then
    if (item.is_object()) {
        auto const& object = item.as_object();

        // a. If item has an [[InitializedTemporalDate]] internal slot, then
        if (is<PlainDate>(object)) {
            auto const& plain_date = static_cast<PlainDate const&>(object);

            // i. Let resolvedOptions be ? GetOptionsObject(options).
            auto resolved_options = TRY(get_options_object(vm, options));

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            TRY(get_temporal_overflow_option(vm, resolved_options));

            // iii. Return ! CreateTemporalDate(item.[[ISODate]], item.[[Calendar]]).
            return MUST(create_temporal_date(vm, plain_date.iso_date(), plain_date.calendar()));
        }

        // b. If item has an [[InitializedTemporalZonedDateTime]] internal slot, then
        if (is<ZonedDateTime>(object)) {
            auto const& zoned_date_time = static_cast<ZonedDateTime const&>(object);

            // i. Let isoDateTime be GetISODateTimeFor(item.[[TimeZone]], item.[[EpochNanoseconds]]).
            auto iso_date_time = get_iso_date_time_for(zoned_date_time.time_zone(), zoned_date_time.epoch_nanoseconds()->big_integer());

            // ii. Let resolvedOptions be ? GetOptionsObject(options).
            auto resolved_options = TRY(get_options_object(vm, options));

            // iii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            TRY(get_temporal_overflow_option(vm, resolved_options));

            // iv. Return ! CreateTemporalDate(isoDateTime.[[ISODate]], item.[[Calendar]]).
            return MUST(create_temporal_date(vm, iso_date_time.iso_date, zoned_date_time.calendar()));
        }

        // c. If item has an [[InitializedTemporalDateTime]] internal slot, then
        if (is<PlainDateTime>(object)) {
            auto const& plain_date_time = static_cast<PlainDateTime const&>(object);

            // i. Let resolvedOptions be ? GetOptionsObject(options).
            auto resolved_options = TRY(get_options_object(vm, options));

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            TRY(get_temporal_overflow_option(vm, resolved_options));

            // iii. Return ! CreateTemporalDate(item.[[ISODateTime]].[[ISODate]], item.[[Calendar]]).
            return MUST(create_temporal_date(vm, plain_date_time.iso_date_time().iso_date, plain_date_time.calendar()));
        }

        // d. Let calendar be ? GetTemporalCalendarIdentifierWithISODefault(item).
        auto calendar = TRY(get_temporal_calendar_identifier_with_iso_default(vm, object));

        // e. Let fields be ? PrepareCalendarFields(calendar, item, « YEAR, MONTH, MONTH-CODE, DAY », «», «»).
        auto fields = TRY(prepare_calendar_fields(vm, calendar, object, { { CalendarField::Year, CalendarField::Month, CalendarField::MonthCode, CalendarField::Day } }, {}, CalendarFieldList {}));

        // f. Let resolvedOptions be ? GetOptionsObject(options).
        auto resolved_options = TRY(get_options_object(vm, options));

        // g. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

        // h. Let isoDate be ? CalendarDateFromFields(calendar, fields, overflow).
        auto iso_date = TRY(calendar_date_from_fields(vm, calendar, fields, overflow));

        // i. Return ! CreateTemporalDate(isoDate, calendar).
        return MUST(create_temporal_date(vm, iso_date, move(calendar)));
    }

    // 3. If item is not a String, throw a TypeError exception.
    if (!item.is_string())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidPlainDate);

    // 4. Let result be ? ParseISODateTime(item, « TemporalDateTimeString[~Zoned] »).
    auto result = TRY(parse_iso_date_time(vm, item.as_string().utf8_string_view(), { { Production::TemporalDateTimeString } }));

    // 5. Let calendar be result.[[Calendar]].
    // 6. If calendar is empty, set calendar to "iso8601".
    auto calendar = result.calendar.value_or("iso8601"_string);

    // 7. Set calendar to ? CanonicalizeCalendar(calendar).
    calendar = TRY(canonicalize_calendar(vm, calendar));

    // 8. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 9. Perform ? GetTemporalOverflowOption(resolvedOptions).
    TRY(get_temporal_overflow_option(vm, resolved_options));

    // 10. Let isoDate be CreateISODateRecord(result.[[Year]], result.[[Month]], result.[[Day]]).
    auto iso_date = create_iso_date_record(*result.year, result.month, result.day);

    // 11. Return ? CreateTemporalDate(isoDate, calendar).
    return TRY(create_temporal_date(vm, iso_date, move(calendar)));
}

// 3.5.5 ISODateSurpasses ( sign, y1, m1, d1, isoDate2 ), https://tc39.es/proposal-temporal/#sec-temporal-isodatesurpasses
bool iso_date_surpasses(i8 sign, double year1, double month1, double day1, ISODate iso_date2)
{
    // 1. If y1 ≠ isoDate2.[[Year]], then
    if (year1 != iso_date2.year) {
        // a. If sign × (y1 - isoDate2.[[Year]]) > 0, return true.
        if (sign * (year1 - iso_date2.year) > 0)
            return true;
    }
    // 2. Else if m1 ≠ isoDate2.[[Month]], then
    else if (month1 != iso_date2.month) {
        // a. If sign × (m1 - isoDate2.[[Month]]) > 0, return true.
        if (sign * (month1 - iso_date2.month) > 0)
            return true;
    }
    // 3. Else if d1 ≠ isoDate2.[[Day]], then
    else if (day1 != iso_date2.day) {
        // a. If sign × (d1 - isoDate2.[[Day]]) > 0, return true.
        if (sign * (day1 - iso_date2.day) > 0)
            return true;
    }

    // 4. Return false.
    return false;
}

// 3.5.6 RegulateISODate ( year, month, day, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-regulateisodate
ThrowCompletionOr<ISODate> regulate_iso_date(VM& vm, double year, double month, double day, Overflow overflow)
{
    switch (overflow) {
    // 1. If overflow is CONSTRAIN, then
    case Overflow::Constrain:
        // a. Set month to the result of clamping month between 1 and 12.
        month = clamp(month, 1, 12);

        // b. Let daysInMonth be ISODaysInMonth(year, month).
        // c. Set day to the result of clamping day between 1 and daysInMonth.
        day = clamp(day, 1, iso_days_in_month(year, month));

        // AD-HOC: We further clamp the year to the range allowed by ISODate.year, to ensure we do not overflow when we
        //         store the year as an integer.
        using YearType = decltype(declval<ISODate>().year);
        year = clamp(year, static_cast<double>(NumericLimits<YearType>::min()), static_cast<double>(NumericLimits<YearType>::max()));

        break;

    // 2. Else,
    case Overflow::Reject:
        // a. Assert: overflow is REJECT.
        // b. If IsValidISODate(year, month, day) is false, throw a RangeError exception.
        if (!is_valid_iso_date(year, month, day))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODate);
        break;
    }

    // 3. Return CreateISODateRecord(year, month, day).
    return create_iso_date_record(year, month, day);
}

// 3.5.7 IsValidISODate ( year, month, day ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidisodate
bool is_valid_iso_date(double year, double month, double day)
{
    // AD-HOC: This is an optimization that allows us to treat these doubles as normal integers from this point onwards.
    //         This does not change the exposed behavior as the call to CreateISODateRecord will immediately check that
    //         these values are valid ISO values (years: [-271821, 275760], months: [1, 12], days: [1, 31]), all of
    //         which are subsets of this check.
    if (!AK::is_within_range<i32>(year) || !AK::is_within_range<u8>(month) || !AK::is_within_range<u8>(day))
        return false;

    // 1. If month < 1 or month > 12, then
    if (month < 1 || month > 12) {
        // a. Return false.
        return false;
    }

    // 2. Let daysInMonth be ISODaysInMonth(year, month).
    auto days_in_month = iso_days_in_month(year, month);

    // 3. If day < 1 or day > daysInMonth, then
    if (day < 1 || day > days_in_month) {
        // a. Return false.
        return false;
    }

    // 4. Return true.
    return true;
}

// 3.5.8 BalanceISODate ( year, month, day ), https://tc39.es/proposal-temporal/#sec-temporal-balanceisodate
ISODate balance_iso_date(double year, double month, double day)
{
    // 1. Let epochDays be ISODateToEpochDays(year, month - 1, day).
    auto epoch_days = iso_date_to_epoch_days(year, month - 1, day);

    // 2. Let ms be EpochDaysToEpochMs(epochDays, 0).
    auto ms = epoch_days_to_epoch_ms(epoch_days, 0);

    // 3. Return CreateISODateRecord(EpochTimeToEpochYear(ms), EpochTimeToMonthInYear(ms) + 1, EpochTimeToDate(ms)).
    return create_iso_date_record(epoch_time_to_epoch_year(ms), epoch_time_to_month_in_year(ms) + 1.0, epoch_time_to_date(ms));
}

// 3.5.9 PadISOYear ( y ), https://tc39.es/proposal-temporal/#sec-temporal-padisoyear
String pad_iso_year(i32 year)
{
    // 1. If y ≥ 0 and y ≤ 9999, then
    if (year >= 0 && year <= 9999) {
        // a. Return ToZeroPaddedDecimalString(y, 4).
        return MUST(String::formatted("{:04}", year));
    }

    // 2. If y > 0, let yearSign be "+"; otherwise, let yearSign be "-".
    auto year_sign = year > 0 ? '+' : '-';

    // 3. Let year be ToZeroPaddedDecimalString(abs(y), 6).
    // 4. Return the string-concatenation of yearSign and year.
    return MUST(String::formatted("{}{:06}", year_sign, abs(year)));
}

// 3.5.10 TemporalDateToString ( temporalDate, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-temporaldatetostring
String temporal_date_to_string(PlainDate const& temporal_date, ShowCalendar show_calendar)
{
    // 1. Let year be PadISOYear(temporalDate.[[ISODate]].[[Year]]).
    auto year = pad_iso_year(temporal_date.iso_date().year);

    // 2. Let month be ToZeroPaddedDecimalString(temporalDate.[[ISODate]].[[Month]], 2).
    auto month = temporal_date.iso_date().month;

    // 3. Let day be ToZeroPaddedDecimalString(temporalDate.[[ISODate]].[[Day]], 2).
    auto day = temporal_date.iso_date().day;

    // 4. Let calendar be FormatCalendarAnnotation(temporalDate.[[Calendar]], showCalendar).
    auto calendar = format_calendar_annotation(temporal_date.calendar(), show_calendar);

    // 5. Return the string-concatenation of year, the code unit 0x002D (HYPHEN-MINUS), month, the code unit 0x002D (HYPHEN-MINUS), day, and calendar.
    return MUST(String::formatted("{}-{:02}-{:02}{}", year, month, day, calendar));
}

// 3.5.11 ISODateWithinLimits ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isodatewithinlimits
bool iso_date_within_limits(ISODate iso_date)
{
    // 1. Let isoDateTime be CombineISODateAndTimeRecord(isoDate, NoonTimeRecord()).
    auto iso_date_time = combine_iso_date_and_time_record(iso_date, noon_time_record());

    // 2. Return ISODateTimeWithinLimits(isoDateTime).
    return iso_date_time_within_limits(iso_date_time);
}

// 3.5.12 CompareISODate ( isoDate1, isoDate2 ), https://tc39.es/proposal-temporal/#sec-temporal-compareisodate
i8 compare_iso_date(ISODate iso_date1, ISODate iso_date2)
{
    // 1. If isoDate1.[[Year]] > isoDate2.[[Year]], return 1.
    if (iso_date1.year > iso_date2.year)
        return 1;

    // 2. If isoDate1.[[Year]] < isoDate2.[[Year]], return -1.
    if (iso_date1.year < iso_date2.year)
        return -1;

    // 3. If isoDate1.[[Month]] > isoDate2.[[Month]], return 1.
    if (iso_date1.month > iso_date2.month)
        return 1;

    // 4. If isoDate1.[[Month]] < isoDate2.[[Month]], return -1.
    if (iso_date1.month < iso_date2.month)
        return -1;

    // 5. If isoDate1.[[Day]] > isoDate2.[[Day]], return 1.
    if (iso_date1.day > iso_date2.day)
        return 1;

    // 6. If isoDate1.[[Day]] < isoDate2.[[Day]], return -1.
    if (iso_date1.day < iso_date2.day)
        return -1;

    // 7. Return 0.
    return 0;
}

// 3.5.13 DifferenceTemporalPlainDate ( operation, temporalDate, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalplaindate
ThrowCompletionOr<GC::Ref<Duration>> difference_temporal_plain_date(VM& vm, DurationOperation operation, PlainDate const& temporal_date, Value other_value, Value options)
{
    // 1. Set other to ? ToTemporalDate(other).
    auto other = TRY(to_temporal_date(vm, other_value));

    // 2. If CalendarEquals(temporalDate.[[Calendar]], other.[[Calendar]]) is false, throw a RangeError exception.
    if (!calendar_equals(temporal_date.calendar(), other->calendar()))
        return vm.throw_completion<RangeError>(ErrorType::TemporalDifferentCalendars);

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 4. Let settings be ? GetDifferenceSettings(operation, resolvedOptions, DATE, « », DAY, DAY).
    auto settings = TRY(get_difference_settings(vm, operation, resolved_options, UnitGroup::Date, {}, Unit::Day, Unit::Day));

    // 5. If CompareISODate(temporalDate.[[ISODate]], other.[[ISODate]]) = 0, then
    if (compare_iso_date(temporal_date.iso_date(), other->iso_date()) == 0) {
        // a. Return ! CreateTemporalDuration(0, 0, 0, 0, 0, 0, 0, 0, 0, 0).
        return MUST(create_temporal_duration(vm, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    }

    // 6. Let dateDifference be CalendarDateUntil(temporalDate.[[Calendar]], temporalDate.[[ISODate]], other.[[ISODate]], settings.[[LargestUnit]]).
    auto date_difference = calendar_date_until(vm, temporal_date.calendar(), temporal_date.iso_date(), other->iso_date(), settings.largest_unit);

    // 7. Let duration be CombineDateAndTimeDuration(dateDifference, 0).
    auto duration = combine_date_and_time_duration(date_difference, TimeDuration { 0 });

    // 8. If settings.[[SmallestUnit]] is not DAY or settings.[[RoundingIncrement]] ≠ 1, then
    if (settings.smallest_unit != Unit::Day || settings.rounding_increment != 1) {
        // a. Let isoDateTime be CombineISODateAndTimeRecord(temporalDate.[[ISODate]], MidnightTimeRecord()).
        auto iso_date_time = combine_iso_date_and_time_record(temporal_date.iso_date(), midnight_time_record());

        // b. Let isoDateTimeOther be CombineISODateAndTimeRecord(other.[[ISODate]], MidnightTimeRecord()).
        auto iso_date_time_other = combine_iso_date_and_time_record(other->iso_date(), midnight_time_record());

        // c. Let destEpochNs be GetUTCEpochNanoseconds(isoDateTimeOther).
        auto dest_epoch_ns = get_utc_epoch_nanoseconds(iso_date_time_other);

        // d. Set duration to ? RoundRelativeDuration(duration, destEpochNs, isoDateTime, UNSET, temporalDate.[[Calendar]], settings.[[LargestUnit]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]]).
        duration = TRY(round_relative_duration(vm, move(duration), dest_epoch_ns, iso_date_time, {}, temporal_date.calendar(), settings.largest_unit, settings.rounding_increment, settings.smallest_unit, settings.rounding_mode));
    }

    // 9. Let result be ! TemporalDurationFromInternal(duration, DAY).
    auto result = MUST(temporal_duration_from_internal(vm, duration, Unit::Day));

    // 10. If operation is since, set result to CreateNegatedTemporalDuration(result).
    if (operation == DurationOperation::Since)
        result = create_negated_temporal_duration(vm, result);

    // 11. Return result.
    return result;
}

// 3.5.14 AddDurationToDate ( operation, temporalDate, temporalDurationLike, options ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtodate
ThrowCompletionOr<GC::Ref<PlainDate>> add_duration_to_date(VM& vm, ArithmeticOperation operation, PlainDate const& temporal_date, Value temporal_duration_like, Value options)
{
    // 1. Let calendar be temporalDate.[[Calendar]].
    auto const& calendar = temporal_date.calendar();

    // 2. Let duration be ? ToTemporalDuration(temporalDurationLike).
    auto duration = TRY(to_temporal_duration(vm, temporal_duration_like));

    // 3. If operation is SUBTRACT, set duration to CreateNegatedTemporalDuration(duration).
    if (operation == ArithmeticOperation::Subtract)
        duration = create_negated_temporal_duration(vm, duration);

    // 4. Let dateDuration be ToDateDurationRecordWithoutTime(duration).
    auto date_duration = to_date_duration_record_without_time(vm, duration);

    // 5. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 6. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
    auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

    // 7. Let result be ? CalendarDateAdd(calendar, temporalDate.[[ISODate]], dateDuration, overflow).
    auto result = TRY(calendar_date_add(vm, calendar, temporal_date.iso_date(), date_duration, overflow));

    // 8. Return ! CreateTemporalDate(result, calendar).
    return MUST(create_temporal_date(vm, result, calendar));
}

}
