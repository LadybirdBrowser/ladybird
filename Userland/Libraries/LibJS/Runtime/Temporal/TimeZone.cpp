/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <AK/TypeCasts.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/TimeZoneConstructor.h>
#include <LibJS/Runtime/Temporal/TimeZoneMethods.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibUnicode/TimeZone.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(TimeZone);

// 11 Temporal.TimeZone Objects, https://tc39.es/proposal-temporal/#sec-temporal-timezone-objects
TimeZone::TimeZone(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

// 11.1.1 IsAvailableTimeZoneName ( timeZone ), https://tc39.es/proposal-temporal/#sec-isavailabletimezonename
bool is_available_time_zone_name(StringView time_zone)
{
    // 1. Let timeZones be AvailableTimeZones().
    auto const& time_zones = Unicode::available_time_zones();

    // 2. For each String candidate in timeZones, do
    for (auto const& candidate : time_zones) {
        // a. If timeZone is an ASCII-case-insensitive match for candidate, return true.
        if (time_zone.equals_ignoring_ascii_case(candidate))
            return true;
    }

    // 3. Return false.
    return false;
}

// 6.4.2 CanonicalizeTimeZoneName ( timeZone ), https://tc39.es/ecma402/#sec-canonicalizetimezonename
// 11.1.2 CanonicalizeTimeZoneName ( timeZone ), https://tc39.es/proposal-temporal/#sec-canonicalizetimezonename
// 15.1.2 CanonicalizeTimeZoneName ( timeZone ), https://tc39.es/proposal-temporal/#sup-canonicalizetimezonename
ThrowCompletionOr<String> canonicalize_time_zone_name(VM&, StringView time_zone)
{
    auto const& time_zones = Unicode::available_time_zones();

    // 1. Let ianaTimeZone be the String value of the Zone or Link name of the IANA Time Zone Database that is an
    //    ASCII-case-insensitive match of timeZone as described in 6.1.
    auto it = time_zones.find_if([&](auto const& candidate) {
        return time_zone.equals_ignoring_ascii_case(candidate);
    });
    VERIFY(it != time_zones.end());

    // 2. If ianaTimeZone is a Link name, let ianaTimeZone be the String value of the corresponding Zone name as specified
    //    in the file backward of the IANA Time Zone Database.
    auto iana_time_zone = Unicode::resolve_primary_time_zone(*it).value_or_lazy_evaluated([&]() {
        return MUST(String::from_utf8(time_zone));
    });

    // 3. If ianaTimeZone is one of "Etc/UTC", "Etc/GMT", or "GMT", return "UTC".
    if (iana_time_zone.is_one_of("Etc/UTC"sv, "Etc/GMT"sv, "GMT"sv))
        return "UTC"_string;

    // 4. Return ianaTimeZone.
    return iana_time_zone;
}

// 11.6.1 CreateTemporalTimeZone ( identifier [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporaltimezone
ThrowCompletionOr<TimeZone*> create_temporal_time_zone(VM& vm, String identifier, FunctionObject const* new_target)
{
    auto& realm = *vm.current_realm();

    // 1. If newTarget is not present, set newTarget to %Temporal.TimeZone%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_time_zone_constructor();

    // 2. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.TimeZone.prototype%", « [[InitializedTemporalTimeZone]], [[Identifier]], [[OffsetNanoseconds]] »).
    auto object = TRY(ordinary_create_from_constructor<TimeZone>(vm, *new_target, &Intrinsics::temporal_time_zone_prototype));

    // 3. If IsTimeZoneOffsetString(identifier) is true, then
    if (is_time_zone_offset_string(identifier)) {
        // a. Let offsetNanosecondsResult be ParseTimeZoneOffsetString(identifier).
        auto offset_nanoseconds_result = parse_time_zone_offset_string(identifier);

        // b. Set object.[[Identifier]] to ! FormatTimeZoneOffsetString(offsetNanosecondsResult).
        object->set_identifier(MUST_OR_THROW_OOM(format_time_zone_offset_string(vm, offset_nanoseconds_result)));

        // c. Set object.[[OffsetNanoseconds]] to offsetNanosecondsResult.
        object->set_offset_nanoseconds(offset_nanoseconds_result);
    }
    // 4. Else,
    else {
        // a. Assert: ! CanonicalizeTimeZoneName(identifier) is identifier.
        VERIFY(MUST_OR_THROW_OOM(canonicalize_time_zone_name(vm, identifier)) == identifier);

        // b. Set object.[[Identifier]] to identifier.
        object->set_identifier(move(identifier));

        // c. Set object.[[OffsetNanoseconds]] to undefined.
        // NOTE: No-op.
    }

    // 5. Return object.
    return object.ptr();
}

// 11.6.2 GetISOPartsFromEpoch ( epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-getisopartsfromepoch
ISODateTime get_iso_parts_from_epoch(VM& vm, Crypto::SignedBigInteger const& epoch_nanoseconds)
{
    // 1. Assert: ! IsValidEpochNanoseconds(ℤ(epochNanoseconds)) is true.
    VERIFY(is_valid_epoch_nanoseconds(BigInt::create(vm, epoch_nanoseconds)));

    // 2. Let remainderNs be epochNanoseconds modulo 10^6.
    auto remainder_ns_bigint = modulo(epoch_nanoseconds, Crypto::UnsignedBigInteger { 1'000'000 });
    auto remainder_ns = remainder_ns_bigint.to_double();

    // 3. Let epochMilliseconds be 𝔽((epochNanoseconds - remainderNs) / 10^6).
    auto epoch_milliseconds_bigint = epoch_nanoseconds.minus(remainder_ns_bigint).divided_by(Crypto::UnsignedBigInteger { 1'000'000 }).quotient;
    auto epoch_milliseconds = epoch_milliseconds_bigint.to_double();

    // 4. Let year be ℝ(! YearFromTime(epochMilliseconds)).
    auto year = year_from_time(epoch_milliseconds);

    // 5. Let month be ℝ(! MonthFromTime(epochMilliseconds)) + 1.
    auto month = static_cast<u8>(month_from_time(epoch_milliseconds) + 1);

    // 6. Let day be ℝ(! DateFromTime(epochMilliseconds)).
    auto day = date_from_time(epoch_milliseconds);

    // 7. Let hour be ℝ(! HourFromTime(epochMilliseconds)).
    auto hour = hour_from_time(epoch_milliseconds);

    // 8. Let minute be ℝ(! MinFromTime(epochMilliseconds)).
    auto minute = min_from_time(epoch_milliseconds);

    // 9. Let second be ℝ(! SecFromTime(epochMilliseconds)).
    auto second = sec_from_time(epoch_milliseconds);

    // 10. Let millisecond be ℝ(! msFromTime(epochMilliseconds)).
    auto millisecond = ms_from_time(epoch_milliseconds);

    // 11. Let microsecond be floor(remainderNs / 1000).
    auto microsecond = floor(remainder_ns / 1000);

    // 12. Assert: microsecond < 1000.
    VERIFY(microsecond < 1000);

    // 13. Let nanosecond be remainderNs modulo 1000.
    auto nanosecond = modulo(remainder_ns, 1000);

    // 14. Return the Record { [[Year]]: year, [[Month]]: month, [[Day]]: day, [[Hour]]: hour, [[Minute]]: minute, [[Second]]: second, [[Millisecond]]: millisecond, [[Microsecond]]: microsecond, [[Nanosecond]]: nanosecond }.
    return { .year = year, .month = month, .day = day, .hour = hour, .minute = minute, .second = second, .millisecond = millisecond, .microsecond = static_cast<u16>(microsecond), .nanosecond = static_cast<u16>(nanosecond) };
}

// 11.6.3 GetNamedTimeZoneNextTransition ( timeZoneIdentifier, epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-getianatimezonenexttransition
BigInt* get_named_time_zone_next_transition(VM&, [[maybe_unused]] StringView time_zone_identifier, [[maybe_unused]] BigInt const& epoch_nanoseconds)
{
    // The implementation-defined abstract operation GetNamedTimeZoneNextTransition takes arguments timeZoneIdentifier (a String) and epochNanoseconds (a BigInt) and returns a BigInt or null.
    // The returned value t represents the number of nanoseconds since the Unix epoch in UTC that corresponds to the first time zone transition after epochNanoseconds in the IANA time zone identified by timeZoneIdentifier. The operation returns null if no such transition exists for which t ≤ ℤ(nsMaxInstant).
    // Given the same values of epochNanoseconds and timeZoneIdentifier, the result must be the same for the lifetime of the surrounding agent.

    // TODO: Implement this
    return nullptr;
}

// 11.6.4 GetNamedTimeZonePreviousTransition ( timeZoneIdentifier, epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-getianatimezoneprevioustransition
BigInt* get_named_time_zone_previous_transition(VM&, [[maybe_unused]] StringView time_zone_identifier, [[maybe_unused]] BigInt const& epoch_nanoseconds)
{
    // The implementation-defined abstract operation GetNamedTimeZonePreviousTransition takes arguments timeZoneIdentifier (a String) and epochNanoseconds (a BigInt) and returns a BigInt or null.
    // The returned value t represents the number of nanoseconds since the Unix epoch in UTC that corresponds to the last time zone transition before epochNanoseconds in the IANA time zone identified by timeZoneIdentifier. The operation returns null if no such transition exists for which t ≥ ℤ(nsMinInstant).
    // Given the same values of epochNanoseconds and timeZoneIdentifier, the result must be the same for the lifetime of the surrounding agent.

    // TODO: Implement this
    return nullptr;
}

// 11.6.5 FormatTimeZoneOffsetString ( offsetNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-formattimezoneoffsetstring
ThrowCompletionOr<String> format_time_zone_offset_string(VM& vm, double offset_nanoseconds)
{
    auto offset = static_cast<i64>(offset_nanoseconds);

    // 1. Assert: offsetNanoseconds is an integer.
    VERIFY(offset == offset_nanoseconds);

    StringBuilder builder;
    // 2. If offsetNanoseconds ≥ 0, let sign be "+"; otherwise, let sign be "-".
    if (offset >= 0)
        builder.append('+');
    else
        builder.append('-');

    // 3. Let _offsetNanoseconds_ be abs(_offsetNanoseconds_).
    offset = AK::abs(offset);

    // 4. Let nanoseconds be offsetNanoseconds modulo 10^9.
    auto nanoseconds = offset % 1000000000;

    // 5. Let seconds be floor(offsetNanoseconds / 10^9) modulo 60.
    auto seconds = (offset / 1000000000) % 60;
    // 6. Let minutes be floor(offsetNanoseconds / (6 × 10^10)) modulo 60.
    auto minutes = (offset / 60000000000) % 60;
    // 7. Let hours be floor(offsetNanoseconds / (3.6 × 10^12)).
    auto hours = offset / 3600000000000;

    // 8. Let h be ToZeroPaddedDecimalString(hours, 2).
    builder.appendff("{:02}", hours);
    // 9. Let m be ToZeroPaddedDecimalString(minutes, 2).
    builder.appendff(":{:02}", minutes);
    // 10. Let s be ToZeroPaddedDecimalString(seconds, 2).
    // NOTE: Handled by steps 11 & 12

    // 11. If nanoseconds ≠ 0, then
    if (nanoseconds != 0) {
        // a. Let fraction be ToZeroPaddedDecimalString(nanoseconds, 9).
        auto fraction = TRY_OR_THROW_OOM(vm, String::formatted("{:09}", nanoseconds));

        // b. Set fraction to the longest possible substring of fraction starting at position 0 and not ending with the code unit 0x0030 (DIGIT ZERO).
        fraction = TRY_OR_THROW_OOM(vm, fraction.trim("0"sv, TrimMode::Right));

        // c. Let post be the string-concatenation of the code unit 0x003A (COLON), s, the code unit 0x002E (FULL STOP), and fraction.
        builder.appendff(":{:02}.{}", seconds, fraction);
    }
    // 12. Else if seconds ≠ 0, then
    else if (seconds != 0) {
        // a. Let post be the string-concatenation of the code unit 0x003A (COLON) and s.
        builder.appendff(":{:02}", seconds);
    }
    // 13. Else,
    //    a. Let post be the empty String.

    // 14. Return the string-concatenation of sign, h, the code unit 0x003A (COLON), m, and post.
    return TRY_OR_THROW_OOM(vm, builder.to_string());
}

// 11.6.6 FormatISOTimeZoneOffsetString ( offsetNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-formatisotimezoneoffsetstring
ThrowCompletionOr<String> format_iso_time_zone_offset_string(VM& vm, double offset_nanoseconds)
{
    // 1. Assert: offsetNanoseconds is an integer.
    VERIFY(trunc(offset_nanoseconds) == offset_nanoseconds);

    // 2. Set offsetNanoseconds to RoundNumberToIncrement(offsetNanoseconds, 60 × 10^9, "halfExpand").
    offset_nanoseconds = round_number_to_increment(offset_nanoseconds, 60000000000, "halfExpand"sv);

    // 3. If offsetNanoseconds ≥ 0, let sign be "+"; otherwise, let sign be "-".
    auto sign = offset_nanoseconds >= 0 ? "+"sv : "-"sv;

    // 4. Set offsetNanoseconds to abs(offsetNanoseconds).
    offset_nanoseconds = fabs(offset_nanoseconds);

    // 5. Let minutes be offsetNanoseconds / (60 × 10^9) modulo 60.
    auto minutes = fmod(offset_nanoseconds / 60000000000, 60);

    // 6. Let hours be floor(offsetNanoseconds / (3600 × 10^9)).
    auto hours = floor(offset_nanoseconds / 3600000000000);

    // 7. Let h be ToZeroPaddedDecimalString(hours, 2).
    // 8. Let m be ToZeroPaddedDecimalString(minutes, 2).
    // 9. Return the string-concatenation of sign, h, the code unit 0x003A (COLON), and m.
    return TRY_OR_THROW_OOM(vm, String::formatted("{}{:02}:{:02}", sign, (u32)hours, (u32)minutes));
}

// 11.6.7 ToTemporalTimeZone ( temporalTimeZoneLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporaltimezone
ThrowCompletionOr<Object*> to_temporal_time_zone(VM& vm, Value temporal_time_zone_like)
{
    // 1. If Type(temporalTimeZoneLike) is Object, then
    if (temporal_time_zone_like.is_object()) {
        // a. If temporalTimeZoneLike has an [[InitializedTemporalTimeZone]] internal slot, then
        if (is<TimeZone>(temporal_time_zone_like.as_object())) {
            // i. Return temporalTimeZoneLike.
            return &temporal_time_zone_like.as_object();
        }

        // b. If temporalTimeZoneLike has an [[InitializedTemporalZonedDateTime]] internal slot, then
        if (is<ZonedDateTime>(temporal_time_zone_like.as_object())) {
            auto& zoned_date_time = static_cast<ZonedDateTime&>(temporal_time_zone_like.as_object());

            // i. Return temporalTimeZoneLike.[[TimeZone]].
            return &zoned_date_time.time_zone();
        }

        // c. If temporalTimeZoneLike has an [[InitializedTemporalCalendar]] internal slot, throw a RangeError exception.
        if (is<Calendar>(temporal_time_zone_like.as_object()))
            return vm.throw_completion<RangeError>(ErrorType::TemporalUnexpectedCalendarObject);

        // d. If ? HasProperty(temporalTimeZoneLike, "timeZone") is false, return temporalTimeZoneLike.
        if (!TRY(temporal_time_zone_like.as_object().has_property(vm.names.timeZone)))
            return &temporal_time_zone_like.as_object();

        // e. Set temporalTimeZoneLike to ? Get(temporalTimeZoneLike, "timeZone").
        temporal_time_zone_like = TRY(temporal_time_zone_like.as_object().get(vm.names.timeZone));

        // f. If Type(temporalTimeZoneLike) is Object, then
        if (temporal_time_zone_like.is_object()) {
            // i. If temporalTimeZoneLike has an [[InitializedTemporalCalendar]] internal slot, throw a RangeError exception.
            if (is<Calendar>(temporal_time_zone_like.as_object()))
                return vm.throw_completion<RangeError>(ErrorType::TemporalUnexpectedCalendarObject);

            // ii. If ? HasProperty(temporalTimeZoneLike, "timeZone") is false, return temporalTimeZoneLike.
            if (!TRY(temporal_time_zone_like.as_object().has_property(vm.names.timeZone)))
                return &temporal_time_zone_like.as_object();
        }
    }

    // 2. Let identifier be ? ToString(temporalTimeZoneLike).
    auto identifier = TRY(temporal_time_zone_like.to_string(vm));

    // 3. Let parseResult be ? ParseTemporalTimeZoneString(identifier).
    auto parse_result = TRY(parse_temporal_time_zone_string(vm, identifier));

    // 4. If parseResult.[[Name]] is not undefined, then
    if (parse_result.name.has_value()) {
        // a. Let name be parseResult.[[Name]].
        auto name = parse_result.name.release_value();

        // b. If IsTimeZoneOffsetString(name) is false, then
        if (!is_time_zone_offset_string(name)) {
            // i. If IsAvailableTimeZoneName(name) is false, throw a RangeError exception.
            if (!is_available_time_zone_name(name))
                return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeZoneName, name);

            // ii. Set name to ! CanonicalizeTimeZoneName(name).
            name = MUST_OR_THROW_OOM(canonicalize_time_zone_name(vm, name));
        }

        // c. Return ! CreateTemporalTimeZone(name).
        return MUST_OR_THROW_OOM(create_temporal_time_zone(vm, move(name)));
    }

    // 5. If parseResult.[[Z]] is true, return ! CreateTemporalTimeZone("UTC").
    if (parse_result.z)
        return MUST_OR_THROW_OOM(create_temporal_time_zone(vm, "UTC"_string));

    // 6. Return ! CreateTemporalTimeZone(parseResult.[[OffsetString]]).
    return MUST_OR_THROW_OOM(create_temporal_time_zone(vm, parse_result.offset_string.release_value()));
}

// 11.5.19 GetOffsetNanosecondsFor ( timeZoneRec, instant ), https://tc39.es/proposal-temporal/#sec-temporal-getoffsetnanosecondsfor
ThrowCompletionOr<double> get_offset_nanoseconds_for(VM& vm, TimeZoneMethods const& time_zone_record, Instant const& instant)
{
    // 1. Let offsetNanoseconds be ? TimeZoneMethodsRecordCall(timeZoneRec, GET-OFFSET-NANOSECONDS-FOR, « instant »).
    auto offset_nanoseconds_value = TRY(time_zone_methods_record_call(vm, time_zone_record, TimeZoneMethod::GetOffsetNanosecondsFor, { { &instant } }));

    // 2. If TimeZoneMethodsRecordIsBuiltin(timeZoneRec), return ℝ(offsetNanoseconds).
    if (time_zone_methods_record_is_builtin(time_zone_record))
        return offset_nanoseconds_value.as_double();

    // 3. If Type(offsetNanoseconds) is not Number, throw a TypeError exception.
    if (!offset_nanoseconds_value.is_number())
        return vm.throw_completion<TypeError>(ErrorType::IsNotA, "Offset nanoseconds value", "number");

    // 4. If IsIntegralNumber(offsetNanoseconds) is false, throw a RangeError exception.
    if (!offset_nanoseconds_value.is_integral_number())
        return vm.throw_completion<RangeError>(ErrorType::IsNotAn, "Offset nanoseconds value", "integral number");

    // 5. Set offsetNanoseconds to ℝ(offsetNanoseconds).
    auto offset_nanoseconds = offset_nanoseconds_value.as_double();

    // 6. If abs(offsetNanoseconds) ≥ nsPerDay, throw a RangeError exception.
    if (fabs(offset_nanoseconds) >= ns_per_day)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidOffsetNanosecondsValue);

    // 7. Return offsetNanoseconds.
    return offset_nanoseconds;
}

// 11.6.9 BuiltinTimeZoneGetOffsetStringFor ( timeZone, instant ), https://tc39.es/proposal-temporal/#sec-temporal-builtintimezonegetoffsetstringfor
ThrowCompletionOr<String> builtin_time_zone_get_offset_string_for(VM& vm, Value time_zone, Instant& instant)
{
    auto time_zone_record = TRY(create_time_zone_methods_record(vm, GC::Ref<Object> { time_zone.as_object() }, { { TimeZoneMethod::GetOffsetNanosecondsFor } }));

    // 1. Let offsetNanoseconds be ? GetOffsetNanosecondsFor(timeZone, instant).
    auto offset_nanoseconds = TRY(get_offset_nanoseconds_for(vm, time_zone_record, instant));

    // 2. Return ! FormatTimeZoneOffsetString(offsetNanoseconds).
    return MUST_OR_THROW_OOM(format_time_zone_offset_string(vm, offset_nanoseconds));
}

// 11.6.10 BuiltinTimeZoneGetPlainDateTimeFor ( timeZone, instant, calendar ), https://tc39.es/proposal-temporal/#sec-temporal-builtintimezonegetplaindatetimefor
ThrowCompletionOr<PlainDateTime*> builtin_time_zone_get_plain_date_time_for(VM& vm, Value time_zone, Instant& instant, Object& calendar)
{
    auto time_zone_record = TRY(create_time_zone_methods_record(vm, GC::Ref<Object> { time_zone.as_object() }, { { TimeZoneMethod::GetOffsetNanosecondsFor } }));

    // 1. Assert: instant has an [[InitializedTemporalInstant]] internal slot.

    // 2. Let offsetNanoseconds be ? GetOffsetNanosecondsFor(timeZone, instant).
    auto offset_nanoseconds = TRY(get_offset_nanoseconds_for(vm, time_zone_record, instant));

    // 3. Let result be ! GetISOPartsFromEpoch(ℝ(instant.[[Nanoseconds]])).
    auto result = get_iso_parts_from_epoch(vm, instant.nanoseconds().big_integer());

    // 4. Set result to BalanceISODateTime(result.[[Year]], result.[[Month]], result.[[Day]], result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]] + offsetNanoseconds).
    result = balance_iso_date_time(result.year, result.month, result.day, result.hour, result.minute, result.second, result.millisecond, result.microsecond, result.nanosecond + offset_nanoseconds);

    // 5. Return ? CreateTemporalDateTime(result.[[Year]], result.[[Month]], result.[[Day]], result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]], calendar).
    return create_temporal_date_time(vm, result.year, result.month, result.day, result.hour, result.minute, result.second, result.millisecond, result.microsecond, result.nanosecond, calendar);
}

// 11.6.11 BuiltinTimeZoneGetInstantFor ( timeZone, dateTime, disambiguation ), https://tc39.es/proposal-temporal/#sec-temporal-builtintimezonegetinstantfor
ThrowCompletionOr<GC::Ref<Instant>> builtin_time_zone_get_instant_for(VM& vm, Value time_zone, PlainDateTime& date_time, StringView disambiguation)
{
    // 1. Assert: dateTime has an [[InitializedTemporalDateTime]] internal slot.

    // 2. Let possibleInstants be ? GetPossibleInstantsFor(timeZone, dateTime).
    auto time_zone_record = TRY(create_time_zone_methods_record(vm, GC::Ref<Object> { time_zone.as_object() }, { { TimeZoneMethod::GetOffsetNanosecondsFor, TimeZoneMethod::GetPossibleInstantsFor } }));
    auto possible_instants = TRY(get_possible_instants_for(vm, time_zone_record, date_time));

    // 3. Return ? DisambiguatePossibleInstants(possibleInstants, timeZone, dateTime, disambiguation).
    return disambiguate_possible_instants(vm, possible_instants, time_zone_record, date_time, disambiguation);
}

// 11.6.12 DisambiguatePossibleInstants ( possibleInstants, timeZone, dateTime, disambiguation ), https://tc39.es/proposal-temporal/#sec-temporal-disambiguatepossibleinstants
ThrowCompletionOr<GC::Ref<Instant>> disambiguate_possible_instants(VM& vm, GC::MarkedVector<GC::Ref<Instant>> const& possible_instants, TimeZoneMethods const& time_zone_record, PlainDateTime& date_time, StringView disambiguation)
{
    // 1. Assert: TimeZoneMethodsRecordHasLookedUp(timeZoneRec, GET-POSSIBLE-INSTANTS-FOR) is true.
    VERIFY(time_zone_methods_record_has_looked_up(time_zone_record, TimeZoneMethod::GetPossibleInstantsFor));

    // 2. Assert: If possibleInstants is empty, and disambiguation is not "reject", TimeZoneMethodsRecordHasLookedUp(timeZoneRec, GET-OFFSET-NANOSECONDS-FOR) is true.
    if (possible_instants.is_empty() && disambiguation != "reject"sv)
        VERIFY(time_zone_methods_record_has_looked_up(time_zone_record, TimeZoneMethod::GetOffsetNanosecondsFor));

    // 3. Let n be possibleInstants's length.
    auto n = possible_instants.size();

    // 4. If n = 1, then
    if (n == 1) {
        // a. Return possibleInstants[0].
        return possible_instants[0];
    }

    // 5. If n ≠ 0, then
    if (n != 0) {
        // a. If disambiguation is "earlier" or "compatible", then
        if (disambiguation.is_one_of("earlier"sv, "compatible"sv)) {
            // i. Return possibleInstants[0].
            return possible_instants[0];
        }

        // b. If disambiguation is "later", then
        if (disambiguation == "later"sv) {
            // i. Return possibleInstants[n - 1].
            return possible_instants[n - 1];
        }

        // c. Assert: disambiguation is "reject".
        VERIFY(disambiguation == "reject"sv);

        // d. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalDisambiguatePossibleInstantsRejectMoreThanOne);
    }

    // 6. Assert: n = 0.
    VERIFY(n == 0);

    // 7. If disambiguation is "reject", then
    if (disambiguation == "reject"sv) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalDisambiguatePossibleInstantsRejectZero);
    }

    // 8. Let epochNanoseconds be GetUTCEpochNanoseconds(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]]).
    auto epoch_nanoseconds = get_utc_epoch_nanoseconds(date_time.iso_year(), date_time.iso_month(), date_time.iso_day(), date_time.iso_hour(), date_time.iso_minute(), date_time.iso_second(), date_time.iso_millisecond(), date_time.iso_microsecond(), date_time.iso_nanosecond());

    // 9. Let dayBeforeNs be epochNanoseconds - ℤ(nsPerDay).
    auto day_before_ns = BigInt::create(vm, epoch_nanoseconds.minus(ns_per_day_bigint));

    // 10. If IsValidEpochNanoseconds(dayBeforeNs) is false, throw a RangeError exception.
    if (!is_valid_epoch_nanoseconds(day_before_ns))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

    // 11. Let dayBefore be ! CreateTemporalInstant(dayBeforeNs).
    auto* day_before = MUST(create_temporal_instant(vm, day_before_ns));

    // 12. Let dayAfterNs be epochNanoseconds + ℤ(nsPerDay).
    auto day_after_ns = BigInt::create(vm, epoch_nanoseconds.plus(ns_per_day_bigint));

    // 13. If IsValidEpochNanoseconds(dayAfterNs) is false, throw a RangeError exception.
    if (!is_valid_epoch_nanoseconds(day_after_ns))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

    // 14. Let dayAfter be ! CreateTemporalInstant(dayAfterNs).
    auto* day_after = MUST(create_temporal_instant(vm, day_after_ns));

    // 15. Let offsetBefore be ? GetOffsetNanosecondsFor(timeZoneRec, dayBefore).
    auto offset_before = TRY(get_offset_nanoseconds_for(vm, time_zone_record, *day_before));

    // 16. Let offsetAfter be ? GetOffsetNanosecondsFor(timeZoneRec, dayAfter).
    auto offset_after = TRY(get_offset_nanoseconds_for(vm, time_zone_record, *day_after));

    // 17. Let nanoseconds be offsetAfter - offsetBefore.
    auto nanoseconds = offset_after - offset_before;

    // 18. If disambiguation is "earlier", then
    if (disambiguation == "earlier"sv) {
        // a. Let earlier be ? AddDateTime(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]], dateTime.[[Calendar]], 0, 0, 0, 0, 0, 0, 0, 0, 0, -nanoseconds, undefined).
        auto earlier = TRY(add_date_time(vm, date_time.iso_year(), date_time.iso_month(), date_time.iso_day(), date_time.iso_hour(), date_time.iso_minute(), date_time.iso_second(), date_time.iso_millisecond(), date_time.iso_microsecond(), date_time.iso_nanosecond(), date_time.calendar(), 0, 0, 0, 0, 0, 0, 0, 0, 0, -nanoseconds, nullptr));

        // b. Let earlierDateTime be ! CreateTemporalDateTime(earlier.[[Year]], earlier.[[Month]], earlier.[[Day]], earlier.[[Hour]], earlier.[[Minute]], earlier.[[Second]], earlier.[[Millisecond]], earlier.[[Microsecond]], earlier.[[Nanosecond]], dateTime.[[Calendar]]).
        auto* earlier_date_time = MUST(create_temporal_date_time(vm, earlier.year, earlier.month, earlier.day, earlier.hour, earlier.minute, earlier.second, earlier.millisecond, earlier.microsecond, earlier.nanosecond, date_time.calendar()));

        // c. Set possibleInstants to ? GetPossibleInstantsFor(timeZone, earlierDateTime).
        auto possible_instants_ = TRY(get_possible_instants_for(vm, time_zone_record, *earlier_date_time));

        // d. If possibleInstants is empty, throw a RangeError exception.
        if (possible_instants_.is_empty())
            return vm.throw_completion<RangeError>(ErrorType::TemporalDisambiguatePossibleInstantsEarlierZero);

        // e. Return possibleInstants[0].
        return possible_instants_[0];
    }

    // 19. Assert: disambiguation is "compatible" or "later".
    VERIFY(disambiguation.is_one_of("compatible"sv, "later"sv));

    // 20. Let later be ? AddDateTime(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]], dateTime.[[Calendar]], 0, 0, 0, 0, 0, 0, 0, 0, 0, nanoseconds, undefined).
    auto later = TRY(add_date_time(vm, date_time.iso_year(), date_time.iso_month(), date_time.iso_day(), date_time.iso_hour(), date_time.iso_minute(), date_time.iso_second(), date_time.iso_millisecond(), date_time.iso_microsecond(), date_time.iso_nanosecond(), date_time.calendar(), 0, 0, 0, 0, 0, 0, 0, 0, 0, nanoseconds, nullptr));

    // 21. Let laterDateTime be ! CreateTemporalDateTime(later.[[Year]], later.[[Month]], later.[[Day]], later.[[Hour]], later.[[Minute]], later.[[Second]], later.[[Millisecond]], later.[[Microsecond]], later.[[Nanosecond]], dateTime.[[Calendar]]).
    auto* later_date_time = MUST(create_temporal_date_time(vm, later.year, later.month, later.day, later.hour, later.minute, later.second, later.millisecond, later.microsecond, later.nanosecond, date_time.calendar()));

    // 22. Set possibleInstants to ? GetPossibleInstantsFor(timeZone, laterDateTime).
    auto possible_instants_ = TRY(get_possible_instants_for(vm, time_zone_record, *later_date_time));

    // 23. Set n to possibleInstants's length.
    n = possible_instants_.size();

    // 24. If n = 0, throw a RangeError exception.
    if (n == 0)
        return vm.throw_completion<RangeError>(ErrorType::TemporalDisambiguatePossibleInstantsZero);

    // 25. Return possibleInstants[n - 1].
    return possible_instants_[n - 1];
}

// 11.5.24 GetPossibleInstantsFor ( timeZoneRec, dateTime ), https://tc39.es/proposal-temporal/#sec-temporal-getpossibleinstantsfor
ThrowCompletionOr<GC::MarkedVector<GC::Ref<Instant>>> get_possible_instants_for(VM& vm, TimeZoneMethods const& time_zone_record, PlainDateTime const& date_time)
{
    // 1. Let possibleInstants be ? TimeZoneMethodsRecordCall(timeZoneRec, GET-POSSIBLE-INSTANTS-FOR, « dateTime »).
    auto possible_instants = TRY(time_zone_methods_record_call(vm, time_zone_record, TimeZoneMethod::GetPossibleInstantsFor, { { &date_time } }));

    // 2. If TimeZoneMethodsRecordIsBuiltin(timeZoneRec), return ! CreateListFromArrayLike(possibleInstants, « Object »).
    if (time_zone_methods_record_is_builtin(time_zone_record)) {
        auto list = GC::MarkedVector<GC::Ref<Instant>> { vm.heap() };

        (void)MUST(create_list_from_array_like(vm, possible_instants, [&list](auto value) -> ThrowCompletionOr<void> {
            list.append(verify_cast<Instant>(value.as_object()));
            return {};
        }));

        return list;
    }

    // 3. Let iteratorRecord be ? GetIterator(possibleInstants, SYNC).
    auto iterator = TRY(get_iterator(vm, possible_instants, IteratorHint::Sync));

    // 4. Let list be a new empty List.
    auto list = GC::MarkedVector<GC::Ref<Instant>> { vm.heap() };

    // 5. Repeat,
    while (true) {
        // a. Let value be ? IteratorStepValue(iteratorRecord).
        auto value = TRY(iterator_step_value(vm, iterator));

        // b. If value is DONE, then
        if (!value.has_value()) {
            // i. Let numResults be list's length.
            auto num_results = list.size();

            // ii. If numResults > 1, then
            if (num_results > 1) {
                // 1. Let epochNs be a new empty List.
                // 2. For each value instant in list, do
                //     a. Append instant.[[EpochNanoseconds]] to the end of the List epochNs.
                //     FIXME: spec bug? shouldn't [[EpochNanoseconds]] just be called [[Nanoseconds]]?
                // 3. Let min be the least element of the List epochNs.
                // 4. Let max be the greatest element of the List epochNs.

                auto const* min = &list.first()->nanoseconds().big_integer();
                auto const* max = &list.first()->nanoseconds().big_integer();

                for (auto it = list.begin() + 1; it != list.end(); ++it) {
                    auto const& value = it->ptr()->nanoseconds().big_integer();

                    if (value < *min)
                        min = &value;
                    else if (value > *max)
                        max = &value;
                }

                // 5. If abs(ℝ(max - min)) > nsPerDay, throw a RangeError exception.
                if (max->minus(*min).unsigned_value() > ns_per_day_bigint.unsigned_value())
                    return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDuration);
            }

            // iii. Return list.
            return list;
        }

        // c. If value is not an Object or value does not have an [[InitializedTemporalInstant]] internal slot, then
        if (!value->is_object() || !is<Instant>(value->as_object())) {
            // i. Let completion be ThrowCompletion(a newly created TypeError object).
            auto completion = vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "Temporal.Instant");

            // ii. Return ? IteratorClose(iteratorRecord, completion).
            return iterator_close(vm, iterator, move(completion));
        }

        // d. Append value to the end of the List list.
        list.append(verify_cast<Instant>(value->as_object()));
    }

    // 7. Return list.
    return { move(list) };
}

// 11.6.14 TimeZoneEquals ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-timezoneequals
ThrowCompletionOr<bool> time_zone_equals(VM& vm, Object& one, Object& two)
{
    // 1. If one and two are the same Object value, return true.
    if (&one == &two)
        return true;

    // 2. Let timeZoneOne be ? ToString(one).
    auto time_zone_one = TRY(Value(&one).to_string(vm));

    // 3. Let timeZoneTwo be ? ToString(two).
    auto time_zone_two = TRY(Value(&two).to_string(vm));

    // 4. If timeZoneOne is timeZoneTwo, return true.
    if (time_zone_one == time_zone_two)
        return true;

    // 5. Return false.
    return false;
}

}
