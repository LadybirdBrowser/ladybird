/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainDateTimeConstructor.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainDateTime);

PlainDateTime::PlainDateTime(ISODateTime const& iso_date_time, String calendar, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_iso_date_time(iso_date_time)
    , m_calendar(move(calendar))
{
}

// 5.5.2 TimeValueToISODateTimeRecord ( t ), https://tc39.es/proposal-temporal/#sec-temporal-timevaluetoisodatetimerecord
ISODateTime time_value_to_iso_date_time_record(double time_value)
{
    // 1. Let isoDate be CreateISODateRecord(ℝ(YearFromTime(t)), ℝ(MonthFromTime(t)) + 1, ℝ(DateFromTime(t))).
    auto iso_date = create_iso_date_record(year_from_time(time_value), month_from_time(time_value) + 1, date_from_time(time_value));

    // 2. Let time be CreateTimeRecord(ℝ(HourFromTime(t)), ℝ(MinFromTime(t)), ℝ(SecFromTime(t)), ℝ(msFromTime(t)), 0, 0).
    auto time = create_time_record(hour_from_time(time_value), min_from_time(time_value), sec_from_time(time_value), ms_from_time(time_value), 0, 0);

    // 3. Return ISO Date-Time Record { [[ISODate]]: isoDate, [[Time]]: time }.
    return { .iso_date = iso_date, .time = time };
}

// 5.5.3 CombineISODateAndTimeRecord ( isoDate, time ), https://tc39.es/proposal-temporal/#sec-temporal-combineisodateandtimerecord
ISODateTime combine_iso_date_and_time_record(ISODate iso_date, Time const& time)
{
    // 1. NOTE: time.[[Days]] is ignored.
    // 2. Return ISO Date-Time Record { [[ISODate]]: isoDate, [[Time]]: time }.
    return { .iso_date = iso_date, .time = time };
}

// nsMinInstant - nsPerDay
static auto const DATETIME_NANOSECONDS_MIN = "-8640000086400000000000"_sbigint;

// nsMaxInstant + nsPerDay
static auto const DATETIME_NANOSECONDS_MAX = "8640000086400000000000"_sbigint;

// 5.5.4 ISODateTimeWithinLimits ( isoDateTime ), https://tc39.es/proposal-temporal/#sec-temporal-isodatetimewithinlimits
bool iso_date_time_within_limits(ISODateTime const& iso_date_time)
{
    // 1. If abs(ISODateToEpochDays(isoDateTime.[[ISODate]].[[Year]], isoDateTime.[[ISODate]].[[Month]] - 1, isoDateTime.[[ISODate]].[[Day]])) > 10**8 + 1, return false.
    if (fabs(iso_date_to_epoch_days(iso_date_time.iso_date.year, iso_date_time.iso_date.month - 1, iso_date_time.iso_date.day)) > 100000001)
        return false;

    // 2. Let ns be ℝ(GetUTCEpochNanoseconds(isoDateTime)).
    auto nanoseconds = get_utc_epoch_nanoseconds(iso_date_time);

    // 3. If ns ≤ nsMinInstant - nsPerDay, then
    if (nanoseconds <= DATETIME_NANOSECONDS_MIN) {
        // a. Return false.
        return false;
    }

    // 4. If ns ≥ nsMaxInstant + nsPerDay, then
    if (nanoseconds >= DATETIME_NANOSECONDS_MAX) {
        // a. Return false.
        return false;
    }

    // 5. Return true.
    return true;
}

// 5.5.5 InterpretTemporalDateTimeFields ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-interprettemporaldatetimefields
ThrowCompletionOr<ISODateTime> interpret_temporal_date_time_fields(VM& vm, StringView calendar, CalendarFields& fields, Overflow overflow)
{
    // 1. Let isoDate be ? CalendarDateFromFields(calendar, fields, overflow).
    auto iso_date = TRY(calendar_date_from_fields(vm, calendar, fields, overflow));

    // 2. Let time be ? RegulateTime(fields.[[Hour]], fields.[[Minute]], fields.[[Second]], fields.[[Millisecond]], fields.[[Microsecond]], fields.[[Nanosecond]], overflow).
    auto time = TRY(regulate_time(vm, *fields.hour, *fields.minute, *fields.second, *fields.millisecond, *fields.microsecond, *fields.nanosecond, overflow));

    // 3. Return CombineISODateAndTimeRecord(isoDate, time).
    return combine_iso_date_and_time_record(iso_date, time);
}

// 5.5.6 ToTemporalDateTime ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaldatetime
ThrowCompletionOr<GC::Ref<PlainDateTime>> to_temporal_date_time(VM& vm, Value item, Value options)
{
    // 1. If options is not present, set options to undefined.

    // 2. If item is an Object, then
    if (item.is_object()) {
        auto const& object = item.as_object();

        // a. If item has an [[InitializedTemporalDateTime]] internal slot, then
        if (is<PlainDateTime>(object)) {
            auto const& plain_date_time = static_cast<PlainDateTime const&>(object);

            // i. Let resolvedOptions be ? GetOptionsObject(options).
            auto resolved_options = TRY(get_options_object(vm, options));

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            TRY(get_temporal_overflow_option(vm, resolved_options));

            // iii. Return ! CreateTemporalDateTime(item.[[ISODateTime]], item.[[Calendar]]).
            return MUST(create_temporal_date_time(vm, plain_date_time.iso_date_time(), plain_date_time.calendar()));
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

            // iv. Return ! CreateTemporalDateTime(isoDateTime, item.[[Calendar]]).
            return MUST(create_temporal_date_time(vm, iso_date_time, zoned_date_time.calendar()));
        }

        // c. If item has an [[InitializedTemporalDate]] internal slot, then
        if (is<PlainDate>(object)) {
            auto const& plain_date = static_cast<PlainDate const&>(object);

            // i. Let resolvedOptions be ? GetOptionsObject(options).
            auto resolved_options = TRY(get_options_object(vm, options));

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            TRY(get_temporal_overflow_option(vm, resolved_options));

            // iii. Let isoDateTime be CombineISODateAndTimeRecord(item.[[ISODate]], MidnightTimeRecord()).
            auto iso_date_time = combine_iso_date_and_time_record(plain_date.iso_date(), midnight_time_record());

            // iv. Return ? CreateTemporalDateTime(isoDateTime, item.[[Calendar]]).
            return TRY(create_temporal_date_time(vm, iso_date_time, plain_date.calendar()));
        }

        // d. Let calendar be ? GetTemporalCalendarIdentifierWithISODefault(item).
        auto calendar = TRY(get_temporal_calendar_identifier_with_iso_default(vm, object));

        // e. Let fields be ? PrepareCalendarFields(calendar, item, « YEAR, MONTH, MONTH-CODE, DAY », « HOUR, MINUTE, SECOND, MILLISECOND, MICROSECOND, NANOSECOND », «»).
        static constexpr auto calendar_field_names = to_array({ CalendarField::Year, CalendarField::Month, CalendarField::MonthCode, CalendarField::Day });
        static constexpr auto non_calendar_field_names = to_array({ CalendarField::Hour, CalendarField::Minute, CalendarField::Second, CalendarField::Millisecond, CalendarField::Microsecond, CalendarField::Nanosecond });
        auto fields = TRY(prepare_calendar_fields(vm, calendar, object, calendar_field_names, non_calendar_field_names, CalendarFieldList {}));

        // f. Let resolvedOptions be ? GetOptionsObject(options).
        auto resolved_options = TRY(get_options_object(vm, options));

        // g. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

        // h. Let result be ? InterpretTemporalDateTimeFields(calendar, fields, overflow).
        auto result = TRY(interpret_temporal_date_time_fields(vm, calendar, fields, overflow));

        // i. Return ? CreateTemporalDateTime(result, calendar).
        return TRY(create_temporal_date_time(vm, result, move(calendar)));
    }

    // 3. If item is not a String, throw a TypeError exception.
    if (!item.is_string())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidPlainDateTime);

    // 4. Let result be ? ParseISODateTime(item, « TemporalDateTimeString[~Zoned] »).
    auto result = TRY(parse_iso_date_time(vm, item.as_string().utf8_string_view(), { { Production::TemporalDateTimeString } }));

    // 5. If result.[[Time]] is START-OF-DAY, let time be MidnightTimeRecord(); else let time be result.[[Time]].
    auto time = result.time.has<ParsedISODateTime::StartOfDay>() ? midnight_time_record() : result.time.get<Time>();

    // 6. Let calendar be result.[[Calendar]].
    // 7. If calendar is empty, set calendar to "iso8601".
    auto calendar = result.calendar.value_or("iso8601"_string);

    // 8. Set calendar to ? CanonicalizeCalendar(calendar).
    calendar = TRY(canonicalize_calendar(vm, calendar));

    // 9. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 10. Perform ? GetTemporalOverflowOption(resolvedOptions).
    TRY(get_temporal_overflow_option(vm, resolved_options));

    // 11. Let isoDate be CreateISODateRecord(result.[[Year]], result.[[Month]], result.[[Day]]).
    auto iso_date = create_iso_date_record(*result.year, result.month, result.day);

    // 12. Let isoDateTime be CombineISODateAndTimeRecord(isoDate, time).
    auto iso_date_time = combine_iso_date_and_time_record(iso_date, time);

    // 13. Return ? CreateTemporalDateTime(isoDateTime, calendar).
    return TRY(create_temporal_date_time(vm, iso_date_time, move(calendar)));
}

// 5.5.7 BalanceISODateTime ( year, month, day, hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/proposal-temporal/#sec-temporal-balanceisodatetime
ISODateTime balance_iso_date_time(double year, double month, double day, double hour, double minute, double second, double millisecond, double microsecond, double nanosecond)
{
    // 1. Let balancedTime be BalanceTime(hour, minute, second, millisecond, microsecond, nanosecond).
    auto balanced_time = balance_time(hour, minute, second, millisecond, microsecond, nanosecond);

    // 2. Let balancedDate be BalanceISODate(year, month, day + balancedTime.[[Days]]).
    auto balanced_date = balance_iso_date(year, month, day + balanced_time.days);

    // 3. Return CombineISODateAndTimeRecord(balancedDate, balancedTime).
    return combine_iso_date_and_time_record(balanced_date, balanced_time);
}

// 5.5.8 CreateTemporalDateTime ( isoDateTime, calendar [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporaldatetime
ThrowCompletionOr<GC::Ref<PlainDateTime>> create_temporal_date_time(VM& vm, ISODateTime const& iso_date_time, String calendar, GC::Ptr<FunctionObject> new_target)
{
    auto& realm = *vm.current_realm();

    // 1. If ISODateTimeWithinLimits(isoDateTime) is false, then
    if (!iso_date_time_within_limits(iso_date_time)) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainDateTime);
    }

    // 2. If newTarget is not present, set newTarget to %Temporal.PlainDateTime%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_plain_date_time_constructor();

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainDateTime.prototype%", « [[InitializedTemporalDateTime]], [[ISODateTime]], [[Calendar]] »).
    // 4. Set object.[[ISODateTime]] to isoDateTime.
    // 5. Set object.[[Calendar]] to calendar.
    auto object = TRY(ordinary_create_from_constructor<PlainDateTime>(vm, *new_target, &Intrinsics::temporal_plain_date_time_prototype, iso_date_time, move(calendar)));

    // 6. Return object.
    return object;
}

// 5.5.9 ISODateTimeToString ( isoDateTime, calendar, precision, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-isodatetimetostring
String iso_date_time_to_string(ISODateTime const& iso_date_time, StringView calendar, SecondsStringPrecision::Precision precision, ShowCalendar show_calendar)
{
    // 1. Let yearString be PadISOYear(isoDateTime.[[ISODate]].[[Year]]).
    auto year_string = pad_iso_year(iso_date_time.iso_date.year);

    // 2. Let monthString be ToZeroPaddedDecimalString(isoDateTime.[[ISODate]].[[Month]], 2).
    auto month = iso_date_time.iso_date.month;

    // 3. Let dayString be ToZeroPaddedDecimalString(isoDateTime.[[ISODate]].[[Day]], 2).
    auto day = iso_date_time.iso_date.day;

    // 4. Let subSecondNanoseconds be isoDateTime.[[Time]].[[Millisecond]] × 10**6 + isoDateTime.[[Time]].[[Microsecond]] × 10**3 + isoDateTime.[[Time]].[[Nanosecond]].
    auto sub_second_nanoseconds = (static_cast<u64>(iso_date_time.time.millisecond) * 1'000'000) + (static_cast<u64>(iso_date_time.time.microsecond) * 1000) + static_cast<u64>(iso_date_time.time.nanosecond);

    // 5. Let timeString be FormatTimeString(isoDateTime.[[Time]].[[Hour]], isoDateTime.[[Time]].[[Minute]], isoDateTime.[[Time]].[[Second]], subSecondNanoseconds, precision).
    auto time_string = format_time_string(iso_date_time.time.hour, iso_date_time.time.minute, iso_date_time.time.second, sub_second_nanoseconds, precision);

    // 6. Let calendarString be FormatCalendarAnnotation(calendar, showCalendar).
    auto calendar_string = format_calendar_annotation(calendar, show_calendar);

    // 7. Return the string-concatenation of yearString, the code unit 0x002D (HYPHEN-MINUS), monthString, the code unit 0x002D (HYPHEN-MINUS),
    //    dayString, 0x0054 (LATIN CAPITAL LETTER T), timeString, and calendarString.
    return MUST(String::formatted("{}-{:02}-{:02}T{}{}", year_string, month, day, time_string, calendar_string));
}

// 5.5.10 CompareISODateTime ( isoDateTime1, isoDateTime2 ), https://tc39.es/proposal-temporal/#sec-temporal-compareisodatetime
i8 compare_iso_date_time(ISODateTime const& iso_date_time1, ISODateTime const& iso_date_time2)
{
    // 1. Let dateResult be CompareISODate(isoDateTime1.[[ISODate]], isoDateTime2.[[ISODate]]).
    auto date_result = compare_iso_date(iso_date_time1.iso_date, iso_date_time2.iso_date);

    // 2. If dateResult ≠ 0, return dateResult.
    if (date_result != 0)
        return date_result;

    // 3. Return CompareTimeRecord(isoDateTime1.[[Time]], isoDateTime2.[[Time]]).
    return compare_time_record(iso_date_time1.time, iso_date_time2.time);
}

// 5.5.11 RoundISODateTime ( isoDateTime, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundisodatetime
ISODateTime round_iso_date_time(ISODateTime const& iso_date_time, u64 increment, Unit unit, RoundingMode rounding_mode)
{
    // 1. Assert: ISODateTimeWithinLimits(isoDateTime) is true.
    VERIFY(iso_date_time_within_limits(iso_date_time));

    // 2. Let roundedTime be RoundTime(isoDateTime.[[Time]], increment, unit, roundingMode).
    auto rounded_time = round_time(iso_date_time.time, increment, unit, rounding_mode);

    // 3. Let balanceResult be BalanceISODate(isoDateTime.[[ISODate]].[[Year]], isoDateTime.[[ISODate]].[[Month]], isoDateTime.[[ISODate]].[[Day]] + roundedTime.[[Days]]).
    auto balance_result = balance_iso_date(iso_date_time.iso_date.year, iso_date_time.iso_date.month, iso_date_time.iso_date.day + rounded_time.days);

    // 4. Return CombineISODateAndTimeRecord(balanceResult, roundedTime).
    return combine_iso_date_and_time_record(balance_result, rounded_time);
}

// 5.5.12 DifferenceISODateTime ( isoDateTime1, isoDateTime2, calendar, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-differenceisodatetime
InternalDuration difference_iso_date_time(VM& vm, ISODateTime const& iso_date_time1, ISODateTime const& iso_date_time2, StringView calendar, Unit largest_unit)
{
    // 1. Assert: ISODateTimeWithinLimits(isoDateTime1) is true.
    VERIFY(iso_date_time_within_limits(iso_date_time1));

    // 2. Assert: ISODateTimeWithinLimits(isoDateTime2) is true.
    VERIFY(iso_date_time_within_limits(iso_date_time2));

    // 3. Let timeDuration be DifferenceTime(isoDateTime1.[[Time]], isoDateTime2.[[Time]]).
    auto time_duration = difference_time(iso_date_time1.time, iso_date_time2.time);

    // 4. Let timeSign be TimeDurationSign(timeDuration).
    auto time_sign = time_duration_sign(time_duration);

    // 5. Let dateSign be CompareISODate(isoDateTime1.[[ISODate]], isoDateTime2.[[ISODate]]).
    auto date_sign = compare_iso_date(iso_date_time1.iso_date, iso_date_time2.iso_date);

    // 6. Let adjustedDate be isoDateTime2.[[ISODate]].
    auto adjusted_date = iso_date_time2.iso_date;

    // 7. If timeSign = dateSign, then
    if (time_sign == date_sign) {
        // a. Set adjustedDate to BalanceISODate(adjustedDate.[[Year]], adjustedDate.[[Month]], adjustedDate.[[Day]] + timeSign).
        adjusted_date = balance_iso_date(adjusted_date.year, adjusted_date.month, static_cast<double>(adjusted_date.day) + time_sign);

        // b. Set timeDuration to ! Add24HourDaysToTimeDuration(timeDuration, -timeSign).
        time_duration = MUST(add_24_hour_days_to_time_duration(vm, time_duration, -time_sign));
    }

    // 8. Let dateLargestUnit be LargerOfTwoTemporalUnits(DAY, largestUnit).
    auto date_largest_unit = larger_of_two_temporal_units(Unit::Day, largest_unit);

    // 9. Let dateDifference be CalendarDateUntil(calendar, isoDateTime1.[[ISODate]], adjustedDate, dateLargestUnit).
    auto date_difference = calendar_date_until(vm, calendar, iso_date_time1.iso_date, adjusted_date, date_largest_unit);

    // 10. If largestUnit is not dateLargestUnit, then
    if (largest_unit != date_largest_unit) {
        // a. Set timeDuration to ! Add24HourDaysToTimeDuration(timeDuration, dateDifference.[[Days]]).
        time_duration = MUST(add_24_hour_days_to_time_duration(vm, time_duration, date_difference.days));

        // b. Set dateDifference.[[Days]] to 0.
        date_difference.days = 0;
    }

    // 11. Return CombineDateAndTimeDuration(dateDifference, timeDuration).
    return combine_date_and_time_duration(date_difference, move(time_duration));
}

// 5.5.13 DifferencePlainDateTimeWithRounding ( isoDateTime1, isoDateTime2, calendar, largestUnit, roundingIncrement, smallestUnit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-differenceplaindatetimewithrounding
ThrowCompletionOr<InternalDuration> difference_plain_date_time_with_rounding(VM& vm, ISODateTime const& iso_date_time1, ISODateTime const& iso_date_time2, StringView calendar, Unit largest_unit, u64 rounding_increment, Unit smallest_unit, RoundingMode rounding_mode)
{
    // 1. If CompareISODateTime(isoDateTime1, isoDateTime2) = 0, then
    if (compare_iso_date_time(iso_date_time1, iso_date_time2) == 0) {
        // a. Return CombineDateAndTimeDuration(ZeroDateDuration(), 0).
        return combine_date_and_time_duration(zero_date_duration(vm), TimeDuration { 0 });
    }

    // 2. If ISODateTimeWithinLimits(isoDateTime1) is false or ISODateTimeWithinLimits(isoDateTime2) is false, throw a
    //    RangeError exception.
    if (!iso_date_time_within_limits(iso_date_time1) || !iso_date_time_within_limits(iso_date_time2))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODateTime);

    // 3. Let diff be DifferenceISODateTime(isoDateTime1, isoDateTime2, calendar, largestUnit).
    auto diff = difference_iso_date_time(vm, iso_date_time1, iso_date_time2, calendar, largest_unit);

    // 4. If smallestUnit is NANOSECOND and roundingIncrement = 1, return diff.
    if (smallest_unit == Unit::Nanosecond && rounding_increment == 1)
        return diff;

    // 5. Let destEpochNs be GetUTCEpochNanoseconds(isoDateTime2).
    auto dest_epoch_ns = get_utc_epoch_nanoseconds(iso_date_time2);

    // 6. Return ? RoundRelativeDuration(diff, destEpochNs, isoDateTime1, UNSET, calendar, largestUnit, roundingIncrement, smallestUnit, roundingMode).
    return TRY(round_relative_duration(vm, diff, dest_epoch_ns, iso_date_time1, {}, calendar, largest_unit, rounding_increment, smallest_unit, rounding_mode));
}

// 5.5.14 DifferencePlainDateTimeWithTotal ( isoDateTime1, isoDateTime2, calendar, unit ), https://tc39.es/proposal-temporal/#sec-temporal-differenceplaindatetimewithtotal
ThrowCompletionOr<Crypto::BigFraction> difference_plain_date_time_with_total(VM& vm, ISODateTime const& iso_date_time1, ISODateTime const& iso_date_time2, StringView calendar, Unit unit)
{
    // 1. If CompareISODateTime(isoDateTime1, isoDateTime2) = 0, then
    if (compare_iso_date_time(iso_date_time1, iso_date_time2) == 0) {
        // a. Return 0.
        return Crypto::BigFraction {};
    }

    // 2. If ISODateTimeWithinLimits(isoDateTime1) is false or ISODateTimeWithinLimits(isoDateTime2) is false, throw a
    //    RangeError exception.
    if (!iso_date_time_within_limits(iso_date_time1) || !iso_date_time_within_limits(iso_date_time2))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODateTime);

    // 3. Let diff be DifferenceISODateTime(isoDateTime1, isoDateTime2, calendar, unit).
    auto diff = difference_iso_date_time(vm, iso_date_time1, iso_date_time2, calendar, unit);

    // 4. If unit is NANOSECOND, return diff.[[Time]].
    if (unit == Unit::Nanosecond)
        return move(diff.time);

    // 5. Let destEpochNs be GetUTCEpochNanoseconds(isoDateTime2).
    auto dest_epoch_ns = get_utc_epoch_nanoseconds(iso_date_time2);

    // 6. Return ? TotalRelativeDuration(diff, destEpochNs, isoDateTime1, UNSET, calendar, unit).
    return TRY(total_relative_duration(vm, diff, dest_epoch_ns, iso_date_time1, {}, calendar, unit));
}

// 5.5.15 DifferenceTemporalPlainDateTime ( operation, dateTime, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalplaindatetime
ThrowCompletionOr<GC::Ref<Duration>> difference_temporal_plain_date_time(VM& vm, DurationOperation operation, PlainDateTime const& date_time, Value other_value, Value options)
{
    // 1. Set other to ? ToTemporalDateTime(other).
    auto other = TRY(to_temporal_date_time(vm, other_value));

    // 2. If CalendarEquals(dateTime.[[Calendar]], other.[[Calendar]]) is false, throw a RangeError exception.
    if (!calendar_equals(date_time.calendar(), other->calendar()))
        return vm.throw_completion<RangeError>(ErrorType::TemporalDifferentCalendars);

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 4. Let settings be ? GetDifferenceSettings(operation, resolvedOptions, DATETIME, « », NANOSECOND, DAY).
    auto settings = TRY(get_difference_settings(vm, operation, resolved_options, UnitGroup::DateTime, {}, Unit::Nanosecond, Unit::Day));

    // 5. If CompareISODateTime(dateTime.[[ISODateTime]], other.[[ISODateTime]]) = 0, then
    if (compare_iso_date_time(date_time.iso_date_time(), other->iso_date_time()) == 0) {
        // a. Return ! CreateTemporalDuration(0, 0, 0, 0, 0, 0, 0, 0, 0, 0).
        return MUST(create_temporal_duration(vm, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    }

    // 6. Let internalDuration be ? DifferencePlainDateTimeWithRounding(dateTime.[[ISODateTime]], other.[[ISODateTime]], dateTime.[[Calendar]], settings.[[LargestUnit]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]]).
    auto internal_duration = TRY(difference_plain_date_time_with_rounding(vm, date_time.iso_date_time(), other->iso_date_time(), date_time.calendar(), settings.largest_unit, settings.rounding_increment, settings.smallest_unit, settings.rounding_mode));

    // 7. Let result be ! TemporalDurationFromInternal(internalDuration, settings.[[LargestUnit]]).
    auto result = MUST(temporal_duration_from_internal(vm, internal_duration, settings.largest_unit));

    // 8. If operation is SINCE, set result to CreateNegatedTemporalDuration(result).
    if (operation == DurationOperation::Since)
        result = create_negated_temporal_duration(vm, result);

    // 9. Return result.
    return result;
}

// 5.5.16 AddDurationToDateTime ( operation, dateTime, temporalDurationLike, options ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtodatetime
ThrowCompletionOr<GC::Ref<PlainDateTime>> add_duration_to_date_time(VM& vm, ArithmeticOperation operation, PlainDateTime const& date_time, Value temporal_duration_like, Value options)
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

    // 5. Let internalDuration be ToInternalDurationRecordWith24HourDays(duration).
    auto internal_duration = to_internal_duration_record_with_24_hour_days(vm, duration);

    // 6. Let timeResult be AddTime(dateTime.[[ISODateTime]].[[Time]], internalDuration.[[Time]]).
    auto time_result = add_time(date_time.iso_date_time().time, internal_duration.time);

    // 7. Let dateDuration be ? AdjustDateDurationRecord(internalDuration.[[Date]], timeResult.[[Days]]).
    auto date_duration = TRY(adjust_date_duration_record(vm, internal_duration.date, time_result.days));

    // 8. Let addedDate be ? CalendarDateAdd(dateTime.[[Calendar]], dateTime.[[ISODateTime]].[[ISODate]], dateDuration, overflow).
    auto added_date = TRY(calendar_date_add(vm, date_time.calendar(), date_time.iso_date_time().iso_date, date_duration, overflow));

    // 9. Let result be CombineISODateAndTimeRecord(addedDate, timeResult).
    auto result = combine_iso_date_and_time_record(added_date, time_result);

    // 10. Return ? CreateTemporalDateTime(result, dateTime.[[Calendar]]).
    return TRY(create_temporal_date_time(vm, result, date_time.calendar()));
}

}
