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
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>

namespace JS::Temporal {

class Instant final : public Object {
    JS_OBJECT(Instant, Object);
    GC_DECLARE_ALLOCATOR(Instant);

public:
    virtual ~Instant() override = default;

    [[nodiscard]] GC::Ref<BigInt const> epoch_nanoseconds() const { return m_epoch_nanoseconds; }

private:
    Instant(BigInt const& epoch_nanoseconds, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    GC::Ref<BigInt const> m_epoch_nanoseconds; // [[EpochNanoseconds]]
};

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

bool is_valid_epoch_nanoseconds(Crypto::SignedBigInteger const& epoch_nanoseconds);
ThrowCompletionOr<GC::Ref<Instant>> create_temporal_instant(VM&, BigInt const& epoch_nanoseconds, GC::Ptr<FunctionObject> new_target = {});
ThrowCompletionOr<GC::Ref<Instant>> to_temporal_instant(VM&, Value item);
i8 compare_epoch_nanoseconds(Crypto::SignedBigInteger const& epoch_nanoseconds_one, Crypto::SignedBigInteger const& epoch_nanoseconds_two);
ThrowCompletionOr<Crypto::SignedBigInteger> add_instant(VM&, Crypto::SignedBigInteger const& epoch_nanoseconds, TimeDuration const&);
InternalDuration difference_instant(VM&, Crypto::SignedBigInteger const& nanoseconds1, Crypto::SignedBigInteger const& nanoseconds2, u64 rounding_increment, Unit smallest_unit, RoundingMode);
Crypto::SignedBigInteger round_temporal_instant(Crypto::SignedBigInteger const& nanoseconds, u64 increment, Unit, RoundingMode);
String temporal_instant_to_string(Instant const&, Optional<StringView> time_zone, SecondsStringPrecision::Precision);
ThrowCompletionOr<GC::Ref<Duration>> difference_temporal_instant(VM&, DurationOperation, Instant const&, Value other, Value options);
ThrowCompletionOr<GC::Ref<Instant>> add_duration_to_instant(VM&, ArithmeticOperation, Instant const&, Value temporal_duration_like);

}
