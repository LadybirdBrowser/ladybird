/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibJS/Runtime/Temporal/ZonedDateTimeConstructor.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(ZonedDateTime);

// 6 Temporal.ZonedDateTime Objects, https://tc39.es/proposal-temporal/#sec-temporal-zoneddatetime-objects
ZonedDateTime::ZonedDateTime(BigInt const& nanoseconds, Object& time_zone, Object& calendar, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_nanoseconds(nanoseconds)
    , m_time_zone(time_zone)
    , m_calendar(calendar)
{
}

void ZonedDateTime::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_nanoseconds);
    visitor.visit(m_time_zone);
    visitor.visit(m_calendar);
}

// 6.5.1 InterpretISODateTimeOffset ( year, month, day, hour, minute, second, millisecond, microsecond, nanosecond, offsetBehaviour, offsetNanoseconds, timeZone, disambiguation, offsetOption, matchBehaviour ), https://tc39.es/proposal-temporal/#sec-temporal-interpretisodatetimeoffset
ThrowCompletionOr<BigInt const*> interpret_iso_date_time_offset(VM& vm, i32 year, u8 month, u8 day, u8 hour, u8 minute, u8 second, u16 millisecond, u16 microsecond, u16 nanosecond, OffsetBehavior offset_behavior, double offset_nanoseconds, Value time_zone, StringView disambiguation, StringView offset_option, MatchBehavior match_behavior)
{
    // 1. Let calendar be ! GetISO8601Calendar().
    auto* calendar = get_iso8601_calendar(vm);

    // 2. Let dateTime be ? CreateTemporalDateTime(year, month, day, hour, minute, second, millisecond, microsecond, nanosecond, calendar).
    auto* date_time = TRY(create_temporal_date_time(vm, year, month, day, hour, minute, second, millisecond, microsecond, nanosecond, *calendar));

    // 3. If offsetBehaviour is wall or offsetOption is "ignore", then
    if (offset_behavior == OffsetBehavior::Wall || offset_option == "ignore"sv) {
        // a. Let instant be ? BuiltinTimeZoneGetInstantFor(timeZone, dateTime, disambiguation).
        auto instant = TRY(builtin_time_zone_get_instant_for(vm, time_zone, *date_time, disambiguation));

        // b. Return instant.[[Nanoseconds]].
        return &instant->nanoseconds();
    }

    // 4. If offsetBehaviour is exact or offsetOption is "use", then
    if (offset_behavior == OffsetBehavior::Exact || offset_option == "use"sv) {
        // a. Let epochNanoseconds be GetUTCEpochNanoseconds(year, month, day, hour, minute, second, millisecond, microsecond, nanosecond).
        auto epoch_nanoseconds = get_utc_epoch_nanoseconds(year, month, day, hour, minute, second, millisecond, microsecond, nanosecond);

        // b. Set epochNanoseconds to epochNanoseconds - ℤ(offsetNanoseconds).
        epoch_nanoseconds = epoch_nanoseconds.minus(Crypto::SignedBigInteger { offset_nanoseconds });

        // c. If ! IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
        if (!is_valid_epoch_nanoseconds(epoch_nanoseconds))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

        // d. Return epochNanoseconds.
        return BigInt::create(vm, move(epoch_nanoseconds)).ptr();
    }

    // 5. Assert: offsetBehaviour is option.
    VERIFY(offset_behavior == OffsetBehavior::Option);

    // 6. Assert: offsetOption is "prefer" or "reject".
    VERIFY(offset_option.is_one_of("prefer"sv, "reject"sv));

    // 7. Let possibleInstants be ? GetPossibleInstantsFor(timeZone, dateTime).
    auto time_zone_record = TRY(create_time_zone_methods_record(vm, NonnullGCPtr<Object> { time_zone.as_object() }, { { TimeZoneMethod::GetPossibleInstantsFor, TimeZoneMethod::GetOffsetNanosecondsFor } }));
    auto possible_instants = TRY(get_possible_instants_for(vm, time_zone_record, *date_time));

    // 8. For each element candidate of possibleInstants, do
    for (auto candidate : possible_instants) {
        // a. Let candidateNanoseconds be ? GetOffsetNanosecondsFor(timeZone, candidate).
        auto candidate_nanoseconds = TRY(get_offset_nanoseconds_for(vm, time_zone_record, *candidate));

        // b. If candidateNanoseconds = offsetNanoseconds, then
        if (candidate_nanoseconds == offset_nanoseconds) {
            // i. Return candidate.[[Nanoseconds]].
            return &candidate->nanoseconds();
        }

        // c. If matchBehaviour is match minutes, then
        if (match_behavior == MatchBehavior::MatchMinutes) {
            // i. Let roundedCandidateNanoseconds be RoundNumberToIncrement(candidateNanoseconds, 60 × 10^9, "halfExpand").
            auto rounded_candidate_nanoseconds = round_number_to_increment(candidate_nanoseconds, 60000000000, "halfExpand"sv);

            // ii. If roundedCandidateNanoseconds = offsetNanoseconds, then
            if (rounded_candidate_nanoseconds == offset_nanoseconds) {
                // 1. Return candidate.[[Nanoseconds]].
                return &candidate->nanoseconds();
            }
        }
    }

    // 9. If offsetOption is "reject", throw a RangeError exception.
    if (offset_option == "reject"sv)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidZonedDateTimeOffset);

    // 10. Let instant be ? DisambiguatePossibleInstants(possibleInstants, timeZone, dateTime, disambiguation).
    auto instant = TRY(disambiguate_possible_instants(vm, possible_instants, time_zone_record, *date_time, disambiguation));

    // 11. Return instant.[[Nanoseconds]].
    return &instant->nanoseconds();
}

// 6.5.2 ToTemporalZonedDateTime ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalzoneddatetime
ThrowCompletionOr<ZonedDateTime*> to_temporal_zoned_date_time(VM& vm, Value item, Object const* options)
{
    // 1. If options is not present, set options to undefined.
    // 2. Assert: Type(options) is Object or Undefined.

    // 3. Let offsetBehaviour be option.
    auto offset_behavior = OffsetBehavior::Option;

    // 4. Let matchBehaviour be match exactly.
    auto match_behavior = MatchBehavior::MatchExactly;

    Object* calendar = nullptr;
    Object* time_zone = nullptr;
    Optional<String> offset_string;
    ISODateTime result;

    // 5. If Type(item) is Object, then
    if (item.is_object()) {
        auto& item_object = item.as_object();

        // a. If item has an [[InitializedTemporalZonedDateTime]] internal slot, then
        if (is<ZonedDateTime>(item_object)) {
            // i. Return item.
            return &static_cast<ZonedDateTime&>(item_object);
        }

        // b. Let calendar be ? GetTemporalCalendarWithISODefault(item).
        calendar = TRY(get_temporal_calendar_with_iso_default(vm, item_object));

        // c. Let fieldNames be ? CalendarFields(calendar, « "day", "hour", "microsecond", "millisecond", "minute", "month", "monthCode", "nanosecond", "second", "year" »).
        auto field_names = TRY(calendar_fields(vm, *calendar, { "day"sv, "hour"sv, "microsecond"sv, "millisecond"sv, "minute"sv, "month"sv, "monthCode"sv, "nanosecond"sv, "second"sv, "year"sv }));

        // d. Append "timeZone" to fieldNames.
        field_names.append("timeZone"_string);

        // e. Append "offset" to fieldNames.
        field_names.append("offset"_string);

        // f. Let fields be ? PrepareTemporalFields(item, fieldNames, « "timeZone" »).
        auto* fields = TRY(prepare_temporal_fields(vm, item_object, field_names, Vector<StringView> { "timeZone"sv }));

        // g. Let timeZone be ! Get(fields, "timeZone").
        auto time_zone_value = MUST(fields->get(vm.names.timeZone));

        // h. Set timeZone to ? ToTemporalTimeZone(timeZone).
        time_zone = TRY(to_temporal_time_zone(vm, time_zone_value));

        // i. Let offsetString be ! Get(fields, "offset").
        auto offset_string_value = MUST(fields->get(vm.names.offset));

        // j. Assert: offsetString is a String or undefined.
        VERIFY(offset_string_value.is_string() || offset_string_value.is_undefined());

        // k. If offsetString is undefined, then
        if (offset_string_value.is_undefined()) {
            // i. Set offsetBehaviour to wall.
            offset_behavior = OffsetBehavior::Wall;
        } else {
            // NOTE: Not in the spec, since it directly assigns to offsetString in step i, but we can't do it there as it's a type mismatch.
            offset_string = offset_string_value.as_string().utf8_string();
        }

        // l. Let result be ? InterpretTemporalDateTimeFields(calendar, fields, options).
        result = TRY(interpret_temporal_date_time_fields(vm, *calendar, *fields, options));
    }
    // 6. Else,
    else {
        // a. Perform ? ToTemporalOverflow(options).
        (void)TRY(to_temporal_overflow(vm, options));

        // b. Let string be ? ToString(item).
        auto string = TRY(item.to_string(vm));

        // c. Let result be ? ParseTemporalZonedDateTimeString(string).
        result = TRY(parse_temporal_zoned_date_time_string(vm, string));

        // d. Let timeZoneName be result.[[TimeZone]].[[Name]].
        auto time_zone_name = result.time_zone.name;

        // e. Assert: timeZoneName is not undefined.
        VERIFY(time_zone_name.has_value());

        // f. If IsTimeZoneOffsetString(timeZoneName) is false, then
        if (!is_time_zone_offset_string(*time_zone_name)) {
            // i. If IsAvailableTimeZoneName(timeZoneName) is false, throw a RangeError exception.
            if (!is_available_time_zone_name(*time_zone_name))
                return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeZoneName, *time_zone_name);

            // ii. Set timeZoneName to ! CanonicalizeTimeZoneName(timeZoneName).
            time_zone_name = MUST_OR_THROW_OOM(canonicalize_time_zone_name(vm, *time_zone_name));
        }

        // g. Let offsetString be result.[[TimeZone]].[[OffsetString]].
        offset_string = move(result.time_zone.offset_string);

        // h. If result.[[TimeZone]].[[Z]] is true, then
        if (result.time_zone.z) {
            // i. Set offsetBehaviour to exact.
            offset_behavior = OffsetBehavior::Exact;
        }
        // i. Else if offsetString is undefined, then
        else if (!offset_string.has_value()) {
            // i. Set offsetBehaviour to wall.
            offset_behavior = OffsetBehavior::Wall;
        }

        // j. Let timeZone be ! CreateTemporalTimeZone(timeZoneName).
        time_zone = MUST_OR_THROW_OOM(create_temporal_time_zone(vm, time_zone_name.release_value()));

        // k. Let calendar be ? ToTemporalCalendarWithISODefault(result.[[Calendar]]).
        auto temporal_calendar_like = result.calendar.has_value()
            ? PrimitiveString::create(vm, result.calendar.value())
            : js_undefined();
        calendar = TRY(to_temporal_calendar_with_iso_default(vm, temporal_calendar_like));

        // l. Set matchBehaviour to match minutes.
        match_behavior = MatchBehavior::MatchMinutes;
    }

    // 7. Let offsetNanoseconds be 0.
    double offset_nanoseconds = 0;

    // 8. If offsetBehaviour is option, then
    if (offset_behavior == OffsetBehavior::Option) {
        // a. If IsTimeZoneOffsetString(offsetString) is false, throw a RangeError exception.
        if (!is_time_zone_offset_string(*offset_string))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeZoneName, *offset_string);

        // a. Set offsetNanoseconds to ? ParseTimeZoneOffsetString(offsetString).
        offset_nanoseconds = parse_time_zone_offset_string(*offset_string);
    }

    // 9. Let disambiguation be ? ToTemporalDisambiguation(options).
    auto disambiguation = TRY(to_temporal_disambiguation(vm, options));

    // 10. Let offsetOption be ? ToTemporalOffset(options, "reject").
    auto offset_option = TRY(to_temporal_offset(vm, options, "reject"sv));

    // 11. Let epochNanoseconds be ? InterpretISODateTimeOffset(result.[[Year]], result.[[Month]], result.[[Day]], result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]], offsetBehaviour, offsetNanoseconds, timeZone, disambiguation, offsetOption, matchBehaviour).
    auto* epoch_nanoseconds = TRY(interpret_iso_date_time_offset(vm, result.year, result.month, result.day, result.hour, result.minute, result.second, result.millisecond, result.microsecond, result.nanosecond, offset_behavior, offset_nanoseconds, time_zone, disambiguation, offset_option, match_behavior));

    // 12. Return ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
    return MUST(create_temporal_zoned_date_time(vm, *epoch_nanoseconds, *time_zone, *calendar));
}

// 6.5.3 CreateTemporalZonedDateTime ( epochNanoseconds, timeZone, calendar [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporalzoneddatetime
ThrowCompletionOr<ZonedDateTime*> create_temporal_zoned_date_time(VM& vm, BigInt const& epoch_nanoseconds, Object& time_zone, Object& calendar, FunctionObject const* new_target)
{
    auto& realm = *vm.current_realm();

    // 1. Assert: ! IsValidEpochNanoseconds(epochNanoseconds) is true.
    VERIFY(is_valid_epoch_nanoseconds(epoch_nanoseconds));

    // 2. If newTarget is not present, set newTarget to %Temporal.ZonedDateTime%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_zoned_date_time_constructor();

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.ZonedDateTime.prototype%", « [[InitializedTemporalZonedDateTime]], [[Nanoseconds]], [[TimeZone]], [[Calendar]] »).
    // 4. Set object.[[Nanoseconds]] to epochNanoseconds.
    // 5. Set object.[[TimeZone]] to timeZone.
    // 6. Set object.[[Calendar]] to calendar.
    auto object = TRY(ordinary_create_from_constructor<ZonedDateTime>(vm, *new_target, &Intrinsics::temporal_time_zone_prototype, epoch_nanoseconds, time_zone, calendar));

    // 7. Return object.
    return object.ptr();
}

// 6.5.4 TemporalZonedDateTimeToString ( zonedDateTime, precision, showCalendar, showTimeZone, showOffset [ , increment, unit, roundingMode ] ), https://tc39.es/proposal-temporal/#sec-temporal-temporalzoneddatetimetostring
ThrowCompletionOr<String> temporal_zoned_date_time_to_string(VM& vm, ZonedDateTime& zoned_date_time, Variant<StringView, u8> const& precision, StringView show_calendar, StringView show_time_zone, StringView show_offset, Optional<u64> increment, Optional<StringView> unit, Optional<StringView> rounding_mode)
{
    // 1. If increment is not present, set increment to 1.
    if (!increment.has_value())
        increment = 1;

    // 2. If unit is not present, set unit to "nanosecond".
    if (!unit.has_value())
        unit = "nanosecond"sv;

    // 3. If roundingMode is not present, set roundingMode to "trunc".
    if (!rounding_mode.has_value())
        rounding_mode = "trunc"sv;

    // 4. Let ns be ! RoundTemporalInstant(zonedDateTime.[[Nanoseconds]], increment, unit, roundingMode).
    auto* ns = round_temporal_instant(vm, zoned_date_time.nanoseconds(), *increment, *unit, *rounding_mode);

    // 5. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time.time_zone();

    // 6. Let instant be ! CreateTemporalInstant(ns).
    auto* instant = MUST(create_temporal_instant(vm, *ns));

    // 7. Let isoCalendar be ! GetISO8601Calendar().
    auto* iso_calendar = get_iso8601_calendar(vm);

    // 8. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, isoCalendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *instant, *iso_calendar));

    // 9. Let dateTimeString be ! TemporalDateTimeToString(temporalDateTime.[[ISOYear]], temporalDateTime.[[ISOMonth]], temporalDateTime.[[ISODay]], temporalDateTime.[[ISOHour]], temporalDateTime.[[ISOMinute]], temporalDateTime.[[ISOSecond]], temporalDateTime.[[ISOMillisecond]], temporalDateTime.[[ISOMicrosecond]], temporalDateTime.[[ISONanosecond]], isoCalendar, precision, "never").
    auto date_time_string = MUST_OR_THROW_OOM(temporal_date_time_to_string(vm, temporal_date_time->iso_year(), temporal_date_time->iso_month(), temporal_date_time->iso_day(), temporal_date_time->iso_hour(), temporal_date_time->iso_minute(), temporal_date_time->iso_second(), temporal_date_time->iso_millisecond(), temporal_date_time->iso_microsecond(), temporal_date_time->iso_nanosecond(), iso_calendar, precision, "never"sv));

    String offset_string;

    // 10. If showOffset is "never", then
    if (show_offset == "never"sv) {
        // a. Let offsetString be the empty String.
        offset_string = {};
    }
    // 11. Else,
    else {
        auto time_zone_record = TRY(create_time_zone_methods_record(vm, NonnullGCPtr<Object> { time_zone }, { { TimeZoneMethod::GetOffsetNanosecondsFor } }));

        // a. Let offsetNs be ? GetOffsetNanosecondsFor(timeZone, instant).
        auto offset_ns = TRY(get_offset_nanoseconds_for(vm, time_zone_record, *instant));

        // b. Let offsetString be ! FormatISOTimeZoneOffsetString(offsetNs).
        offset_string = MUST_OR_THROW_OOM(format_iso_time_zone_offset_string(vm, offset_ns));
    }

    String time_zone_string;

    // 12. If showTimeZone is "never", then
    if (show_time_zone == "never"sv) {
        // a. Let timeZoneString be the empty String.
        time_zone_string = {};
    }
    // 13. Else,
    else {
        // a. Let timeZoneID be ? ToString(timeZone).
        auto time_zone_id = TRY(Value(&time_zone).to_string(vm));

        // b. If showTimeZone is "critical", let flag be "!"; else let flag be the empty String.
        auto flag = show_time_zone == "critical"sv ? "!"sv : ""sv;

        // c. Let timeZoneString be the string-concatenation of the code unit 0x005B (LEFT SQUARE BRACKET), flag, timeZoneID, and the code unit 0x005D (RIGHT SQUARE BRACKET).
        time_zone_string = TRY_OR_THROW_OOM(vm, String::formatted("[{}{}]", flag, time_zone_id));
    }

    // 14. Let calendarString be ? MaybeFormatCalendarAnnotation(zonedDateTime.[[Calendar]], showCalendar).
    auto calendar_string = TRY(maybe_format_calendar_annotation(vm, &zoned_date_time.calendar(), show_calendar));

    // 15. Return the string-concatenation of dateTimeString, offsetString, timeZoneString, and calendarString.
    return TRY_OR_THROW_OOM(vm, String::formatted("{}{}{}{}", date_time_string, offset_string, time_zone_string, calendar_string));
}

// 6.5.5 AddZonedDateTime ( epochNanoseconds, timeZone, calendar, years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-addzoneddatetime
ThrowCompletionOr<BigInt*> add_zoned_date_time(VM& vm, BigInt const& epoch_nanoseconds, Value time_zone, Object& calendar, double years, double months, double weeks, double days, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds, Object* options)
{
    // 1. If options is not present, set options to undefined.
    // 2. Assert: Type(options) is Object or Undefined.

    // 3. If all of years, months, weeks, and days are 0, then
    if (years == 0 && months == 0 && weeks == 0 && days == 0) {
        // a. Return ? AddInstant(epochNanoseconds, hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
        return add_instant(vm, epoch_nanoseconds, hours, minutes, seconds, milliseconds, microseconds, nanoseconds);
    }

    // 4. Let instant be ! CreateTemporalInstant(epochNanoseconds).
    auto* instant = MUST(create_temporal_instant(vm, epoch_nanoseconds));

    // 5. Let temporalDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, instant, calendar).
    auto* temporal_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, time_zone, *instant, calendar));

    // 6. Let datePart be ! CreateTemporalDate(temporalDateTime.[[ISOYear]], temporalDateTime.[[ISOMonth]], temporalDateTime.[[ISODay]], calendar).
    auto* date_part = MUST(create_temporal_date(vm, temporal_date_time->iso_year(), temporal_date_time->iso_month(), temporal_date_time->iso_day(), calendar));

    // 7. Let dateDuration be ! CreateTemporalDuration(years, months, weeks, days, 0, 0, 0, 0, 0, 0).
    auto date_duration = MUST(create_temporal_duration(vm, years, months, weeks, days, 0, 0, 0, 0, 0, 0));

    // 8. Let addedDate be ? CalendarDateAdd(calendar, datePart, dateDuration, options).
    auto* added_date = TRY(calendar_date_add(vm, calendar, date_part, *date_duration, options));

    // 9. Let intermediateDateTime be ? CreateTemporalDateTime(addedDate.[[ISOYear]], addedDate.[[ISOMonth]], addedDate.[[ISODay]], temporalDateTime.[[ISOHour]], temporalDateTime.[[ISOMinute]], temporalDateTime.[[ISOSecond]], temporalDateTime.[[ISOMillisecond]], temporalDateTime.[[ISOMicrosecond]], temporalDateTime.[[ISONanosecond]], calendar).
    auto* intermediate_date_time = TRY(create_temporal_date_time(vm, added_date->iso_year(), added_date->iso_month(), added_date->iso_day(), temporal_date_time->iso_hour(), temporal_date_time->iso_minute(), temporal_date_time->iso_second(), temporal_date_time->iso_millisecond(), temporal_date_time->iso_microsecond(), temporal_date_time->iso_nanosecond(), calendar));

    // 10. Let intermediateInstant be ? BuiltinTimeZoneGetInstantFor(timeZone, intermediateDateTime, "compatible").
    auto intermediate_instant = TRY(builtin_time_zone_get_instant_for(vm, time_zone, *intermediate_date_time, "compatible"sv));

    // 11. Return ? AddInstant(intermediateInstant.[[Nanoseconds]], hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
    return add_instant(vm, intermediate_instant->nanoseconds(), hours, minutes, seconds, milliseconds, microseconds, nanoseconds);
}

// 6.5.6 DifferenceZonedDateTime ( ns1, ns2, timeZone, calendar, largestUnit, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencezoneddatetime
ThrowCompletionOr<DurationRecord> difference_zoned_date_time(VM& vm, BigInt const& nanoseconds1, BigInt const& nanoseconds2, Object& time_zone, Object& calendar, StringView largest_unit, Object const& options)
{
    // 1. If ns1 is ns2, then
    if (nanoseconds1.big_integer() == nanoseconds2.big_integer()) {
        // a. Return ! CreateDurationRecord(0, 0, 0, 0, 0, 0, 0, 0, 0, 0).
        return create_duration_record(0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    }

    // 2. Let startInstant be ! CreateTemporalInstant(ns1).
    auto* start_instant = MUST(create_temporal_instant(vm, nanoseconds1));

    // 3. Let startDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, startInstant, calendar).
    auto* start_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *start_instant, calendar));

    // 4. Let endInstant be ! CreateTemporalInstant(ns2).
    auto* end_instant = MUST(create_temporal_instant(vm, nanoseconds2));

    // 5. Let endDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(timeZone, endInstant, calendar).
    auto* end_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &time_zone, *end_instant, calendar));

    // 6. Let dateDifference be ? DifferenceISODateTime(startDateTime.[[ISOYear]], startDateTime.[[ISOMonth]], startDateTime.[[ISODay]], startDateTime.[[ISOHour]], startDateTime.[[ISOMinute]], startDateTime.[[ISOSecond]], startDateTime.[[ISOMillisecond]], startDateTime.[[ISOMicrosecond]], startDateTime.[[ISONanosecond]], endDateTime.[[ISOYear]], endDateTime.[[ISOMonth]], endDateTime.[[ISODay]], endDateTime.[[ISOHour]], endDateTime.[[ISOMinute]], endDateTime.[[ISOSecond]], endDateTime.[[ISOMillisecond]], endDateTime.[[ISOMicrosecond]], endDateTime.[[ISONanosecond]], calendar, largestUnit, options).
    auto date_difference = TRY(difference_iso_date_time(vm, start_date_time->iso_year(), start_date_time->iso_month(), start_date_time->iso_day(), start_date_time->iso_hour(), start_date_time->iso_minute(), start_date_time->iso_second(), start_date_time->iso_millisecond(), start_date_time->iso_microsecond(), start_date_time->iso_nanosecond(), end_date_time->iso_year(), end_date_time->iso_month(), end_date_time->iso_day(), end_date_time->iso_hour(), end_date_time->iso_minute(), end_date_time->iso_second(), end_date_time->iso_millisecond(), end_date_time->iso_microsecond(), end_date_time->iso_nanosecond(), calendar, largest_unit, options));

    // 7. Let intermediateNs be ? AddZonedDateTime(ns1, timeZone, calendar, dateDifference.[[Years]], dateDifference.[[Months]], dateDifference.[[Weeks]], 0, 0, 0, 0, 0, 0, 0).
    auto* intermediate_ns = TRY(add_zoned_date_time(vm, nanoseconds1, &time_zone, calendar, date_difference.years, date_difference.months, date_difference.weeks, 0, 0, 0, 0, 0, 0, 0));

    // 8. Let timeRemainderNs be ns2 - intermediateNs.
    auto time_remainder_ns = nanoseconds2.big_integer().minus(intermediate_ns->big_integer());

    // 9. Let intermediate be ! CreateTemporalZonedDateTime(intermediateNs, timeZone, calendar).
    auto* intermediate = MUST(create_temporal_zoned_date_time(vm, *intermediate_ns, time_zone, calendar));

    // 10. Let result be ? NanosecondsToDays(timeRemainderNs, intermediate).
    auto result = TRY(nanoseconds_to_days(vm, time_remainder_ns, intermediate));

    // 11. Let timeDifference be ! BalanceDuration(0, 0, 0, 0, 0, 0, result.[[Nanoseconds]], "hour").
    auto time_difference = MUST(balance_duration(vm, 0, 0, 0, 0, 0, 0, result.nanoseconds, "hour"sv));

    // 12. Return ! CreateDurationRecord(dateDifference.[[Years]], dateDifference.[[Months]], dateDifference.[[Weeks]], result.[[Days]], timeDifference.[[Hours]], timeDifference.[[Minutes]], timeDifference.[[Seconds]], timeDifference.[[Milliseconds]], timeDifference.[[Microseconds]], timeDifference.[[Nanoseconds]]).
    return create_duration_record(date_difference.years, date_difference.months, date_difference.weeks, result.days, time_difference.hours, time_difference.minutes, time_difference.seconds, time_difference.milliseconds, time_difference.microseconds, time_difference.nanoseconds);
}

// 6.5.7 NanosecondsToDays ( nanoseconds, relativeTo ), https://tc39.es/proposal-temporal/#sec-temporal-nanosecondstodays
ThrowCompletionOr<NanosecondsToDaysResult> nanoseconds_to_days(VM& vm, Crypto::SignedBigInteger nanoseconds, Value relative_to_value)
{
    auto& realm = *vm.current_realm();

    // 1. Let dayLengthNs be nsPerDay.
    auto day_length_ns = ns_per_day_bigint;

    // 2. If nanoseconds = 0, then
    if (nanoseconds.is_zero()) {
        // a. Return the Record { [[Days]]: 0, [[Nanoseconds]]: 0, [[DayLength]]: dayLengthNs }.
        return NanosecondsToDaysResult { .days = 0, .nanoseconds = "0"_sbigint, .day_length = day_length_ns.to_double() };
    }

    // 3. If nanoseconds < 0, let sign be -1; else, let sign be 1.
    auto sign = nanoseconds.is_negative() ? -1 : 1;

    // 4. If Type(relativeTo) is not Object or relativeTo does not have an [[InitializedTemporalZonedDateTime]] internal slot, then
    if (!relative_to_value.is_object() || !is<ZonedDateTime>(relative_to_value.as_object())) {
        // a. Return the Record { [[Days]]: truncate(nanoseconds / dayLengthNs), [[Nanoseconds]]: (abs(nanoseconds) modulo dayLengthNs) × sign, [[DayLength]]: dayLengthNs }.
        return NanosecondsToDaysResult {
            .days = nanoseconds.divided_by(day_length_ns).quotient.to_double(),
            .nanoseconds = Crypto::SignedBigInteger { nanoseconds.unsigned_value() }.divided_by(day_length_ns).remainder.multiplied_by(Crypto::SignedBigInteger { sign }),
            .day_length = day_length_ns.to_double()
        };
    }

    auto& relative_to = static_cast<ZonedDateTime&>(relative_to_value.as_object());

    // 5. Let startNs be ℝ(relativeTo.[[Nanoseconds]]).
    auto& start_ns = relative_to.nanoseconds().big_integer();

    // 6. Let startInstant be ! CreateTemporalInstant(ℤ(startNs)).
    auto* start_instant = MUST(create_temporal_instant(vm, BigInt::create(vm, start_ns)));

    // 7. Let startDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(relativeTo.[[TimeZone]], startInstant, relativeTo.[[Calendar]]).
    auto* start_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &relative_to.time_zone(), *start_instant, relative_to.calendar()));

    // 8. Let endNs be startNs + nanoseconds.
    auto end_ns = start_ns.plus(nanoseconds);

    auto end_ns_bigint = BigInt::create(vm, end_ns);

    // 9. If ! IsValidEpochNanoseconds(ℤ(endNs)) is false, throw a RangeError exception.
    if (!is_valid_epoch_nanoseconds(end_ns_bigint))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

    // 10. Let endInstant be ! CreateTemporalInstant(ℤ(endNs)).
    auto* end_instant = MUST(create_temporal_instant(vm, end_ns_bigint));

    // 11. Let endDateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(relativeTo.[[TimeZone]], endInstant, relativeTo.[[Calendar]]).
    auto* end_date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, &relative_to.time_zone(), *end_instant, relative_to.calendar()));

    // 12. Let dateDifference be ? DifferenceISODateTime(startDateTime.[[ISOYear]], startDateTime.[[ISOMonth]], startDateTime.[[ISODay]], startDateTime.[[ISOHour]], startDateTime.[[ISOMinute]], startDateTime.[[ISOSecond]], startDateTime.[[ISOMillisecond]], startDateTime.[[ISOMicrosecond]], startDateTime.[[ISONanosecond]], endDateTime.[[ISOYear]], endDateTime.[[ISOMonth]], endDateTime.[[ISODay]], endDateTime.[[ISOHour]], endDateTime.[[ISOMinute]], endDateTime.[[ISOSecond]], endDateTime.[[ISOMillisecond]], endDateTime.[[ISOMicrosecond]], endDateTime.[[ISONanosecond]], relativeTo.[[Calendar]], "day", OrdinaryObjectCreate(null)).
    auto date_difference = TRY(difference_iso_date_time(vm, start_date_time->iso_year(), start_date_time->iso_month(), start_date_time->iso_day(), start_date_time->iso_hour(), start_date_time->iso_minute(), start_date_time->iso_second(), start_date_time->iso_millisecond(), start_date_time->iso_microsecond(), start_date_time->iso_nanosecond(), end_date_time->iso_year(), end_date_time->iso_month(), end_date_time->iso_day(), end_date_time->iso_hour(), end_date_time->iso_minute(), end_date_time->iso_second(), end_date_time->iso_millisecond(), end_date_time->iso_microsecond(), end_date_time->iso_nanosecond(), relative_to.calendar(), "day"sv, *Object::create(realm, nullptr)));

    // 13. Let days be dateDifference.[[Days]].
    auto days = date_difference.days;

    // 14. Let intermediateNs be ℝ(? AddZonedDateTime(ℤ(startNs), relativeTo.[[TimeZone]], relativeTo.[[Calendar]], 0, 0, 0, days, 0, 0, 0, 0, 0, 0)).
    auto intermediate_ns = TRY(add_zoned_date_time(vm, BigInt::create(vm, start_ns), &relative_to.time_zone(), relative_to.calendar(), 0, 0, 0, days, 0, 0, 0, 0, 0, 0))->big_integer();

    // 15. If sign is 1, then
    if (sign == 1) {
        // a. Repeat, while days > 0 and intermediateNs > endNs,
        while (days > 0 && intermediate_ns > end_ns) {
            // i. Set days to days - 1.
            days--;

            // ii. Set intermediateNs to ℝ(? AddZonedDateTime(ℤ(startNs), relativeTo.[[TimeZone]], relativeTo.[[Calendar]], 0, 0, 0, days, 0, 0, 0, 0, 0, 0)).
            intermediate_ns = TRY(add_zoned_date_time(vm, BigInt::create(vm, start_ns), &relative_to.time_zone(), relative_to.calendar(), 0, 0, 0, days, 0, 0, 0, 0, 0, 0))->big_integer();
        }
    }

    // 16. Set nanoseconds to endNs - intermediateNs.
    nanoseconds = end_ns.minus(intermediate_ns);

    // 17. Let done be false.
    // 18. Repeat, while done is false,
    while (true) {
        // a. Let oneDayFartherNs be ℝ(? AddZonedDateTime(ℤ(intermediateNs), relativeTo.[[TimeZone]], relativeTo.[[Calendar]], 0, 0, 0, sign, 0, 0, 0, 0, 0, 0)).
        auto one_day_farther_ns = TRY(add_zoned_date_time(vm, BigInt::create(vm, intermediate_ns), &relative_to.time_zone(), relative_to.calendar(), 0, 0, 0, sign, 0, 0, 0, 0, 0, 0))->big_integer();

        // b. Set dayLengthNs to oneDayFartherNs - intermediateNs.
        day_length_ns = one_day_farther_ns.minus(intermediate_ns);

        // c. If (nanoseconds - dayLengthNs) × sign ≥ 0, then
        if (nanoseconds.minus(day_length_ns).multiplied_by(Crypto::SignedBigInteger { sign }) >= "0"_sbigint) {
            // i. Set nanoseconds to nanoseconds - dayLengthNs.
            nanoseconds = nanoseconds.minus(day_length_ns);

            // ii. Set intermediateNs to oneDayFartherNs.
            intermediate_ns = move(one_day_farther_ns);

            // iii. Set days to days + sign.
            days += sign;
        }
        // d. Else,
        else {
            // i. Set done to true.
            break;
        }
    }

    // 19. If days < 0 and sign = 1, throw a RangeError exception.
    if (days < 0 && sign == 1)
        return vm.throw_completion<RangeError>(ErrorType::TemporalNanosecondsConvertedToDaysWithOppositeSign);

    // 20. If days > 0 and sign = -1, throw a RangeError exception.
    if (days > 0 && sign == -1)
        return vm.throw_completion<RangeError>(ErrorType::TemporalNanosecondsConvertedToDaysWithOppositeSign);

    // 21. If nanoseconds < 0 and sign = 1, throw a RangeError exception.
    if (nanoseconds.is_negative() && sign == 1)
        return vm.throw_completion<RangeError>(ErrorType::TemporalNanosecondsConvertedToRemainderOfNanosecondsWithOppositeSign);

    // 22. If nanoseconds > 0 and sign = -1, throw a RangeError exception.
    if (nanoseconds.is_positive() && sign == -1)
        return vm.throw_completion<RangeError>(ErrorType::TemporalNanosecondsConvertedToRemainderOfNanosecondsWithOppositeSign);

    // 23. If abs(nanoseconds) ≥ abs(dayLengthNs), throw a RangeError exception.
    auto compare_result = nanoseconds.unsigned_value().compare_to_double(fabs(day_length_ns.to_double()));
    if (compare_result == Crypto::UnsignedBigInteger::CompareResult::DoubleLessThanBigInt || compare_result == Crypto::UnsignedBigInteger::CompareResult::DoubleEqualsBigInt)
        return vm.throw_completion<RangeError>(ErrorType::TemporalNanosecondsConvertedToRemainderOfNanosecondsLongerThanDayLength);

    // 24. Return the Record { [[Days]]: days, [[Nanoseconds]]: nanoseconds, [[DayLength]]: abs(dayLengthNs) }.
    return NanosecondsToDaysResult { .days = days, .nanoseconds = move(nanoseconds), .day_length = fabs(day_length_ns.to_double()) };
}

// 6.5.8 DifferenceTemporalZonedDateTime ( operation, zonedDateTime, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalzoneddatetime
ThrowCompletionOr<NonnullGCPtr<Duration>> difference_temporal_zoned_date_time(VM& vm, DifferenceOperation operation, ZonedDateTime& zoned_date_time, Value other_value, Value options_value)
{
    // 1. If operation is since, let sign be -1. Otherwise, let sign be 1.
    i8 sign = operation == DifferenceOperation::Since ? -1 : 1;

    // 2. Set other to ? ToTemporalZonedDateTime(other).
    auto* other = TRY(to_temporal_zoned_date_time(vm, other_value));

    // 3. If ? CalendarEquals(zonedDateTime.[[Calendar]], other.[[Calendar]]) is false, then
    if (!TRY(calendar_equals(vm, zoned_date_time.calendar(), other->calendar()))) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalDifferentCalendars);
    }

    // 4. Let settings be ? GetDifferenceSettings(operation, options, datetime, « », "nanosecond", "hour").
    auto settings = TRY(get_difference_settings(vm, operation, options_value, UnitGroup::DateTime, {}, { "nanosecond"sv }, "hour"sv));

    // 5. If settings.[[LargestUnit]] is not one of "year", "month", "week", or "day", then
    if (!settings.largest_unit.is_one_of("year"sv, "month"sv, "week"sv, "day"sv)) {
        // a. Let result be DifferenceInstant(zonedDateTime.[[Nanoseconds]], other.[[Nanoseconds]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[LargestUnit]], settings.[[RoundingMode]]).
        auto result = difference_instant(vm, zoned_date_time.nanoseconds(), other->nanoseconds(), settings.rounding_increment, settings.smallest_unit, settings.largest_unit, settings.rounding_mode);

        // b. Return ! CreateTemporalDuration(0, 0, 0, 0, sign × result.[[Hours]], sign × result.[[Minutes]], sign × result.[[Seconds]], sign × result.[[Milliseconds]], sign × result.[[Microseconds]], sign × result.[[Nanoseconds]]).
        return create_temporal_duration(vm, 0, 0, 0, 0, sign * result.hours, sign * result.minutes, sign * result.seconds, sign * result.milliseconds, sign * result.microseconds, sign * result.nanoseconds);
    }

    // 6. If ? TimeZoneEquals(zonedDateTime.[[TimeZone]], other.[[TimeZone]]) is false, then
    if (!TRY(time_zone_equals(vm, zoned_date_time.time_zone(), other->time_zone()))) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalDifferentTimeZones);
    }

    // 7. Let untilOptions be ? MergeLargestUnitOption(settings.[[Options]], settings.[[LargestUnit]]).
    auto* until_options = TRY(merge_largest_unit_option(vm, settings.options, settings.largest_unit));

    // 8. Let difference be ? DifferenceZonedDateTime(zonedDateTime.[[Nanoseconds]], other.[[Nanoseconds]], zonedDateTime.[[TimeZone]], zonedDateTime.[[Calendar]], settings.[[LargestUnit]], untilOptions).
    auto difference = TRY(difference_zoned_date_time(vm, zoned_date_time.nanoseconds(), other->nanoseconds(), zoned_date_time.time_zone(), zoned_date_time.calendar(), settings.largest_unit, *until_options));

    auto calendar_record = TRY(create_calendar_methods_record(vm, NonnullGCPtr<Object> { zoned_date_time.calendar() }, { { CalendarMethod::DateAdd, CalendarMethod::DateFromFields, CalendarMethod::DateUntil, CalendarMethod::Fields } }));

    // 9. Let roundResult be (? RoundDuration(difference.[[Years]], difference.[[Months]], difference.[[Weeks]], difference.[[Days]], difference.[[Hours]], difference.[[Minutes]], difference.[[Seconds]], difference.[[Milliseconds]], difference.[[Microseconds]], difference.[[Nanoseconds]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]], zonedDateTime)).[[DurationRecord]].
    auto round_result = TRY(round_duration(vm, difference.years, difference.months, difference.weeks, difference.days, difference.hours, difference.minutes, difference.seconds, difference.milliseconds, difference.microseconds, difference.nanoseconds, settings.rounding_increment, settings.smallest_unit, settings.rounding_mode, &zoned_date_time, calendar_record)).duration_record;

    // 10. Let result be ? AdjustRoundedDurationDays(roundResult.[[Years]], roundResult.[[Months]], roundResult.[[Weeks]], roundResult.[[Days]], roundResult.[[Hours]], roundResult.[[Minutes]], roundResult.[[Seconds]], roundResult.[[Milliseconds]], roundResult.[[Microseconds]], roundResult.[[Nanoseconds]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]], zonedDateTime).
    auto result = TRY(adjust_rounded_duration_days(vm, round_result.years, round_result.months, round_result.weeks, round_result.days, round_result.hours, round_result.minutes, round_result.seconds, round_result.milliseconds, round_result.microseconds, round_result.nanoseconds, settings.rounding_increment, settings.smallest_unit, settings.rounding_mode, &zoned_date_time));

    // 11. Return ! CreateTemporalDuration(sign × result.[[Years]], sign × result.[[Months]], sign × result.[[Weeks]], sign × result.[[Days]], sign × result.[[Hours]], sign × result.[[Minutes]], sign × result.[[Seconds]], sign × result.[[Milliseconds]], sign × result.[[Microseconds]], sign × result.[[Nanoseconds]]).
    return MUST(create_temporal_duration(vm, sign * result.years, sign * result.months, sign * result.weeks, sign * result.days, sign * result.hours, sign * result.minutes, sign * result.seconds, sign * result.milliseconds, sign * result.microseconds, sign * result.nanoseconds));
}

// 6.5.9 AddDurationToOrSubtractDurationFromZonedDateTime ( operation, zonedDateTime, temporalDurationLike, options ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtoOrsubtractdurationfromzoneddatetime
ThrowCompletionOr<ZonedDateTime*> add_duration_to_or_subtract_duration_from_zoned_date_time(VM& vm, ArithmeticOperation operation, ZonedDateTime& zoned_date_time, Value temporal_duration_like, Value options_value)
{
    // 1. If operation is subtract, let sign be -1. Otherwise, let sign be 1.
    i8 sign = operation == ArithmeticOperation::Subtract ? -1 : 1;

    // 2. Let duration be ? ToTemporalDurationRecord(temporalDurationLike).
    auto duration = TRY(to_temporal_duration_record(vm, temporal_duration_like));

    // 3. Set options to ? GetOptionsObject(options).
    auto* options = TRY(get_options_object(vm, options_value));

    // 4. Let timeZone be zonedDateTime.[[TimeZone]].
    auto& time_zone = zoned_date_time.time_zone();

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto& calendar = zoned_date_time.calendar();

    // 6. Let epochNanoseconds be ? AddZonedDateTime(zonedDateTime.[[Nanoseconds]], timeZone, calendar, sign × duration.[[Years]], sign × duration.[[Months]], sign × duration.[[Weeks]], sign × duration.[[Days]], sign × duration.[[Hours]], sign × duration.[[Minutes]], sign × duration.[[Seconds]], sign × duration.[[Milliseconds]], sign × duration.[[Microseconds]], sign × duration.[[Nanoseconds]], options).
    auto* epoch_nanoseconds = TRY(add_zoned_date_time(vm, zoned_date_time.nanoseconds(), &time_zone, calendar, sign * duration.years, sign * duration.months, sign * duration.weeks, sign * duration.days, sign * duration.hours, sign * duration.minutes, sign * duration.seconds, sign * duration.milliseconds, sign * duration.microseconds, sign * duration.nanoseconds, options));

    // 7. Return ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
    return MUST(create_temporal_zoned_date_time(vm, *epoch_nanoseconds, time_zone, calendar));
}

}
