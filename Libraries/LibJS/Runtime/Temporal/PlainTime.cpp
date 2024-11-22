/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <math.h>

namespace JS::Temporal {

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

}
