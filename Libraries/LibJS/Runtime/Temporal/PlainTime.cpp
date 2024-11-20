/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>

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

// 4.5.4 NoonTimeRecord ( ), https://tc39.es/proposal-temporal/#sec-temporal-noontimerecord
Time noon_time_record()
{
    // 1. Return Time Record { [[Days]]: 0, [[Hour]]: 12, [[Minute]]: 0, [[Second]]: 0, [[Millisecond]]: 0, [[Microsecond]]: 0, [[Nanosecond]]: 0  }.
    return { .days = 0, .hour = 12, .minute = 0, .second = 0, .millisecond = 0, .microsecond = 0, .nanosecond = 0 };
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

}
