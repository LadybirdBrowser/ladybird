/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>

namespace JS::Temporal {

// https://tc39.es/proposal-temporal/#eqn-nsMaxInstant
extern Crypto::SignedBigInteger const NANOSECONDS_MAX_INSTANT;

// https://tc39.es/proposal-temporal/#eqn-nsMinInstant
extern Crypto::SignedBigInteger const NANOSECONDS_MIN_INSTANT;

// https://tc39.es/proposal-temporal/#eqn-nsPerDay
extern Crypto::UnsignedBigInteger const NANOSECONDS_PER_DAY;

// Non-standard:
extern Crypto::UnsignedBigInteger const NANOSECONDS_PER_HOUR;
extern Crypto::UnsignedBigInteger const NANOSECONDS_PER_MINUTE;
extern Crypto::UnsignedBigInteger const NANOSECONDS_PER_SECOND;
extern Crypto::UnsignedBigInteger const NANOSECONDS_PER_MILLISECOND;
extern Crypto::UnsignedBigInteger const NANOSECONDS_PER_MICROSECOND;
extern Crypto::UnsignedBigInteger const NANOSECONDS_PER_NANOSECOND;

extern Crypto::UnsignedBigInteger const MICROSECONDS_PER_MILLISECOND;
extern Crypto::UnsignedBigInteger const MILLISECONDS_PER_SECOND;
extern Crypto::UnsignedBigInteger const SECONDS_PER_MINUTE;
extern Crypto::UnsignedBigInteger const MINUTES_PER_HOUR;
extern Crypto::UnsignedBigInteger const HOURS_PER_DAY;

}
