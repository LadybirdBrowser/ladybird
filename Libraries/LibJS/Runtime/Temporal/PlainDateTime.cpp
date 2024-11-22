/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>

namespace JS::Temporal {

// 5.5.3 CombineISODateAndTimeRecord ( isoDate, time ), https://tc39.es/proposal-temporal/#sec-temporal-combineisodateandtimerecord
ISODateTime combine_iso_date_and_time_record(ISODate iso_date, Time time)
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
bool iso_date_time_within_limits(ISODateTime iso_date_time)
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

// 5.5.12 DifferenceISODateTime ( isoDateTime1, isoDateTime2, calendar, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-differenceisodatetime
ThrowCompletionOr<InternalDuration> difference_iso_date_time(VM& vm, ISODateTime const& iso_date_time1, ISODateTime const& iso_date_time2, StringView calendar, Unit largest_unit)
{
    // 1. Assert: ISODateTimeWithinLimits(isoDateTime1) is true.
    VERIFY(iso_date_time_within_limits(iso_date_time1));

    // 2. Assert: ISODateTimeWithinLimits(isoDateTime2) is true.
    VERIFY(iso_date_time_within_limits(iso_date_time2));

    // 3. Let timeDuration be DifferenceTime(isoDateTime1.[[Time]], isoDateTime2.[[Time]]).
    auto time_duration = difference_time(iso_date_time1.time, iso_date_time2.time);

    // 4. Let timeSign be TimeDurationSign(timeDuration).
    auto time_sign = time_duration_sign(time_duration);

    // 5. Let dateSign be CompareISODate(isoDateTime2.[[ISODate]], isoDateTime1.[[ISODate]]).
    auto date_sign = compare_iso_date(iso_date_time2.iso_date, iso_date_time1.iso_date);

    // 6. Let adjustedDate be isoDateTime2.[[ISODate]].
    auto adjusted_date = iso_date_time2.iso_date;

    // 7. If timeSign = -dateSign, then
    if (time_sign == -date_sign) {
        // a. Set adjustedDate to BalanceISODate(adjustedDate.[[Year]], adjustedDate.[[Month]], adjustedDate.[[Day]] + timeSign).
        adjusted_date = balance_iso_date(adjusted_date.year, adjusted_date.month, static_cast<double>(adjusted_date.day) + time_sign);

        // b. Set timeDuration to ? Add24HourDaysToTimeDuration(timeDuration, -timeSign).
        time_duration = TRY(add_24_hour_days_to_time_duration(vm, time_duration, -time_sign));
    }

    // 8. Let dateLargestUnit be LargerOfTwoTemporalUnits(DAY, largestUnit).
    auto date_largest_unit = larger_of_two_temporal_units(Unit::Day, largest_unit);

    // 9. Let dateDifference be CalendarDateUntil(calendar, isoDateTime1.[[ISODate]], adjustedDate, dateLargestUnit).
    auto date_difference = calendar_date_until(vm, calendar, iso_date_time1.iso_date, adjusted_date, date_largest_unit);

    // 10. If largestUnit is not dateLargestUnit, then
    if (largest_unit != date_largest_unit) {
        // a. Set timeDuration to ? Add24HourDaysToTimeDuration(timeDuration, dateDifference.[[Days]]).
        time_duration = TRY(add_24_hour_days_to_time_duration(vm, time_duration, date_difference.days));

        // b. Set dateDifference.[[Days]] to 0.
        date_difference.days = 0;
    }

    // 11. Return ? CombineDateAndTimeDuration(dateDifference, timeDuration).
    return TRY(combine_date_and_time_duration(vm, date_difference, move(time_duration)));
}

// 5.5.13 DifferencePlainDateTimeWithRounding ( isoDateTime1, isoDateTime2, calendar, largestUnit, roundingIncrement, smallestUnit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-differenceplaindatetimewithrounding
ThrowCompletionOr<InternalDuration> difference_plain_date_time_with_rounding(VM& vm, ISODateTime const& iso_date_time1, ISODateTime const& iso_date_time2, StringView calendar, Unit largest_unit, u64 rounding_increment, Unit smallest_unit, RoundingMode rounding_mode)
{
    // 1. If CompareISODateTime(isoDateTime1, isoDateTime2) = 0, then
    if (compare_iso_date_time(iso_date_time1, iso_date_time2) == 0) {
        // a. Return ! CombineDateAndTimeDuration(ZeroDateDuration(), 0).
        return MUST(combine_date_and_time_duration(vm, zero_date_duration(vm), TimeDuration { 0 }));
    }

    // 2. Let diff be ? DifferenceISODateTime(isoDateTime1, isoDateTime2, calendar, largestUnit).
    auto diff = TRY(difference_iso_date_time(vm, iso_date_time1, iso_date_time2, calendar, largest_unit));

    // 3. If smallestUnit is NANOSECOND and roundingIncrement = 1, return diff.
    if (smallest_unit == Unit::Nanosecond && rounding_increment == 1)
        return diff;

    // 4. Let destEpochNs be GetUTCEpochNanoseconds(isoDateTime2).
    auto dest_epoch_ns = get_utc_epoch_nanoseconds(iso_date_time2);

    // 5. Return ? RoundRelativeDuration(diff, destEpochNs, isoDateTime1, UNSET, calendar, largestUnit, roundingIncrement, smallestUnit, roundingMode).
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

    // 2. Let diff be ? DifferenceISODateTime(isoDateTime1, isoDateTime2, calendar, unit).
    auto diff = TRY(difference_iso_date_time(vm, iso_date_time1, iso_date_time2, calendar, unit));

    // 3. If unit is NANOSECOND, return diff.[[Time]].
    if (unit == Unit::Nanosecond)
        return move(diff.time);

    // 4. Let destEpochNs be GetUTCEpochNanoseconds(isoDateTime2).
    auto dest_epoch_ns = get_utc_epoch_nanoseconds(iso_date_time2);

    // 5. Return ? TotalRelativeDuration(diff, destEpochNs, isoDateTime1, UNSET, calendar, unit).
    return TRY(total_relative_duration(vm, diff, dest_epoch_ns, iso_date_time1, {}, calendar, unit));
}

}
