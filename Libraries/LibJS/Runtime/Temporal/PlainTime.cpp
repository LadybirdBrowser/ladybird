/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <math.h>

namespace JS::Temporal {

// FIXME: We should add a generic floor() method to our BigInt classes. But for now, since we know we are only dividing
//        by powers of 10, we can implement a very situationally specific method to compute the floor of a division.
static TimeDuration big_floor(TimeDuration const& numerator, Crypto::UnsignedBigInteger const& denominator)
{
    auto result = numerator.divided_by(denominator);

    if (result.remainder.is_zero())
        return result.quotient;
    if (!result.quotient.is_negative() && result.remainder.is_positive())
        return result.quotient;

    return result.quotient.minus(TimeDuration { 1 });
}

// 4.5.2 CreateTimeRecord ( hour, minute, second, millisecond, microsecond, nanosecond [ , deltaDays ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtimerecord
Time create_time_record(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond, double delta_days)
{
    // 1. If deltaDays is not present, set deltaDays to 0.
    // 2. Assert: IsValidTime(hour, minute, second, millisecond, microsecond, nanosecond).
    VERIFY(is_valid_time(hour, minute, second, millisecond, microsecond, nanosecond));

    // 3. Return Time Record { [[Days]]: deltaDays, [[Hour]]: hour, [[Minute]]: minute, [[Second]]: second, [[Millisecond]]: millisecond, [[Microsecond]]: microsecond, [[Nanosecond]]: nanosecond  }.
    return {
        .days = delta_days,
        .hour = static_cast<u8>(hour),
        .minute = static_cast<u8>(minute),
        .second = static_cast<u8>(second),
        .millisecond = static_cast<u16>(millisecond),
        .microsecond = static_cast<u16>(microsecond),
        .nanosecond = static_cast<u16>(nanosecond),
    };
}

// 4.5.3 MidnightTimeRecord ( ), https://tc39.es/proposal-temporal/#sec-temporal-midnighttimerecord
Time midnight_time_record()
{
    // 1. Return Time Record { [[Days]]: 0, [[Hour]]: 0, [[Minute]]: 0, [[Second]]: 0, [[Millisecond]]: 0, [[Microsecond]]: 0, [[Nanosecond]]: 0  }.
    return { .days = 0, .hour = 0, .minute = 0, .second = 0, .millisecond = 0, .microsecond = 0, .nanosecond = 0 };
}

// 4.5.4 NoonTimeRecord ( ), https://tc39.es/proposal-temporal/#sec-temporal-noontimerecord
Time noon_time_record()
{
    // 1. Return Time Record { [[Days]]: 0, [[Hour]]: 12, [[Minute]]: 0, [[Second]]: 0, [[Millisecond]]: 0, [[Microsecond]]: 0, [[Nanosecond]]: 0  }.
    return { .days = 0, .hour = 12, .minute = 0, .second = 0, .millisecond = 0, .microsecond = 0, .nanosecond = 0 };
}

// 4.5.5 DifferenceTime ( time1, time2 ), https://tc39.es/proposal-temporal/#sec-temporal-differencetime
TimeDuration difference_time(Time const& time1, Time const& time2)
{
    // 1. Let hours be time2.[[Hour]] - time1.[[Hour]].
    auto hours = static_cast<double>(time2.hour) - static_cast<double>(time1.hour);

    // 2. Let minutes be time2.[[Minute]] - time1.[[Minute]].
    auto minutes = static_cast<double>(time2.minute) - static_cast<double>(time1.minute);

    // 3. Let seconds be time2.[[Second]] - time1.[[Second]].
    auto seconds = static_cast<double>(time2.second) - static_cast<double>(time1.second);

    // 4. Let milliseconds be time2.[[Millisecond]] - time1.[[Millisecond]].
    auto milliseconds = static_cast<double>(time2.millisecond) - static_cast<double>(time1.millisecond);

    // 5. Let microseconds be time2.[[Microsecond]] - time1.[[Microsecond]].
    auto microseconds = static_cast<double>(time2.microsecond) - static_cast<double>(time1.microsecond);

    // 6. Let nanoseconds be time2.[[Nanosecond]] - time1.[[Nanosecond]].
    auto nanoseconds = static_cast<double>(time2.nanosecond) - static_cast<double>(time1.nanosecond);

    // 7. Let timeDuration be TimeDurationFromComponents(hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
    auto time_duration = time_duration_from_components(hours, minutes, seconds, milliseconds, microseconds, nanoseconds);

    // 8. Assert: abs(timeDuration) < nsPerDay.
    VERIFY(time_duration.unsigned_value() < NANOSECONDS_PER_DAY);

    // 9. Return timeDuration.
    return time_duration;
}

// 4.5.8 RegulateTime ( hour, minute, second, millisecond, microsecond, nanosecond, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-regulatetime
ThrowCompletionOr<Time> regulate_time(VM& vm, double hour, double minute, double second, double millisecond, double microsecond, double nanosecond, Overflow overflow)
{
    switch (overflow) {
    // 1. If overflow is CONSTRAIN, then
    case Overflow::Constrain:
        // a. Set hour to the result of clamping hour between 0 and 23.
        hour = clamp(hour, 0, 23);

        // b. Set minute to the result of clamping minute between 0 and 59.
        minute = clamp(minute, 0, 59);

        // c. Set second to the result of clamping second between 0 and 59.
        second = clamp(second, 0, 59);

        // d. Set millisecond to the result of clamping millisecond between 0 and 999.
        millisecond = clamp(millisecond, 0, 999);

        // e. Set microsecond to the result of clamping microsecond between 0 and 999.
        microsecond = clamp(microsecond, 0, 999);

        // f. Set nanosecond to the result of clamping nanosecond between 0 and 999.
        nanosecond = clamp(nanosecond, 0, 999);

        break;

    // 2. Else,
    case Overflow::Reject:
        // a. Assert: overflow is REJECT.
        // b. If IsValidTime(hour, minute, second, millisecond, microsecond, nanosecond) is false, throw a RangeError exception.
        if (!is_valid_time(hour, minute, second, millisecond, microsecond, nanosecond))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainTime);

        break;
    }

    // 3. Return CreateTimeRecord(hour, minute, second, millisecond, microsecond,nanosecond).
    return create_time_record(hour, minute, second, millisecond, microsecond, nanosecond);
}

// 4.5.9 IsValidTime ( hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidtime
bool is_valid_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond)
{
    // 1. If hour < 0 or hour > 23, then
    if (hour < 0 || hour > 23) {
        // a. Return false.
        return false;
    }

    // 2. If minute < 0 or minute > 59, then
    if (minute < 0 || minute > 59) {
        // a. Return false.
        return false;
    }

    // 3. If second < 0 or second > 59, then
    if (second < 0 || second > 59) {
        // a. Return false.
        return false;
    }

    // 4. If millisecond < 0 or millisecond > 999, then
    if (millisecond < 0 || millisecond > 999) {
        // a. Return false.
        return false;
    }

    // 5. If microsecond < 0 or microsecond > 999, then
    if (microsecond < 0 || microsecond > 999) {
        // a. Return false.
        return false;
    }

    // 6. If nanosecond < 0 or nanosecond > 999, then
    if (nanosecond < 0 || nanosecond > 999) {
        // a. Return false.
        return false;
    }

    // 7. Return true.
    return true;
}

// 4.5.10 BalanceTime ( hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/proposal-temporal/#sec-temporal-balancetime
Time balance_time(double hour, double minute, double second, double millisecond, double microsecond, double nanosecond)
{
    // 1. Set microsecond to microsecond + floor(nanosecond / 1000).
    microsecond += floor(nanosecond / 1000.0);

    // 2. Set nanosecond to nanosecond modulo 1000.
    nanosecond = modulo(nanosecond, 1000.0);

    // 3. Set millisecond to millisecond + floor(microsecond / 1000).
    millisecond += floor(microsecond / 1000.0);

    // 4. Set microsecond to microsecond modulo 1000.
    microsecond = modulo(microsecond, 1000.0);

    // 5. Set second to second + floor(millisecond / 1000).
    second += floor(millisecond / 1000.0);

    // 6. Set millisecond to millisecond modulo 1000.
    millisecond = modulo(millisecond, 1000.0);

    // 7. Set minute to minute + floor(second / 60).
    minute += floor(second / 60.0);

    // 8. Set second to second modulo 60.
    second = modulo(second, 60.0);

    // 9. Set hour to hour + floor(minute / 60).
    hour += floor(minute / 60.0);

    // 10. Set minute to minute modulo 60.
    minute = modulo(minute, 60.0);

    // 11. Let deltaDays be floor(hour / 24).
    auto delta_days = floor(hour / 24.0);

    // 12. Set hour to hour modulo 24.
    hour = modulo(hour, 24.0);

    // 13. Return CreateTimeRecord(hour, minute, second, millisecond, microsecond, nanosecond, deltaDays).
    return create_time_record(hour, minute, second, millisecond, microsecond, nanosecond, delta_days);
}

// 4.5.10 BalanceTime ( hour, minute, second, millisecond, microsecond, nanosecond ), https://tc39.es/proposal-temporal/#sec-temporal-balancetime
Time balance_time(double hour, double minute, double second, double millisecond, double microsecond, TimeDuration const& nanosecond_value)
{
    // 1. Set microsecond to microsecond + floor(nanosecond / 1000).
    auto microsecond_value = TimeDuration { microsecond }.plus(big_floor(nanosecond_value, NANOSECONDS_PER_MICROSECOND));

    // 2. Set nanosecond to nanosecond modulo 1000.
    auto nanosecond = modulo(nanosecond_value, NANOSECONDS_PER_MICROSECOND).to_double();

    // 3. Set millisecond to millisecond + floor(microsecond / 1000).
    auto millisecond_value = TimeDuration { millisecond }.plus(big_floor(microsecond_value, MICROSECONDS_PER_MILLISECOND));

    // 4. Set microsecond to microsecond modulo 1000.
    microsecond = modulo(microsecond_value, MICROSECONDS_PER_MILLISECOND).to_double();

    // 5. Set second to second + floor(millisecond / 1000).
    auto second_value = TimeDuration { second }.plus(big_floor(millisecond_value, MILLISECONDS_PER_SECOND));

    // 6. Set millisecond to millisecond modulo 1000.
    millisecond = modulo(millisecond_value, MILLISECONDS_PER_SECOND).to_double();

    // 7. Set minute to minute + floor(second / 60).
    auto minute_value = TimeDuration { minute }.plus(big_floor(second_value, SECONDS_PER_MINUTE));

    // 8. Set second to second modulo 60.
    second = modulo(second_value, SECONDS_PER_MINUTE).to_double();

    // 9. Set hour to hour + floor(minute / 60).
    auto hour_value = TimeDuration { hour }.plus(big_floor(minute_value, MINUTES_PER_HOUR));

    // 10. Set minute to minute modulo 60.
    minute = modulo(minute_value, MINUTES_PER_HOUR).to_double();

    // 11. Let deltaDays be floor(hour / 24).
    auto delta_days = big_floor(hour_value, HOURS_PER_DAY).to_double();

    // 12. Set hour to hour modulo 24.
    hour = modulo(hour_value, HOURS_PER_DAY).to_double();

    // 13. Return CreateTimeRecord(hour, minute, second, millisecond, microsecond, nanosecond, deltaDays).
    return create_time_record(hour, minute, second, millisecond, microsecond, nanosecond, delta_days);
}

// 4.5.14 CompareTimeRecord ( time1, time2 ), https://tc39.es/proposal-temporal/#sec-temporal-comparetimerecord
i8 compare_time_record(Time const& time1, Time const& time2)
{
    // 1. If time1.[[Hour]] > time2.[[Hour]], return 1.
    if (time1.hour > time2.hour)
        return 1;
    // 2. If time1.[[Hour]] < time2.[[Hour]], return -1.
    if (time1.hour < time2.hour)
        return -1;

    // 3. If time1.[[Minute]] > time2.[[Minute]], return 1.
    if (time1.minute > time2.minute)
        return 1;
    // 4. If time1.[[Minute]] < time2.[[Minute]], return -1.
    if (time1.minute < time2.minute)
        return -1;

    // 5. If time1.[[Second]] > time2.[[Second]], return 1.
    if (time1.second > time2.second)
        return 1;
    // 6. If time1.[[Second]] < time2.[[Second]], return -1.
    if (time1.second < time2.second)
        return -1;

    // 7. If time1.[[Millisecond]] > time2.[[Millisecond]], return 1.
    if (time1.millisecond > time2.millisecond)
        return 1;
    // 8. If time1.[[Millisecond]] < time2.[[Millisecond]], return -1.
    if (time1.millisecond < time2.millisecond)
        return -1;

    // 9. If time1.[[Microsecond]] > time2.[[Microsecond]], return 1.
    if (time1.microsecond > time2.microsecond)
        return 1;
    // 10. If time1.[[Microsecond]] < time2.[[Microsecond]], return -1.
    if (time1.microsecond < time2.microsecond)
        return -1;

    // 11. If time1.[[Nanosecond]] > time2.[[Nanosecond]], return 1.
    if (time1.nanosecond > time2.nanosecond)
        return 1;
    // 12. If time1.[[Nanosecond]] < time2.[[Nanosecond]], return -1.
    if (time1.nanosecond < time2.nanosecond)
        return -1;

    // 13. Return 0.
    return 0;
}

// 4.5.15 AddTime ( time, timeDuration ), https://tc39.es/proposal-temporal/#sec-temporal-addtime
Time add_time(Time const& time, TimeDuration const& time_duration)
{
    auto nanoseconds = time_duration.plus(TimeDuration { static_cast<i64>(time.nanosecond) });

    // 1. Return BalanceTime(time.[[Hour]], time.[[Minute]], time.[[Second]], time.[[Millisecond]], time.[[Microsecond]], time.[[Nanosecond]] + timeDuration).
    return balance_time(time.hour, time.minute, time.second, time.millisecond, time.microsecond, nanoseconds);
}

}
