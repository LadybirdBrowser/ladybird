/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>

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

}
