/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/ISORecords.h>
#include <LibJS/Runtime/Value.h>

namespace JS::Temporal {

extern String UTC_TIME_ZONE;

ISODateTime get_iso_parts_from_epoch(Crypto::SignedBigInteger const& epoch_nanoseconds);
Optional<Crypto::SignedBigInteger> get_named_time_zone_next_transition(String const& time_zone, Crypto::SignedBigInteger const& epoch_nanoseconds);
Optional<Crypto::SignedBigInteger> get_named_time_zone_previous_transition(String const& time_zone, Crypto::SignedBigInteger const& epoch_nanoseconds);
String format_offset_time_zone_identifier(i64 offset_minutes, Optional<TimeStyle> = {});
String format_utc_offset_nanoseconds(i64 offset_nanoseconds);
String format_date_time_utc_offset_rounded(i64 offset_nanoseconds);
ThrowCompletionOr<String> to_temporal_time_zone_identifier(VM&, Value temporal_time_zone_like);
i64 get_offset_nanoseconds_for(String const& time_zone, Crypto::SignedBigInteger const& epoch_nanoseconds);
ISODateTime get_iso_date_time_for(String const& time_zone, Crypto::SignedBigInteger const& epoch_nanoseconds);
ThrowCompletionOr<String> to_temporal_time_zone_identifier(VM&, StringView temporal_time_zone_like);
ThrowCompletionOr<Crypto::SignedBigInteger> get_epoch_nanoseconds_for(VM&, String const& time_zone, ISODateTime const&, Disambiguation);
ThrowCompletionOr<Crypto::SignedBigInteger> disambiguate_possible_epoch_nanoseconds(VM&, Vector<Crypto::SignedBigInteger> possible_epoch_ns, String const& time_zone, ISODateTime const&, Disambiguation);
ThrowCompletionOr<Vector<Crypto::SignedBigInteger>> get_possible_epoch_nanoseconds(VM&, String const& time_zone, ISODateTime const&);
ThrowCompletionOr<Crypto::SignedBigInteger> get_start_of_day(VM&, String const& time_zone, ISODate);
bool time_zone_equals(StringView one, StringView two);
ThrowCompletionOr<ParsedTimeZoneIdentifier> parse_time_zone_identifier(VM&, String const& identifier);
ParsedTimeZoneIdentifier const& parse_time_zone_identifier(String const& identifier);
ParsedTimeZoneIdentifier parse_time_zone_identifier(ParseResult const&);

}
