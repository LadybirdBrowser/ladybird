/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/ISORecords.h>
#include <LibJS/Runtime/Value.h>

namespace JS::Temporal {

extern Utf16String UTC_TIME_ZONE;

ISODateTime get_iso_parts_from_epoch(Crypto::SignedBigInteger const& epoch_nanoseconds);
Optional<Crypto::SignedBigInteger> get_named_time_zone_next_transition(Utf16View time_zone, Crypto::SignedBigInteger const& epoch_nanoseconds);
Optional<Crypto::SignedBigInteger> get_named_time_zone_previous_transition(Utf16View time_zone, Crypto::SignedBigInteger const& epoch_nanoseconds);
Utf16String format_offset_time_zone_identifier(i64 offset_minutes, Optional<TimeStyle> = {});
Utf16String format_utc_offset_nanoseconds(i64 offset_nanoseconds);
Utf16String format_date_time_utc_offset_rounded(i64 offset_nanoseconds);
ThrowCompletionOr<Utf16String> to_temporal_time_zone_identifier(VM&, Value temporal_time_zone_like);
i64 get_offset_nanoseconds_for(Utf16View time_zone, Crypto::SignedBigInteger const& epoch_nanoseconds);
ISODateTime get_iso_date_time_for(Utf16View time_zone, Crypto::SignedBigInteger const& epoch_nanoseconds);
ThrowCompletionOr<Utf16String> to_temporal_time_zone_identifier(VM&, Utf16View temporal_time_zone_like);
ThrowCompletionOr<Crypto::SignedBigInteger> get_epoch_nanoseconds_for(VM&, Utf16View time_zone, ISODateTime const&, Disambiguation);
ThrowCompletionOr<Crypto::SignedBigInteger> disambiguate_possible_epoch_nanoseconds(VM&, Vector<Crypto::SignedBigInteger> possible_epoch_ns, Utf16View time_zone, ISODateTime const&, Disambiguation);
ThrowCompletionOr<Vector<Crypto::SignedBigInteger>> get_possible_epoch_nanoseconds(VM&, Utf16View time_zone, ISODateTime const&);
ThrowCompletionOr<Crypto::SignedBigInteger> get_start_of_day(VM&, Utf16View time_zone, ISODate);
bool time_zone_equals(Utf16View one, Utf16View two);
ThrowCompletionOr<ParsedTimeZoneIdentifier> parse_time_zone_identifier(VM&, Utf16View identifier);
ParsedTimeZoneIdentifier const& parse_time_zone_identifier(Utf16View identifier);
ParsedTimeZoneIdentifier parse_time_zone_identifier(ParseResult const&);

}
