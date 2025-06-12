/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>

namespace JS::Temporal {

// 3.5.1 ISO Date Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-date-records
struct ISODate {
    i32 year { 0 };
    u8 month { 0 };
    u8 day { 0 };
};

// 4.5.1 Time Records, https://tc39.es/proposal-temporal/#sec-temporal-time-records
struct Time {
    double days { 0 };
    u8 hour { 0 };
    u8 minute { 0 };
    u8 second { 0 };
    u16 millisecond { 0 };
    u16 microsecond { 0 };
    u16 nanosecond { 0 };
};

// 5.5.1 ISO Date-Time Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-date-time-records
struct ISODateTime {
    ISODate iso_date;
    Time time;
};

// 7.5.3 Internal Duration Records, https://tc39.es/proposal-temporal/#sec-temporal-internal-duration-records
// A time duration is an integer in the inclusive interval from -maxTimeDuration to maxTimeDuration, where
// maxTimeDuration = 2**53 × 10**9 - 1 = 9,007,199,254,740,991,999,999,999. It represents the portion of a
// Temporal.Duration object that deals with time units, but as a combined value of total nanoseconds.
using TimeDuration = Crypto::SignedBigInteger;

// 9.5.1 ISO Year-Month Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-year-month-records
struct ISOYearMonth {
    i32 year { 0 };
    u8 month { 0 };
};

// 13.31 ISO String Time Zone Parse Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-string-time-zone-parse-records
struct ParsedISOTimeZone {
    bool z_designator { false };
    Optional<String> offset_string;
    Optional<String> time_zone_annotation;
};

// 13.32 ISO Date-Time Parse Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-date-time-parse-records
struct ParsedISODateTime {
    struct StartOfDay { };

    Optional<i32> year { 0 };
    u8 month { 0 };
    u8 day { 0 };
    Variant<StartOfDay, Time> time;
    ParsedISOTimeZone time_zone;
    Optional<String> calendar;
};

}
