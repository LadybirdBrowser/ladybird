/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Value.h>
#include <math.h>

namespace JS::Temporal {

// 7.5.10 DurationSign ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-durationsign
i8 duration_sign(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds)
{
    // 1. For each value v of « years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds », do
    for (auto& v : { years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds }) {
        // a. If v < 0, return -1.
        if (v < 0)
            return -1;

        // b. If v > 0, return 1.
        if (v > 0)
            return 1;
    }

    // 2. Return 0.
    return 0;
}

// 7.5.11 IsValidDuration ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidduration
bool is_valid_duration(double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds)
{
    // 1. Let sign be ! DurationSign(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
    auto sign = duration_sign(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds);

    // 2. For each value v of « years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds », do
    for (auto& v : { years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds }) {
        // a. If 𝔽(v) is not finite, return false.
        if (!isfinite(v))
            return false;

        // b. If v < 0 and sign > 0, return false.
        if (v < 0 && sign > 0)
            return false;

        // c. If v > 0 and sign < 0, return false.
        if (v > 0 && sign < 0)
            return false;
    }

    // 3. If abs(years) ≥ 2**32, return false.
    if (AK::fabs(years) > NumericLimits<u32>::max())
        return false;

    // 4. If abs(months) ≥ 2**32, return false.
    if (AK::fabs(months) > NumericLimits<u32>::max())
        return false;

    // 5. If abs(weeks) ≥ 2**32, return false.
    if (AK::fabs(weeks) > NumericLimits<u32>::max())
        return false;

    // 6. Let normalizedSeconds be days × 86,400 + hours × 3600 + minutes × 60 + seconds + ℝ(𝔽(milliseconds)) × 10**-3 + ℝ(𝔽(microseconds)) × 10**-6 + ℝ(𝔽(nanoseconds)) × 10**-9.
    // 7. NOTE: The above step cannot be implemented directly using floating-point arithmetic. Multiplying by 10**-3, 10**-6, and 10**-9 respectively may be imprecise when milliseconds, microseconds, or nanoseconds is an unsafe integer. This multiplication can be implemented in C++ with an implementation of std::remquo() with sufficient bits in the quotient. String manipulation will also give an exact result, since the multiplication is by a power of 10.
    static Crypto::SignedBigInteger days_to_nanoseconds { 8.64e13 };
    static Crypto::SignedBigInteger hours_to_nanoseconds { 3.6e12 };
    static Crypto::SignedBigInteger minutes_to_nanoseconds { 6e10 };
    static Crypto::SignedBigInteger seconds_to_nanoseconds { 1e9 };
    static Crypto::SignedBigInteger milliseconds_to_nanoseconds { 1e6 };
    static Crypto::SignedBigInteger microseconds_to_nanoseconds { 1e3 };

    auto normalized_nanoseconds = Crypto::SignedBigInteger { days }.multiplied_by(days_to_nanoseconds);
    normalized_nanoseconds = normalized_nanoseconds.plus(Crypto::SignedBigInteger { hours }.multiplied_by(hours_to_nanoseconds));
    normalized_nanoseconds = normalized_nanoseconds.plus(Crypto::SignedBigInteger { minutes }.multiplied_by(minutes_to_nanoseconds));
    normalized_nanoseconds = normalized_nanoseconds.plus(Crypto::SignedBigInteger { seconds }.multiplied_by(seconds_to_nanoseconds));
    normalized_nanoseconds = normalized_nanoseconds.plus(Crypto::SignedBigInteger { milliseconds }.multiplied_by(milliseconds_to_nanoseconds));
    normalized_nanoseconds = normalized_nanoseconds.plus(Crypto::SignedBigInteger { microseconds }.multiplied_by(microseconds_to_nanoseconds));
    normalized_nanoseconds = normalized_nanoseconds.plus(Crypto::SignedBigInteger { nanoseconds });

    // 8. If abs(normalizedSeconds) ≥ 2**53, return false.
    static auto maximum_time = Crypto::SignedBigInteger { MAX_ARRAY_LIKE_INDEX }.plus(Crypto::SignedBigInteger { 1 }).multiplied_by(seconds_to_nanoseconds);

    if (normalized_nanoseconds.is_negative())
        normalized_nanoseconds.negate();

    if (normalized_nanoseconds >= maximum_time)
        return false;

    // 3. Return true.
    return true;
}

}
