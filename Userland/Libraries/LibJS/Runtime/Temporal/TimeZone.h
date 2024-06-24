/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Heap/MarkedVector.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>

namespace JS::Temporal {

class TimeZone final : public Object {
    JS_OBJECT(TimeZone, Object);
    JS_DECLARE_ALLOCATOR(TimeZone);

public:
    // Needs to store values in the range -8.64 * 10^13 to 8.64 * 10^13
    using OffsetType = double;

    virtual ~TimeZone() override = default;

    [[nodiscard]] String const& identifier() const { return m_identifier; }
    [[nodiscard]] Optional<OffsetType> const& offset_nanoseconds() const { return m_offset_nanoseconds; }

    void set_identifier(String identifier) { m_identifier = move(identifier); }
    void set_offset_nanoseconds(OffsetType offset_nanoseconds) { m_offset_nanoseconds = offset_nanoseconds; }

private:
    explicit TimeZone(Object& prototype);

    // 11.5 Properties of Temporal.TimeZone Instances, https://tc39.es/proposal-temporal/#sec-properties-of-temporal-timezone-instances
    String m_identifier;                       // [[Identifier]]
    Optional<OffsetType> m_offset_nanoseconds; // [[OffsetNanoseconds]]
};

bool is_available_time_zone_name(StringView time_zone);
ThrowCompletionOr<String> canonicalize_time_zone_name(VM&, StringView time_zone);
ThrowCompletionOr<TimeZone*> create_temporal_time_zone(VM&, String identifier, FunctionObject const* new_target = nullptr);
ISODateTime get_iso_parts_from_epoch(VM&, Crypto::SignedBigInteger const& epoch_nanoseconds);
BigInt* get_named_time_zone_next_transition(VM&, StringView time_zone_identifier, BigInt const& epoch_nanoseconds);
BigInt* get_named_time_zone_previous_transition(VM&, StringView time_zone_identifier, BigInt const& epoch_nanoseconds);
ThrowCompletionOr<String> format_time_zone_offset_string(VM&, double offset_nanoseconds);
ThrowCompletionOr<String> format_iso_time_zone_offset_string(VM&, double offset_nanoseconds);
ThrowCompletionOr<Object*> to_temporal_time_zone(VM&, Value temporal_time_zone_like);
ThrowCompletionOr<double> get_offset_nanoseconds_for(VM& vm, TimeZoneMethods const& time_zone_record, Instant const& instant);
ThrowCompletionOr<String> builtin_time_zone_get_offset_string_for(VM&, Value time_zone, Instant&);
ThrowCompletionOr<PlainDateTime*> builtin_time_zone_get_plain_date_time_for(VM&, Value time_zone, Instant&, Object& calendar);
ThrowCompletionOr<NonnullGCPtr<Instant>> builtin_time_zone_get_instant_for(VM&, Value time_zone, PlainDateTime&, StringView disambiguation);
ThrowCompletionOr<NonnullGCPtr<Instant>> disambiguate_possible_instants(VM&, MarkedVector<NonnullGCPtr<Instant>> const& possible_instants, TimeZoneMethods const&, PlainDateTime&, StringView disambiguation);
ThrowCompletionOr<MarkedVector<NonnullGCPtr<Instant>>> get_possible_instants_for(VM&, TimeZoneMethods const&, PlainDateTime const&);
ThrowCompletionOr<bool> time_zone_equals(VM&, Object& one, Object& two);

}
