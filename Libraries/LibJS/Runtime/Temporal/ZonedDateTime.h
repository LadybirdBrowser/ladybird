/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/BigFraction/BigFraction.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/ISORecords.h>

namespace JS::Temporal {

class ZonedDateTime final : public Object {
    JS_OBJECT(ZonedDateTime, Object);
    GC_DECLARE_ALLOCATOR(ZonedDateTime);

public:
    virtual ~ZonedDateTime() override = default;

    [[nodiscard]] GC::Ref<BigInt const> epoch_nanoseconds() const { return m_epoch_nanoseconds; }
    [[nodiscard]] String const& time_zone() const { return m_time_zone; }
    [[nodiscard]] String const& calendar() const { return m_calendar; }

private:
    ZonedDateTime(BigInt const& nanoseconds, String time_zone, String calendar, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    GC::Ref<BigInt const> m_epoch_nanoseconds; // [[EpochNanoseconds]]
    String m_time_zone;                        // [[TimeZone]]
    String m_calendar;                         // [[Calendar]]
};

enum class OffsetBehavior {
    Option,
    Exact,
    Wall,
};

enum class MatchBehavior {
    MatchExactly,
    MatchMinutes,
};

ThrowCompletionOr<Crypto::SignedBigInteger> interpret_iso_date_time_offset(VM&, ISODate, Variant<ParsedISODateTime::StartOfDay, Time> const&, OffsetBehavior, double offset_nanoseconds, StringView time_zone, Disambiguation, OffsetOption, MatchBehavior);
ThrowCompletionOr<GC::Ref<ZonedDateTime>> to_temporal_zoned_date_time(VM&, Value item, Value options = js_undefined());
ThrowCompletionOr<GC::Ref<ZonedDateTime>> create_temporal_zoned_date_time(VM&, BigInt const& epoch_nanoseconds, String time_zone, String calendar, GC::Ptr<FunctionObject> new_target = {});
String temporal_zoned_date_time_to_string(ZonedDateTime const&, SecondsStringPrecision::Precision, ShowCalendar, ShowTimeZoneName, ShowOffset, u64 increment = 1, Unit = Unit::Nanosecond, RoundingMode = RoundingMode::Trunc);
ThrowCompletionOr<Crypto::SignedBigInteger> add_zoned_date_time(VM&, Crypto::SignedBigInteger const& epoch_nanoseconds, StringView time_zone, StringView calendar, InternalDuration const&, Overflow);
ThrowCompletionOr<InternalDuration> difference_zoned_date_time(VM&, Crypto::SignedBigInteger const& nanoseconds1, Crypto::SignedBigInteger const& nanoseconds2, StringView time_zone, StringView calendar, Unit largest_unit);
ThrowCompletionOr<InternalDuration> difference_zoned_date_time_with_rounding(VM&, Crypto::SignedBigInteger const& nanoseconds1, Crypto::SignedBigInteger const& nanoseconds2, StringView time_zone, StringView calendar, Unit largest_unit, u64 rounding_increment, Unit smallest_unit, RoundingMode);
ThrowCompletionOr<Crypto::BigFraction> difference_zoned_date_time_with_total(VM&, Crypto::SignedBigInteger const& nanoseconds1, Crypto::SignedBigInteger const& nanoseconds2, StringView time_zone, StringView calendar, Unit);
ThrowCompletionOr<GC::Ref<Duration>> difference_temporal_zoned_date_time(VM&, DurationOperation, ZonedDateTime const&, Value other, Value options);
ThrowCompletionOr<GC::Ref<ZonedDateTime>> add_duration_to_zoned_date_time(VM&, ArithmeticOperation, ZonedDateTime const&, Value temporal_duration_like, Value options);

}
