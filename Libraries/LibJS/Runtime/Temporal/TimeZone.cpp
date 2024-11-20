/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/ISO8601.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/VM.h>

namespace JS::Temporal {

// 11.1.5 FormatOffsetTimeZoneIdentifier ( offsetMinutes [ , style ] ), https://tc39.es/proposal-temporal/#sec-temporal-formatoffsettimezoneidentifier
String format_offset_time_zone_identifier(i64 offset_minutes, Optional<TimeStyle> style)
{
    // 1. If offsetMinutes ≥ 0, let sign be the code unit 0x002B (PLUS SIGN); otherwise, let sign be the code unit 0x002D (HYPHEN-MINUS).
    auto sign = offset_minutes >= 0 ? '+' : '-';

    // 2. Let absoluteMinutes be abs(offsetMinutes).
    auto absolute_minutes = abs(offset_minutes);

    // 3. Let hour be floor(absoluteMinutes / 60).
    auto hour = static_cast<u8>(floor(static_cast<double>(absolute_minutes) / 60.0));

    // 4. Let minute be absoluteMinutes modulo 60.
    auto minute = static_cast<u8>(modulo(static_cast<double>(absolute_minutes), 60.0));

    // 5. Let timeString be FormatTimeString(hour, minute, 0, 0, MINUTE, style).
    auto time_string = format_time_string(hour, minute, 0, 0, SecondsStringPrecision::Minute {}, style);

    // 6. Return the string-concatenation of sign and timeString.
    return MUST(String::formatted("{}{}", sign, time_string));
}

// 11.1.8 ToTemporalTimeZoneIdentifier ( temporalTimeZoneLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaltimezoneidentifier
ThrowCompletionOr<String> to_temporal_time_zone_identifier(VM& vm, Value temporal_time_zone_like)
{
    // 1. If temporalTimeZoneLike is an Object, then
    if (temporal_time_zone_like.is_object()) {
        // FIXME: a. If temporalTimeZoneLike has an [[InitializedTemporalZonedDateTime]] internal slot, then
        // FIXME:     i. Return temporalTimeZoneLike.[[TimeZone]].
    }

    // 2. If temporalTimeZoneLike is not a String, throw a TypeError exception.
    if (!temporal_time_zone_like.is_string())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidTimeZoneName, temporal_time_zone_like);

    // 3. Let parseResult be ? ParseTemporalTimeZoneString(temporalTimeZoneLike).
    auto parse_result = TRY(parse_temporal_time_zone_string(vm, temporal_time_zone_like.as_string().utf8_string_view()));

    // 4. Let offsetMinutes be parseResult.[[OffsetMinutes]].
    // 5. If offsetMinutes is not empty, return FormatOffsetTimeZoneIdentifier(offsetMinutes).
    if (parse_result.offset_minutes.has_value())
        return format_offset_time_zone_identifier(*parse_result.offset_minutes);

    // 6. Let name be parseResult.[[Name]].
    // 7. Let timeZoneIdentifierRecord be GetAvailableNamedTimeZoneIdentifier(name).
    auto time_zone_identifier_record = Intl::get_available_named_time_zone_identifier(*parse_result.name);

    // 8. If timeZoneIdentifierRecord is empty, throw a RangeError exception.
    if (!time_zone_identifier_record.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeZoneName, temporal_time_zone_like);

    // 9. Return timeZoneIdentifierRecord.[[Identifier]].
    return time_zone_identifier_record->identifier;
}

// 11.1.16 ParseTimeZoneIdentifier ( identifier ), https://tc39.es/proposal-temporal/#sec-parsetimezoneidentifier
ThrowCompletionOr<TimeZone> parse_time_zone_identifier(VM& vm, StringView identifier)
{
    // 1. Let parseResult be ParseText(StringToCodePoints(identifier), TimeZoneIdentifier).
    auto parse_result = parse_iso8601(Production::TimeZoneIdentifier, identifier);

    // 2. If parseResult is a List of errors, throw a RangeError exception.
    if (!parse_result.has_value())
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeZoneString, identifier);

    return parse_time_zone_identifier(*parse_result);
}

// 11.1.16 ParseTimeZoneIdentifier ( identifier ), https://tc39.es/proposal-temporal/#sec-parsetimezoneidentifier
TimeZone parse_time_zone_identifier(StringView identifier)
{
    // OPTIMIZATION: Some callers can assume that parsing will succeed.

    // 1. Let parseResult be ParseText(StringToCodePoints(identifier), TimeZoneIdentifier).
    auto parse_result = parse_iso8601(Production::TimeZoneIdentifier, identifier);
    VERIFY(parse_result.has_value());

    return parse_time_zone_identifier(*parse_result);
}

// 11.1.16 ParseTimeZoneIdentifier ( identifier ), https://tc39.es/proposal-temporal/#sec-parsetimezoneidentifier
TimeZone parse_time_zone_identifier(ParseResult const& parse_result)
{
    // OPTIMIZATION: Some callers will have already parsed and validated the time zone identifier.

    // 3. If parseResult contains a TimeZoneIANAName Parse Node, then
    if (parse_result.time_zone_iana_name.has_value()) {
        // a. Let name be the source text matched by the TimeZoneIANAName Parse Node contained within parseResult.
        // b. NOTE: name is syntactically valid, but does not necessarily conform to IANA Time Zone Database naming
        //    guidelines or correspond with an available named time zone identifier.
        // c. Return the Record { [[Name]]: CodePointsToString(name), [[OffsetMinutes]]: empty }.
        return TimeZone { .name = String::from_utf8_without_validation(parse_result.time_zone_iana_name->bytes()), .offset_minutes = {} };
    }
    // 4. Else,
    else {
        // a. Assert: parseResult contains a UTCOffset[~SubMinutePrecision] Parse Node.
        VERIFY(parse_result.time_zone_offset.has_value());

        // b. Let offsetString be the source text matched by the UTCOffset[~SubMinutePrecision] Parse Node contained within parseResult.
        // c. Let offsetNanoseconds be ! ParseDateTimeUTCOffset(offsetString).
        // FIXME: ParseTimeZoneOffsetString should be renamed to ParseDateTimeUTCOffset and updated for Temporal.
        auto offset_nanoseconds = parse_time_zone_offset_string(parse_result.time_zone_offset->source_text);

        // d. Let offsetMinutes be offsetNanoseconds / (60 × 10**9).
        auto offset_minutes = offset_nanoseconds / 60'000'000'000;

        // e. Assert: offsetMinutes is an integer.
        VERIFY(trunc(offset_minutes) == offset_minutes);

        // f. Return the Record { [[Name]]: empty, [[OffsetMinutes]]: offsetMinutes }.
        return TimeZone { .name = {}, .offset_minutes = static_cast<i64>(offset_minutes) };
    }
}

}
