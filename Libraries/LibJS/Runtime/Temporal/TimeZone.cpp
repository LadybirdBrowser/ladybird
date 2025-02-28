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
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/DateEquations.h>
#include <LibJS/Runtime/Temporal/ISO8601.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibJS/Runtime/VM.h>

namespace JS::Temporal {

// 11.1.2 GetISOPartsFromEpoch ( epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-getisopartsfromepoch
ISODateTime get_iso_parts_from_epoch(Crypto::SignedBigInteger const& epoch_nanoseconds)
{
    // 1. Assert: IsValidEpochNanoseconds(ℤ(epochNanoseconds)) is true.
    VERIFY(is_valid_epoch_nanoseconds(epoch_nanoseconds));

    // 2. Let remainderNs be epochNanoseconds modulo 10**6.
    auto remainder_nanoseconds = modulo(epoch_nanoseconds, NANOSECONDS_PER_MILLISECOND);
    auto remainder_nanoseconds_value = remainder_nanoseconds.to_double();

    // 3. Let epochMilliseconds be 𝔽((epochNanoseconds - remainderNs) / 10**6).
    auto epoch_milliseconds = epoch_nanoseconds.minus(remainder_nanoseconds).divided_by(NANOSECONDS_PER_MILLISECOND).quotient.to_double();

    // 4. Let year be EpochTimeToEpochYear(epochMilliseconds).
    auto year = epoch_time_to_epoch_year(epoch_milliseconds);

    // 5. Let month be EpochTimeToMonthInYear(epochMilliseconds) + 1.
    auto month = epoch_time_to_month_in_year(epoch_milliseconds) + 1;

    // 6. Let day be EpochTimeToDate(epochMilliseconds).
    auto day = epoch_time_to_date(epoch_milliseconds);

    // 7. Let hour be ℝ(HourFromTime(epochMilliseconds)).
    auto hour = hour_from_time(epoch_milliseconds);

    // 8. Let minute be ℝ(MinFromTime(epochMilliseconds)).
    auto minute = min_from_time(epoch_milliseconds);

    // 9. Let second be ℝ(SecFromTime(epochMilliseconds)).
    auto second = sec_from_time(epoch_milliseconds);

    // 10. Let millisecond be ℝ(msFromTime(epochMilliseconds)).
    auto millisecond = ms_from_time(epoch_milliseconds);

    // 11. Let microsecond be floor(remainderNs / 1000).
    auto microsecond = floor(remainder_nanoseconds_value / 1000.0);

    // 12. Assert: microsecond < 1000.
    VERIFY(microsecond < 1000.0);

    // 13. Let nanosecond be remainderNs modulo 1000.
    auto nanosecond = modulo(remainder_nanoseconds_value, 1000.0);

    // 14. Let isoDate be CreateISODateRecord(year, month, day).
    auto iso_date = create_iso_date_record(year, month, day);

    // 15. Let time be CreateTimeRecord(hour, minute, second, millisecond, microsecond, nanosecond).
    auto time = create_time_record(hour, minute, second, millisecond, microsecond, nanosecond);

    // 16. Return CombineISODateAndTimeRecord(isoDate, time).
    return combine_iso_date_and_time_record(iso_date, time);
}

// 11.1.3 GetNamedTimeZoneNextTransition ( timeZoneIdentifier, epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-getnamedtimezonenexttransition
Optional<Crypto::SignedBigInteger> get_named_time_zone_next_transition(StringView time_zone, Crypto::SignedBigInteger const& epoch_nanoseconds)
{
    // FIXME: Implement this AO.
    (void)time_zone;
    (void)epoch_nanoseconds;

    return {};
}

// 11.1.4 GetNamedTimeZonePreviousTransition ( timeZoneIdentifier, epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-getnamedtimezoneprevioustransition
Optional<Crypto::SignedBigInteger> get_named_time_zone_previous_transition(StringView time_zone, Crypto::SignedBigInteger const& epoch_nanoseconds)
{
    // FIXME: Implement this AO.
    (void)time_zone;
    (void)epoch_nanoseconds;

    return {};
}

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

// 11.1.6 FormatUTCOffsetNanoseconds ( offsetNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-formatutcoffsetnanoseconds
String format_utc_offset_nanoseconds(i64 offset_nanoseconds)
{
    // 1. If offsetNanoseconds ≥ 0, let sign be the code unit 0x002B (PLUS SIGN); otherwise, let sign be the code unit 0x002D (HYPHEN-MINUS).
    auto sign = offset_nanoseconds >= 0 ? '+' : '-';

    // 2. Let absoluteNanoseconds be abs(offsetNanoseconds).
    auto absolute_nanoseconds = static_cast<double>(abs(offset_nanoseconds));

    // 3. Let hour be floor(absoluteNanoseconds / (3600 × 10**9)).
    auto hour = floor(absolute_nanoseconds / 3'600'000'000'000.0);

    // 4. Let minute be floor(absoluteNanoseconds / (60 × 10**9)) modulo 60.
    auto minute = modulo(floor(absolute_nanoseconds / 60'000'000'000.0), 60.0);

    // 5. Let second be floor(absoluteNanoseconds / 10**9) modulo 60.
    auto second = modulo(floor(absolute_nanoseconds / 1'000'000'000.0), 60.0);

    // 6. Let subSecondNanoseconds be absoluteNanoseconds modulo 10**9.
    auto sub_second_nanoseconds = modulo(absolute_nanoseconds, 1'000'000'000.0);

    // 7. If second = 0 and subSecondNanoseconds = 0, let precision be MINUTE; otherwise, let precision be AUTO.
    SecondsStringPrecision::Precision precision { Auto {} };
    if (second == 0 && sub_second_nanoseconds == 0)
        precision = SecondsStringPrecision::Minute {};

    // 8. Let timeString be FormatTimeString(hour, minute, second, subSecondNanoseconds, precision).
    auto time_string = format_time_string(hour, minute, second, sub_second_nanoseconds, precision);

    // 9. Return the string-concatenation of sign and timeString.
    return MUST(String::formatted("{}{}", sign, time_string));
}

// 11.1.7 FormatDateTimeUTCOffsetRounded ( offsetNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-formatdatetimeutcoffsetrounded
String format_date_time_utc_offset_rounded(i64 offset_nanoseconds)
{
    // 1. Set offsetNanoseconds to RoundNumberToIncrement(offsetNanoseconds, 60 × 10**9, HALF-EXPAND).
    auto offset_nanoseconds_value = round_number_to_increment(static_cast<double>(offset_nanoseconds), 60'000'000'000, RoundingMode::HalfExpand);

    // 2. Let offsetMinutes be offsetNanoseconds / (60 × 10**9).
    auto offset_minutes = offset_nanoseconds_value / 60'000'000'000;

    // 3. Assert: offsetMinutes is an integer.
    VERIFY(trunc(offset_minutes) == offset_minutes);

    // 4. Return FormatOffsetTimeZoneIdentifier(offsetMinutes).
    return format_offset_time_zone_identifier(static_cast<i64>(offset_minutes));
}

// 11.1.8 ToTemporalTimeZoneIdentifier ( temporalTimeZoneLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaltimezoneidentifier
ThrowCompletionOr<String> to_temporal_time_zone_identifier(VM& vm, Value temporal_time_zone_like)
{
    // 1. If temporalTimeZoneLike is an Object, then
    if (temporal_time_zone_like.is_object()) {
        auto const& object = temporal_time_zone_like.as_object();

        // a. If temporalTimeZoneLike has an [[InitializedTemporalZonedDateTime]] internal slot, then
        if (is<ZonedDateTime>(object)) {
            // i. Return temporalTimeZoneLike.[[TimeZone]].
            return static_cast<ZonedDateTime const&>(object).time_zone();
        }
    }

    // 2. If temporalTimeZoneLike is not a String, throw a TypeError exception.
    if (!temporal_time_zone_like.is_string())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidTimeZoneName, temporal_time_zone_like);

    return to_temporal_time_zone_identifier(vm, temporal_time_zone_like.as_string().utf8_string_view());
}

// 11.1.8 ToTemporalTimeZoneIdentifier ( temporalTimeZoneLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaltimezoneidentifier
ThrowCompletionOr<String> to_temporal_time_zone_identifier(VM& vm, StringView temporal_time_zone_like)
{
    // 3. Let parseResult be ? ParseTemporalTimeZoneString(temporalTimeZoneLike).
    auto parse_result = TRY(parse_temporal_time_zone_string(vm, temporal_time_zone_like));

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

// 11.1.9 GetOffsetNanosecondsFor ( timeZone, epochNs ), https://tc39.es/proposal-temporal/#sec-temporal-getoffsetnanosecondsfor
i64 get_offset_nanoseconds_for(StringView time_zone, Crypto::SignedBigInteger const& epoch_nanoseconds)
{
    // 1. Let parseResult be ! ParseTimeZoneIdentifier(timeZone).
    auto parse_result = parse_time_zone_identifier(time_zone);

    // 2. If parseResult.[[OffsetMinutes]] is not empty, return parseResult.[[OffsetMinutes]] × (60 × 10**9).
    if (parse_result.offset_minutes.has_value())
        return *parse_result.offset_minutes * 60'000'000'000;

    // 3. Return GetNamedTimeZoneOffsetNanoseconds(parseResult.[[Name]], epochNs).
    return get_named_time_zone_offset_nanoseconds(*parse_result.name, epoch_nanoseconds).offset.to_nanoseconds();
}

// 11.1.10 GetISODateTimeFor ( timeZone, epochNs ), https://tc39.es/proposal-temporal/#sec-temporal-getisodatetimefor
ISODateTime get_iso_date_time_for(StringView time_zone, Crypto::SignedBigInteger const& epoch_nanoseconds)
{
    // 1. Let offsetNanoseconds be GetOffsetNanosecondsFor(timeZone, epochNs).
    auto offset_nanoseconds = get_offset_nanoseconds_for(time_zone, epoch_nanoseconds);

    // 2. Let result be GetISOPartsFromEpoch(ℝ(epochNs)).
    auto result = get_iso_parts_from_epoch(epoch_nanoseconds);

    // 3. Return BalanceISODateTime(result.[[ISODate]].[[Year]], result.[[ISODate]].[[Month]], result.[[ISODate]].[[Day]], result.[[Time]].[[Hour]], result.[[Time]].[[Minute]], result.[[Time]].[[Second]], result.[[Time]].[[Millisecond]], result.[[Time]].[[Microsecond]], result.[[Time]].[[Nanosecond]] + offsetNanoseconds).
    return balance_iso_date_time(result.iso_date.year, result.iso_date.month, result.iso_date.day, result.time.hour, result.time.minute, result.time.second, result.time.millisecond, result.time.microsecond, static_cast<double>(result.time.nanosecond) + static_cast<double>(offset_nanoseconds));
}

// 11.1.11 GetEpochNanosecondsFor ( timeZone, isoDateTime, disambiguation ), https://tc39.es/proposal-temporal/#sec-temporal-getepochnanosecondsfor
ThrowCompletionOr<Crypto::SignedBigInteger> get_epoch_nanoseconds_for(VM& vm, StringView time_zone, ISODateTime const& iso_date_time, Disambiguation disambiguation)
{
    // 1. Let possibleEpochNs be ? GetPossibleEpochNanoseconds(timeZone, isoDateTime).
    auto possible_epoch_ns = TRY(get_possible_epoch_nanoseconds(vm, time_zone, iso_date_time));

    // 2. Return ? DisambiguatePossibleEpochNanoseconds(possibleEpochNs, timeZone, isoDateTime, disambiguation).
    return TRY(disambiguate_possible_epoch_nanoseconds(vm, move(possible_epoch_ns), time_zone, iso_date_time, disambiguation));
}

// 11.1.12 DisambiguatePossibleEpochNanoseconds ( possibleEpochNs, timeZone, isoDateTime, disambiguation ), https://tc39.es/proposal-temporal/#sec-temporal-disambiguatepossibleepochnanoseconds
ThrowCompletionOr<Crypto::SignedBigInteger> disambiguate_possible_epoch_nanoseconds(VM& vm, Vector<Crypto::SignedBigInteger> possible_epoch_ns, StringView time_zone, ISODateTime const& iso_date_time, Disambiguation disambiguation)
{
    // 1. Let n be possibleEpochNs's length.
    auto n = possible_epoch_ns.size();

    // 2. If n = 1, then
    if (n == 1) {
        // a. Return possibleEpochNs[0].
        return move(possible_epoch_ns[0]);
    }

    // 3. If n ≠ 0, then
    if (n != 0) {
        // a. If disambiguation is EARLIER or COMPATIBLE, then
        if (disambiguation == Disambiguation::Earlier || disambiguation == Disambiguation::Compatible) {
            // i. Return possibleEpochNs[0].
            return move(possible_epoch_ns[0]);
        }

        // b. If disambiguation is LATER, then
        if (disambiguation == Disambiguation::Later) {
            // i. Return possibleEpochNs[n - 1].
            return move(possible_epoch_ns[n - 1]);
        }

        // c. Assert: disambiguation is REJECT.
        VERIFY(disambiguation == Disambiguation::Reject);

        // d. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalDisambiguatePossibleEpochNSRejectMoreThanOne);
    }

    // 4. Assert: n = 0.
    VERIFY(n == 0);

    // 5. If disambiguation is REJECT, then
    if (disambiguation == Disambiguation::Reject) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalDisambiguatePossibleEpochNSRejectZero);
    }

    // FIXME: GetNamedTimeZoneEpochNanoseconds currently does not produce zero instants.
    (void)time_zone;
    (void)iso_date_time;
    TODO();
}

// 11.1.13 GetPossibleEpochNanoseconds ( timeZone, isoDateTime ), https://tc39.es/proposal-temporal/#sec-temporal-getpossibleepochnanoseconds
ThrowCompletionOr<Vector<Crypto::SignedBigInteger>> get_possible_epoch_nanoseconds(VM& vm, StringView time_zone, ISODateTime const& iso_date_time)
{
    Vector<Crypto::SignedBigInteger> possible_epoch_nanoseconds;

    // 1. Let parseResult be ! ParseTimeZoneIdentifier(timeZone).
    auto parse_result = parse_time_zone_identifier(time_zone);

    // 2. If parseResult.[[OffsetMinutes]] is not empty, then
    if (parse_result.offset_minutes.has_value()) {
        // a. Let balanced be BalanceISODateTime(isoDateTime.[[ISODate]].[[Year]], isoDateTime.[[ISODate]].[[Month]], isoDateTime.[[ISODate]].[[Day]], isoDateTime.[[Time]].[[Hour]], isoDateTime.[[Time]].[[Minute]] - parseResult.[[OffsetMinutes]], isoDateTime.[[Time]].[[Second]], isoDateTime.[[Time]].[[Millisecond]], isoDateTime.[[Time]].[[Microsecond]], isoDateTime.[[Time]].[[Nanosecond]]).
        auto balanced = balance_iso_date_time(
            iso_date_time.iso_date.year,
            iso_date_time.iso_date.month,
            iso_date_time.iso_date.day,
            iso_date_time.time.hour,
            static_cast<double>(iso_date_time.time.minute) - static_cast<double>(*parse_result.offset_minutes),
            iso_date_time.time.second,
            iso_date_time.time.millisecond,
            iso_date_time.time.microsecond,
            iso_date_time.time.nanosecond);

        // b. Perform ? CheckISODaysRange(balanced.[[ISODate]]).
        TRY(check_iso_days_range(vm, balanced.iso_date));

        // c. Let epochNanoseconds be GetUTCEpochNanoseconds(balanced).
        auto epoch_nanoseconds = get_utc_epoch_nanoseconds(balanced);

        // d. Let possibleEpochNanoseconds be « epochNanoseconds ».
        possible_epoch_nanoseconds.append(move(epoch_nanoseconds));
    }
    // 3. Else,
    else {
        // a. Perform ? CheckISODaysRange(isoDateTime.[[ISODate]]).
        TRY(check_iso_days_range(vm, iso_date_time.iso_date));

        // b. Let possibleEpochNanoseconds be GetNamedTimeZoneEpochNanoseconds(parseResult.[[Name]], isoDateTime).
        possible_epoch_nanoseconds = get_named_time_zone_epoch_nanoseconds(*parse_result.name, iso_date_time);
    }

    // 4. For each value epochNanoseconds in possibleEpochNanoseconds, do
    for (auto const& epoch_nanoseconds : possible_epoch_nanoseconds) {
        // a. If IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
        if (!is_valid_epoch_nanoseconds(epoch_nanoseconds))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);
    }

    // 5. Return possibleEpochNanoseconds.
    return possible_epoch_nanoseconds;
}

// 11.1.14 GetStartOfDay ( timeZone, isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-getstartofday
ThrowCompletionOr<Crypto::SignedBigInteger> get_start_of_day(VM& vm, StringView time_zone, ISODate iso_date)
{
    // 1. Let isoDateTime be CombineISODateAndTimeRecord(isoDate, MidnightTimeRecord()).
    auto iso_date_time = combine_iso_date_and_time_record(iso_date, midnight_time_record());

    // 2. Let possibleEpochNs be ? GetPossibleEpochNanoseconds(timeZone, isoDateTime).
    auto possible_epoch_nanoseconds = TRY(get_possible_epoch_nanoseconds(vm, time_zone, iso_date_time));

    // 3. If possibleEpochNs is not empty, return possibleEpochNs[0].
    if (!possible_epoch_nanoseconds.is_empty())
        return move(possible_epoch_nanoseconds[0]);

    // FIXME: GetNamedTimeZoneEpochNanoseconds currently does not produce zero instants.
    TODO();
}

// 11.1.15 TimeZoneEquals ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-timezoneequals
bool time_zone_equals(StringView one, StringView two)
{
    // 1. If one is two, return true.
    if (one == two)
        return true;

    // 2. Let offsetMinutesOne be ! ParseTimeZoneIdentifier(one).[[OffsetMinutes]].
    auto offset_minutes_one = parse_time_zone_identifier(one).offset_minutes;

    // 3. Let offsetMinutesTwo be ! ParseTimeZoneIdentifier(two).[[OffsetMinutes]].
    auto offset_minutes_two = parse_time_zone_identifier(two).offset_minutes;

    // 4. If offsetMinutesOne is EMPTY and offsetMinutesTwo is EMPTY, then
    if (!offset_minutes_one.has_value() && !offset_minutes_two.has_value()) {
        // a. Let recordOne be GetAvailableNamedTimeZoneIdentifier(one).
        auto record_one = Intl::get_available_named_time_zone_identifier(one);

        // b. Let recordTwo be GetAvailableNamedTimeZoneIdentifier(two).
        auto record_two = Intl::get_available_named_time_zone_identifier(two);

        // c. If recordOne is not EMPTY and recordTwo is not EMPTY and recordOne.[[PrimaryIdentifier]] is
        //    recordTwo.[[PrimaryIdentifier]], return true.
        if (record_one.has_value() && record_two.has_value()) {
            if (record_one->primary_identifier == record_two->primary_identifier)
                return true;
        }
    }
    // 5. Else,
    else {
        // a. If offsetMinutesOne is not EMPTY and offsetMinutesTwo is not EMPTY and offsetMinutesOne = offsetMinutesTwo,
        //    return true.
        if (offset_minutes_one.has_value() && offset_minutes_two.has_value()) {
            if (offset_minutes_one == offset_minutes_two)
                return true;
        }
    }

    // 6. Return false.
    return false;
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

        // b. Let offset be the source text matched by the UTCOffset[~SubMinutePrecision] Parse Node contained within parseResult.
        // c. Let offsetNanoseconds be ! ParseDateTimeUTCOffset(CodePointsToString(offset)).
        auto offset_nanoseconds = parse_date_time_utc_offset(parse_result.time_zone_offset->source_text);

        // d. Let offsetMinutes be offsetNanoseconds / (60 × 10**9).
        auto offset_minutes = offset_nanoseconds / 60'000'000'000;

        // e. Assert: offsetMinutes is an integer.
        VERIFY(trunc(offset_minutes) == offset_minutes);

        // f. Return the Record { [[Name]]: empty, [[OffsetMinutes]]: offsetMinutes }.
        return TimeZone { .name = {}, .offset_minutes = static_cast<i64>(offset_minutes) };
    }
}

}
