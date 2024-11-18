/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/Instant.h>

namespace JS::Temporal {

// nsMaxInstant = 10**8 × nsPerDay = 8.64 × 10**21
Crypto::SignedBigInteger const NANOSECONDS_MAX_INSTANT = "8640000000000000000000"_sbigint;

// nsMinInstant = -nsMaxInstant = -8.64 × 10**21
Crypto::SignedBigInteger const NANOSECONDS_MIN_INSTANT = "-8640000000000000000000"_sbigint;

// nsPerDay = 10**6 × ℝ(msPerDay) = 8.64 × 10**13
Crypto::UnsignedBigInteger const NANOSECONDS_PER_DAY = 86400000000000_bigint;

// Non-standard:
Crypto::UnsignedBigInteger const NANOSECONDS_PER_HOUR = 3600000000000_bigint;
Crypto::UnsignedBigInteger const NANOSECONDS_PER_MINUTE = 60000000000_bigint;
Crypto::UnsignedBigInteger const NANOSECONDS_PER_SECOND = 1000000000_bigint;
Crypto::UnsignedBigInteger const NANOSECONDS_PER_MILLISECOND = 1000000_bigint;
Crypto::UnsignedBigInteger const NANOSECONDS_PER_MICROSECOND = 1000_bigint;
Crypto::UnsignedBigInteger const NANOSECONDS_PER_NANOSECOND = 1_bigint;

Crypto::UnsignedBigInteger const MICROSECONDS_PER_MILLISECOND = 1000_bigint;
Crypto::UnsignedBigInteger const MILLISECONDS_PER_SECOND = 1000_bigint;
Crypto::UnsignedBigInteger const SECONDS_PER_MINUTE = 60_bigint;
Crypto::UnsignedBigInteger const MINUTES_PER_HOUR = 60_bigint;
Crypto::UnsignedBigInteger const HOURS_PER_DAY = 24_bigint;

}
