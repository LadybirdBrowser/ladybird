/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibJS/Runtime/Temporal/ZonedDateTimeConstructor.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(ZonedDateTime);

// 6 Temporal.ZonedDateTime Objects, https://tc39.es/proposal-temporal/#sec-temporal-zoneddatetime-objects
ZonedDateTime::ZonedDateTime(BigInt const& epoch_nanoseconds, String time_zone, String calendar, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_epoch_nanoseconds(epoch_nanoseconds)
    , m_time_zone(move(time_zone))
    , m_calendar(move(calendar))
{
}

void ZonedDateTime::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_epoch_nanoseconds);
}

// 6.5.1 InterpretISODateTimeOffset ( isoDate, time, offsetBehaviour, offsetNanoseconds, timeZone, disambiguation, offsetOption, matchBehaviour ), https://tc39.es/proposal-temporal/#sec-temporal-interpretisodatetimeoffset
ThrowCompletionOr<Crypto::SignedBigInteger> interpret_iso_date_time_offset(VM& vm, ISODate iso_date, Variant<ParsedISODateTime::StartOfDay, Time> const& time_or_start_of_day, OffsetBehavior offset_behavior, double offset_nanoseconds, StringView time_zone, Disambiguation disambiguation, OffsetOption offset_option, MatchBehavior match_behavior)
{
    // 1. If time is START-OF-DAY, then
    if (time_or_start_of_day.has<ParsedISODateTime::StartOfDay>()) {
        // a. Assert: offsetBehaviour is WALL.
        VERIFY(offset_behavior == OffsetBehavior::Wall);

        // b. Assert: offsetNanoseconds is 0.
        VERIFY(offset_nanoseconds == 0);

        // c. Return ? GetStartOfDay(timeZone, isoDate).
        return TRY(get_start_of_day(vm, time_zone, iso_date));
    }

    auto time = time_or_start_of_day.get<Time>();

    // 2. Let isoDateTime be CombineISODateAndTimeRecord(isoDate, time).
    auto iso_date_time = combine_iso_date_and_time_record(iso_date, time);

    // 3. If offsetBehaviour is WALL, or offsetBehaviour is OPTION and offsetOption is IGNORE, then
    if (offset_behavior == OffsetBehavior::Wall || (offset_behavior == OffsetBehavior::Option && offset_option == OffsetOption::Ignore)) {
        // a. Return ? GetEpochNanosecondsFor(timeZone, isoDateTime, disambiguation).
        return TRY(get_epoch_nanoseconds_for(vm, time_zone, iso_date_time, disambiguation));
    }

    // 4. If offsetBehaviour is EXACT, or offsetBehaviour is OPTION and offsetOption is USE, then
    if (offset_behavior == OffsetBehavior::Exact || (offset_behavior == OffsetBehavior::Option && offset_option == OffsetOption::Use)) {
        // a. Let balanced be BalanceISODateTime(isoDate.[[Year]], isoDate.[[Month]], isoDate.[[Day]], time.[[Hour]], time.[[Minute]], time.[[Second]], time.[[Millisecond]], time.[[Microsecond]], time.[[Nanosecond]] - offsetNanoseconds).
        auto balanced = balance_iso_date_time(iso_date.year, iso_date.month, iso_date.day, time.hour, time.minute, time.second, time.millisecond, time.microsecond, static_cast<double>(time.nanosecond) - offset_nanoseconds);

        // b. Perform ? CheckISODaysRange(balanced.[[ISODate]]).
        TRY(check_iso_days_range(vm, balanced.iso_date));

        // c. Let epochNanoseconds be GetUTCEpochNanoseconds(balanced).
        auto epoch_nanoseconds = get_utc_epoch_nanoseconds(balanced);

        // d. If IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
        if (!is_valid_epoch_nanoseconds(epoch_nanoseconds))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

        // e. Return epochNanoseconds.
        return epoch_nanoseconds;
    }

    // 5. Assert: offsetBehaviour is OPTION.
    VERIFY(offset_behavior == OffsetBehavior::Option);

    // 6. Assert: offsetOption is PREFER or REJECT.
    VERIFY(offset_option == OffsetOption::Prefer || offset_option == OffsetOption::Reject);

    // 7. Perform ? CheckISODaysRange(isoDate).
    TRY(check_iso_days_range(vm, iso_date));

    // 8. Let utcEpochNanoseconds be GetUTCEpochNanoseconds(isoDateTime).
    auto utc_epoch_nanoseconds = get_utc_epoch_nanoseconds(iso_date_time);

    // 9. Let possibleEpochNs be ? GetPossibleEpochNanoseconds(timeZone, isoDateTime).
    auto possible_epoch_nanoseconds = TRY(get_possible_epoch_nanoseconds(vm, time_zone, iso_date_time));

    // 10. For each element candidate of possibleEpochNs, do
    for (auto& candidate : possible_epoch_nanoseconds) {
        // a. Let candidateOffset be utcEpochNanoseconds - candidate.
        auto candidate_offset = utc_epoch_nanoseconds.minus(candidate);

        // b. If candidateOffset = offsetNanoseconds, then
        if (candidate_offset.compare_to_double(offset_nanoseconds) == Crypto::UnsignedBigInteger::CompareResult::DoubleEqualsBigInt) {
            // i. Return candidate.
            return move(candidate);
        }

        // c. If matchBehaviour is MATCH-MINUTES, then
        if (match_behavior == MatchBehavior::MatchMinutes) {
            // i. Let roundedCandidateNanoseconds be RoundNumberToIncrement(candidateOffset, 60 × 10**9, HALF-EXPAND).
            auto rounded_candidate_nanoseconds = round_number_to_increment(candidate_offset, NANOSECONDS_PER_MINUTE, RoundingMode::HalfExpand);

            // ii. If roundedCandidateNanoseconds = offsetNanoseconds, then
            if (rounded_candidate_nanoseconds.compare_to_double(offset_nanoseconds) == Crypto::UnsignedBigInteger::CompareResult::DoubleEqualsBigInt) {
                // 1. Return candidate.
                return move(candidate);
            }
        }
    }

    // 11. If offsetOption is reject, throw a RangeError exception.
    if (offset_option == OffsetOption::Reject)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidZonedDateTimeOffset);

    // 12. Return ? DisambiguatePossibleEpochNanoseconds(possibleEpochNs, timeZone, isoDateTime, disambiguation).
    return TRY(disambiguate_possible_epoch_nanoseconds(vm, move(possible_epoch_nanoseconds), time_zone, iso_date_time, disambiguation));
}

// 6.5.2 ToTemporalZonedDateTime ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalzoneddatetime
ThrowCompletionOr<GC::Ref<ZonedDateTime>> to_temporal_zoned_date_time(VM& vm, Value item, Value options)
{
    // 1. If options is not present, set options to undefined.

    // 2. Let offsetBehaviour be OPTION.
    auto offset_behavior = OffsetBehavior::Option;

    // 3. Let matchBehaviour be MATCH-EXACTLY.
    auto match_behavior = MatchBehavior::MatchExactly;

    String calendar;
    String time_zone;
    Optional<String> offset_string;

    Disambiguation disambiguation;
    OffsetOption offset_option;

    ISODate iso_date;
    Variant<ParsedISODateTime::StartOfDay, Time> time { Time {} };

    // 4. If item is an Object, then
    if (item.is_object()) {
        auto const& object = item.as_object();

        // a. If item has an [[InitializedTemporalZonedDateTime]] internal slot, then
        if (is<ZonedDateTime>(object)) {
            auto const& zoned_date_time = static_cast<ZonedDateTime const&>(object);

            // i. NOTE: The following steps, and similar ones below, read options and perform independent validation in
            //    alphabetical order (GetTemporalDisambiguationOption reads "disambiguation", GetTemporalOffsetOption
            //    reads "offset", and GetTemporalOverflowOption reads "overflow").

            // ii. Let resolvedOptions be ? GetOptionsObject(options).
            auto resolved_options = TRY(get_options_object(vm, options));

            // iii. Perform ? GetTemporalDisambiguationOption(resolvedOptions).
            TRY(get_temporal_disambiguation_option(vm, resolved_options));

            // iv. Perform ? GetTemporalOffsetOption(resolvedOptions, REJECT).
            TRY(get_temporal_offset_option(vm, resolved_options, OffsetOption::Reject));

            // v. Perform ? GetTemporalOverflowOption(resolvedOptions).
            TRY(get_temporal_overflow_option(vm, resolved_options));

            // vi. Return ! CreateTemporalZonedDateTime(item.[[EpochNanoseconds]], item.[[TimeZone]], item.[[Calendar]]).
            return MUST(create_temporal_zoned_date_time(vm, zoned_date_time.epoch_nanoseconds(), zoned_date_time.time_zone(), zoned_date_time.calendar()));
        }

        // b. Let calendar be ? GetTemporalCalendarIdentifierWithISODefault(item).
        calendar = TRY(get_temporal_calendar_identifier_with_iso_default(vm, object));

        // c. Let fields be ? PrepareCalendarFields(calendar, item, « YEAR, MONTH, MONTH-CODE, DAY », « HOUR, MINUTE, SECOND, MILLISECOND, MICROSECOND, NANOSECOND, OFFSET, TIME-ZONE », « TIME-ZONE »).
        static constexpr auto calendar_field_names = to_array({ CalendarField::Year, CalendarField::Month, CalendarField::MonthCode, CalendarField::Day });
        static constexpr auto non_calendar_field_names = to_array({ CalendarField::Hour, CalendarField::Minute, CalendarField::Second, CalendarField::Millisecond, CalendarField::Microsecond, CalendarField::Nanosecond, CalendarField::Offset, CalendarField::TimeZone });
        static constexpr auto required_field_names = to_array({ CalendarField::TimeZone });
        auto fields = TRY(prepare_calendar_fields(vm, calendar, object, calendar_field_names, non_calendar_field_names, required_field_names.span()));

        // d. Let timeZone be fields.[[TimeZone]].
        time_zone = fields.time_zone.release_value();

        // e. Let offsetString be fields.[[OffsetString]].
        offset_string = move(fields.offset_string);

        // f. If offsetString is UNSET, then
        if (!offset_string.has_value()) {
            // i. Set offsetBehaviour to WALL.
            offset_behavior = OffsetBehavior::Wall;
        }

        // g. Let resolvedOptions be ? GetOptionsObject(options).
        auto resolved_options = TRY(get_options_object(vm, options));

        // h. Let disambiguation be ? GetTemporalDisambiguationOption(resolvedOptions).
        disambiguation = TRY(get_temporal_disambiguation_option(vm, resolved_options));

        // i. Let offsetOption be ? GetTemporalOffsetOption(resolvedOptions, REJECT).
        offset_option = TRY(get_temporal_offset_option(vm, resolved_options, OffsetOption::Reject));

        // j. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

        // k. Let result be ? InterpretTemporalDateTimeFields(calendar, fields, overflow).
        auto result = TRY(interpret_temporal_date_time_fields(vm, calendar, fields, overflow));

        // l. Let isoDate be result.[[ISODate]].
        iso_date = result.iso_date;

        // m. Let time be result.[[Time]].
        time = result.time;
    }
    // 5. Else,
    else {
        // a. If item is not a String, throw a TypeError exception.
        if (!item.is_string())
            return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidZonedDateTimeString, item);

        // b. Let result be ? ParseISODateTime(item, « TemporalDateTimeString[+Zoned] »).
        auto result = TRY(parse_iso_date_time(vm, item.as_string().utf8_string_view(), { { Production::TemporalZonedDateTimeString } }));

        // c. Let annotation be result.[[TimeZone]].[[TimeZoneAnnotation]].
        auto annotation = move(result.time_zone.time_zone_annotation);

        // d. Assert: annotation is not empty.
        VERIFY(annotation.has_value());

        // e. Let timeZone be ? ToTemporalTimeZoneIdentifier(annotation).
        time_zone = TRY(to_temporal_time_zone_identifier(vm, *annotation));

        // f. Let offsetString be result.[[TimeZone]].[[OffsetString]].
        offset_string = move(result.time_zone.offset_string);

        // g. If result.[[TimeZone]].[[Z]] is true, then
        if (result.time_zone.z_designator) {
            // i. Set offsetBehaviour to EXACT.
            offset_behavior = OffsetBehavior::Exact;
        }
        // h. Else if offsetString is EMPTY, then
        else if (!offset_string.has_value()) {
            // i. Set offsetBehaviour to WALL.
            offset_behavior = OffsetBehavior::Wall;
        }

        // i. Let calendar be result.[[Calendar]].
        // j. If calendar is empty, set calendar to "iso8601".
        calendar = result.calendar.value_or("iso8601"_string);

        // k. Set calendar to ? CanonicalizeCalendar(calendar).
        calendar = TRY(canonicalize_calendar(vm, calendar));

        // l. Set matchBehaviour to MATCH-MINUTES.
        match_behavior = MatchBehavior::MatchMinutes;

        // m. Let resolvedOptions be ? GetOptionsObject(options).
        auto resolved_options = TRY(get_options_object(vm, options));

        // n. Let disambiguation be ? GetTemporalDisambiguationOption(resolvedOptions).
        disambiguation = TRY(get_temporal_disambiguation_option(vm, resolved_options));

        // o. Let offsetOption be ? GetTemporalOffsetOption(resolvedOptions, REJECT).
        offset_option = TRY(get_temporal_offset_option(vm, resolved_options, OffsetOption::Reject));

        // p. Perform ? GetTemporalOverflowOption(resolvedOptions).
        TRY(get_temporal_overflow_option(vm, resolved_options));

        // q. Let isoDate be CreateISODateRecord(result.[[Year]], result.[[Month]], result.[[Day]]).
        iso_date = create_iso_date_record(*result.year, result.month, result.day);

        // r. Let time be result.[[Time]].
        time = result.time;
    }

    // 6. Let offsetNanoseconds be 0.
    double offset_nanoseconds = 0;

    // 7. If offsetBehaviour is OPTION, then
    if (offset_behavior == OffsetBehavior::Option) {
        // a. Set offsetNanoseconds to ! ParseDateTimeUTCOffset(offsetString).
        offset_nanoseconds = parse_date_time_utc_offset(*offset_string);
    }

    // 8. Let epochNanoseconds be ? InterpretISODateTimeOffset(isoDate, time, offsetBehaviour, offsetNanoseconds, timeZone, disambiguation, offsetOption, matchBehaviour).
    auto epoch_nanoseconds = TRY(interpret_iso_date_time_offset(vm, iso_date, time, offset_behavior, offset_nanoseconds, time_zone, disambiguation, offset_option, match_behavior));

    // 9. Return ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
    return MUST(create_temporal_zoned_date_time(vm, BigInt::create(vm, move(epoch_nanoseconds)), move(time_zone), move(calendar)));
}

// 6.5.3 CreateTemporalZonedDateTime ( epochNanoseconds, timeZone, calendar [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporalzoneddatetime
ThrowCompletionOr<GC::Ref<ZonedDateTime>> create_temporal_zoned_date_time(VM& vm, BigInt const& epoch_nanoseconds, String time_zone, String calendar, GC::Ptr<FunctionObject> new_target)
{
    auto& realm = *vm.current_realm();

    // 1. Assert: IsValidEpochNanoseconds(epochNanoseconds) is true.
    VERIFY(is_valid_epoch_nanoseconds(epoch_nanoseconds.big_integer()));

    // 2. If newTarget is not present, set newTarget to %Temporal.ZonedDateTime%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_zoned_date_time_constructor();

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.ZonedDateTime.prototype%", « [[InitializedTemporalZonedDateTime]], [[EpochNanoseconds]], [[TimeZone]], [[Calendar]] »).
    // 4. Set object.[[EpochNanoseconds]] to epochNanoseconds.
    // 5. Set object.[[TimeZone]] to timeZone.
    // 6. Set object.[[Calendar]] to calendar.
    auto object = TRY(ordinary_create_from_constructor<ZonedDateTime>(vm, *new_target, &Intrinsics::temporal_zoned_date_time_prototype, epoch_nanoseconds, move(time_zone), move(calendar)));

    // 7. Return object.
    return object;
}

// 6.5.4 TemporalZonedDateTimeToString ( zonedDateTime, precision, showCalendar, showTimeZone, showOffset [ , increment [ , unit [ , roundingMode ] ] ] ), https://tc39.es/proposal-temporal/#sec-temporal-temporalzoneddatetimetostring
String temporal_zoned_date_time_to_string(ZonedDateTime const& zoned_date_time, SecondsStringPrecision::Precision precision, ShowCalendar show_calendar, ShowTimeZoneName show_time_zone, ShowOffset show_offset, u64 increment, Unit unit, RoundingMode rounding_mode)
{
    // 1. If increment is not present, set increment to 1.
    // 2. If unit is not present, set unit to NANOSECOND.
    // 3. If roundingMode is not present, set roundingMode to TRUNC.

    // 4. Let epochNs be zonedDateTime.[[EpochNanoseconds]].
    // 5. Set epochNs to RoundTemporalInstant(epochNs, increment, unit, roundingMode).
    auto epoch_nanoseconds = round_temporal_instant(zoned_date_time.epoch_nanoseconds()->big_integer(), increment, unit, rounding_mode);

    // 6. Let timeZone be zonedDateTime.[[TimeZone]].
    auto const& time_zone = zoned_date_time.time_zone();

    // 7. Let offsetNanoseconds be GetOffsetNanosecondsFor(timeZone, epochNs).
    auto offset_nanoseconds = get_offset_nanoseconds_for(time_zone, epoch_nanoseconds);

    // 8. Let isoDateTime be GetISODateTimeFor(timeZone, epochNs).
    auto iso_date_time = get_iso_date_time_for(time_zone, epoch_nanoseconds);

    // 9. Let dateTimeString be ISODateTimeToString(isoDateTime, "iso8601", precision, NEVER).
    auto date_time_string = iso_date_time_to_string(iso_date_time, "iso8601"sv, precision, ShowCalendar::Never);

    String offset_string;
    String time_zone_string;

    // 10. If showOffset is NEVER, then
    if (show_offset == ShowOffset::Never) {
        // a. Let offsetString be the empty String.
    }
    // 11. Else,
    else {
        // a. Let offsetString be FormatDateTimeUTCOffsetRounded(offsetNanoseconds).
        offset_string = format_date_time_utc_offset_rounded(offset_nanoseconds);
    }

    // 12. If showTimeZone is NEVER, then
    if (show_time_zone == ShowTimeZoneName::Never) {
        // a. Let timeZoneString be the empty String.
    }
    // 13. Else,
    else {
        // a. If showTimeZone is critical, let flag be "!"; else let flag be the empty String.
        auto flag = show_time_zone == ShowTimeZoneName::Critical ? "!"sv : ""sv;

        // b. Let timeZoneString be the string-concatenation of the code unit 0x005B (LEFT SQUARE BRACKET), flag,
        //    timeZone, and the code unit 0x005D (RIGHT SQUARE BRACKET).
        time_zone_string = MUST(String::formatted("[{}{}]", flag, time_zone));
    }

    // 14. Let calendarString be FormatCalendarAnnotation(zonedDateTime.[[Calendar]], showCalendar).
    auto calendar_string = format_calendar_annotation(zoned_date_time.calendar(), show_calendar);

    // 15. Return the string-concatenation of dateTimeString, offsetString, timeZoneString, and calendarString.
    return MUST(String::formatted("{}{}{}{}", date_time_string, offset_string, time_zone_string, calendar_string));
}

// 6.5.5 AddZonedDateTime ( epochNanoseconds, timeZone, calendar, duration, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-addzoneddatetime
ThrowCompletionOr<Crypto::SignedBigInteger> add_zoned_date_time(VM& vm, Crypto::SignedBigInteger const& epoch_nanoseconds, StringView time_zone, StringView calendar, InternalDuration const& duration, Overflow overflow)
{
    // 1. If DateDurationSign(duration.[[Date]]) = 0, then
    if (date_duration_sign(duration.date) == 0) {
        // a. Return ? AddInstant(epochNanoseconds, duration.[[Time]]).
        return TRY(add_instant(vm, epoch_nanoseconds, duration.time));
    }

    // 2. Let isoDateTime be GetISODateTimeFor(timeZone, epochNanoseconds).
    auto iso_date_time = get_iso_date_time_for(time_zone, epoch_nanoseconds);

    // 3. Let addedDate be ? CalendarDateAdd(calendar, isoDateTime.[[ISODate]], duration.[[Date]], overflow).
    auto added_date = TRY(calendar_date_add(vm, calendar, iso_date_time.iso_date, duration.date, overflow));

    // 4. Let intermediateDateTime be CombineISODateAndTimeRecord(addedDate, isoDateTime.[[Time]]).
    auto intermediate_date_time = combine_iso_date_and_time_record(added_date, iso_date_time.time);

    // 5. If ISODateTimeWithinLimits(intermediateDateTime) is false, throw a RangeError exception.
    if (!iso_date_time_within_limits(intermediate_date_time))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODateTime);

    // 6. Let intermediateNs be ! GetEpochNanosecondsFor(timeZone, intermediateDateTime, COMPATIBLE).
    auto intermediate_nanoseconds = MUST(get_epoch_nanoseconds_for(vm, time_zone, intermediate_date_time, Disambiguation::Compatible));

    // 7. Return ? AddInstant(intermediateNs, duration.[[Time]]).
    return TRY(add_instant(vm, intermediate_nanoseconds, duration.time));
}

// 6.5.6 DifferenceZonedDateTime ( ns1, ns2, timeZone, calendar, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-differencezoneddatetime
ThrowCompletionOr<InternalDuration> difference_zoned_date_time(VM& vm, Crypto::SignedBigInteger const& nanoseconds1, Crypto::SignedBigInteger const& nanoseconds2, StringView time_zone, StringView calendar, Unit largest_unit)
{
    // 1. If ns1 = ns2, return ! CombineDateAndTimeDuration(ZeroDateDuration(), 0).
    if (nanoseconds1 == nanoseconds2)
        return MUST(combine_date_and_time_duration(vm, zero_date_duration(vm), TimeDuration { 0 }));

    // 2. Let startDateTime be GetISODateTimeFor(timeZone, ns1).
    auto start_date_time = get_iso_date_time_for(time_zone, nanoseconds1);

    // 3. Let endDateTime be GetISODateTimeFor(timeZone, ns2).
    auto end_date_time = get_iso_date_time_for(time_zone, nanoseconds2);

    // 4. If ns2 - ns1 < 0, let sign be -1; else let sign be 1.
    double sign = nanoseconds2 < nanoseconds1 ? -1 : 1;

    // 5. If sign = 1, let maxDayCorrection be 2; else let maxDayCorrection be 1.
    double max_day_correction = sign == 1 ? 2 : 1;

    // 6. Let dayCorrection be 0.
    double day_correction = 0;

    // 7. Let timeDuration be DifferenceTime(startDateTime.[[Time]], endDateTime.[[Time]]).
    auto time_duration = difference_time(start_date_time.time, end_date_time.time);

    // 8. If TimeDurationSign(timeDuration) = -sign, set dayCorrection to dayCorrection + 1.
    if (time_duration_sign(time_duration) == -sign)
        ++day_correction;

    // 9. Let success be false.
    auto success = false;

    ISODateTime intermediate_date_time;

    // 10. Repeat, while dayCorrection ≤ maxDayCorrection and success is false,
    while (day_correction <= max_day_correction && !success) {
        // a. Let intermediateDate be BalanceISODate(endDateTime.[[ISODate]].[[Year]], endDateTime.[[ISODate]].[[Month]], endDateTime.[[ISODate]].[[Day]] - dayCorrection × sign).
        auto intermediate_date = balance_iso_date(end_date_time.iso_date.year, end_date_time.iso_date.month, static_cast<double>(end_date_time.iso_date.day) - (day_correction * sign));

        // b. Let intermediateDateTime be CombineISODateAndTimeRecord(intermediateDate, startDateTime.[[Time]]).
        intermediate_date_time = combine_iso_date_and_time_record(intermediate_date, start_date_time.time);

        // c. Let intermediateNs be ? GetEpochNanosecondsFor(timeZone, intermediateDateTime, COMPATIBLE).
        auto intermediate_nanoseconds = TRY(get_epoch_nanoseconds_for(vm, time_zone, intermediate_date_time, Disambiguation::Compatible));

        // d. Set timeDuration to TimeDurationFromEpochNanosecondsDifference(ns2, intermediateNs).
        time_duration = time_duration_from_epoch_nanoseconds_difference(nanoseconds2, intermediate_nanoseconds);

        // e. Let timeSign be TimeDurationSign(timeDuration).
        auto time_sign = time_duration_sign(time_duration);

        // f. If sign ≠ -timeSign, then
        if (sign != -time_sign) {
            // i. Set success to true.
            success = true;
        }

        // g. Set dayCorrection to dayCorrection + 1.
        ++day_correction;
    }

    // 11. Assert: success is true.
    VERIFY(success);

    // 12. Let dateLargestUnit be LargerOfTwoTemporalUnits(largestUnit, DAY).
    auto date_largest_unit = larger_of_two_temporal_units(largest_unit, Unit::Day);

    // 13. Let dateDifference be CalendarDateUntil(calendar, startDateTime.[[ISODate]], intermediateDateTime.[[ISODate]], dateLargestUnit).
    auto date_difference = calendar_date_until(vm, calendar, start_date_time.iso_date, intermediate_date_time.iso_date, date_largest_unit);

    // 14. Return ? CombineDateAndTimeDuration(dateDifference, timeDuration).
    return TRY(combine_date_and_time_duration(vm, date_difference, move(time_duration)));
}

// 6.5.7 DifferenceZonedDateTimeWithRounding ( ns1, ns2, timeZone, calendar, largestUnit, roundingIncrement, smallestUnit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-differencezoneddatetimewithrounding
ThrowCompletionOr<InternalDuration> difference_zoned_date_time_with_rounding(VM& vm, Crypto::SignedBigInteger const& nanoseconds1, Crypto::SignedBigInteger const& nanoseconds2, StringView time_zone, StringView calendar, Unit largest_unit, u64 rounding_increment, Unit smallest_unit, RoundingMode rounding_mode)
{
    // 1. If TemporalUnitCategory(largestUnit) is TIME, then
    if (temporal_unit_category(largest_unit) == UnitCategory::Time) {
        // a. Return DifferenceInstant(ns1, ns2, roundingIncrement, smallestUnit, roundingMode).
        return difference_instant(vm, nanoseconds1, nanoseconds2, rounding_increment, smallest_unit, rounding_mode);
    }

    // 2. Let difference be ? DifferenceZonedDateTime(ns1, ns2, timeZone, calendar, largestUnit).
    auto difference = TRY(difference_zoned_date_time(vm, nanoseconds1, nanoseconds2, time_zone, calendar, largest_unit));

    // 3. If smallestUnit is NANOSECOND and roundingIncrement = 1, return difference.
    if (smallest_unit == Unit::Nanosecond && rounding_increment == 1)
        return difference;

    // 4. Let dateTime be GetISODateTimeFor(timeZone, ns1).
    auto date_time = get_iso_date_time_for(time_zone, nanoseconds1);

    // 5. Return ? RoundRelativeDuration(difference, ns2, dateTime, timeZone, calendar, largestUnit, roundingIncrement, smallestUnit, roundingMode).
    return TRY(round_relative_duration(vm, difference, nanoseconds2, date_time, time_zone, calendar, largest_unit, rounding_increment, smallest_unit, rounding_mode));
}

// 6.5.8 DifferenceZonedDateTimeWithTotal ( ns1, ns2, timeZone, calendar, unit ), https://tc39.es/proposal-temporal/#sec-temporal-differencezoneddatetimewithtotal
ThrowCompletionOr<Crypto::BigFraction> difference_zoned_date_time_with_total(VM& vm, Crypto::SignedBigInteger const& nanoseconds1, Crypto::SignedBigInteger const& nanoseconds2, StringView time_zone, StringView calendar, Unit unit)
{
    // 1. If TemporalUnitCategory(unit) is TIME, then
    if (temporal_unit_category(unit) == UnitCategory::Time) {
        // a. Let difference be TimeDurationFromEpochNanosecondsDifference(ns2, ns1).
        auto difference = time_duration_from_epoch_nanoseconds_difference(nanoseconds2, nanoseconds1);

        // b. Return TotalTimeDuration(difference, unit).
        return total_time_duration(difference, unit);
    }

    // 2. Let difference be ? DifferenceZonedDateTime(ns1, ns2, timeZone, calendar, unit).
    auto difference = TRY(difference_zoned_date_time(vm, nanoseconds1, nanoseconds2, time_zone, calendar, unit));

    // 3. Let dateTime be GetISODateTimeFor(timeZone, ns1).
    auto date_time = get_iso_date_time_for(time_zone, nanoseconds1);

    // 4. Return ? TotalRelativeDuration(difference, ns2, dateTime, timeZone, calendar, unit).
    return TRY(total_relative_duration(vm, difference, nanoseconds2, date_time, time_zone, calendar, unit));
}

// 6.5.9 DifferenceTemporalZonedDateTime ( operation, zonedDateTime, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalzoneddatetime
ThrowCompletionOr<GC::Ref<Duration>> difference_temporal_zoned_date_time(VM& vm, DurationOperation operation, ZonedDateTime const& zoned_date_time, Value other_value, Value options)
{
    // 1. Set other to ? ToTemporalZonedDateTime(other).
    auto other = TRY(to_temporal_zoned_date_time(vm, other_value));

    // 2. If CalendarEquals(zonedDateTime.[[Calendar]], other.[[Calendar]]) is false, then
    if (!calendar_equals(zoned_date_time.calendar(), other->calendar())) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalDifferentCalendars);
    }

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 4. Let settings be ? GetDifferenceSettings(operation, resolvedOptions, DATETIME, « », NANOSECOND, HOUR).
    auto settings = TRY(get_difference_settings(vm, operation, resolved_options, UnitGroup::DateTime, {}, Unit::Nanosecond, Unit::Hour));

    // 5. If TemporalUnitCategory(settings.[[LargestUnit]]) is TIME, then
    if (temporal_unit_category(settings.largest_unit) == UnitCategory::Time) {
        // a. Let internalDuration be DifferenceInstant(zonedDateTime.[[EpochNanoseconds]], other.[[EpochNanoseconds]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]]).
        auto internal_duration = difference_instant(vm, zoned_date_time.epoch_nanoseconds()->big_integer(), other->epoch_nanoseconds()->big_integer(), settings.rounding_increment, settings.smallest_unit, settings.rounding_mode);

        // b. Let result be ! TemporalDurationFromInternal(internalDuration, settings.[[LargestUnit]]).
        auto result = MUST(temporal_duration_from_internal(vm, internal_duration, settings.largest_unit));

        // c. If operation is SINCE, set result to CreateNegatedTemporalDuration(result).
        if (operation == DurationOperation::Since)
            result = create_negated_temporal_duration(vm, result);

        // d. Return result.
        return result;
    }

    // 6. NOTE: To calculate differences in two different time zones, settings.[[LargestUnit]] must be a time unit,
    //    because day lengths can vary between time zones due to DST and other UTC offset shifts.

    // 7. If TimeZoneEquals(zonedDateTime.[[TimeZone]], other.[[TimeZone]]) is false, then
    if (!time_zone_equals(zoned_date_time.time_zone(), other->time_zone())) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalDifferentTimeZones);
    }

    // 8. If zonedDateTime.[[EpochNanoseconds]] = other.[[EpochNanoseconds]], then
    if (zoned_date_time.epoch_nanoseconds()->big_integer() == other->epoch_nanoseconds()->big_integer()) {
        // a. Return ! CreateTemporalDuration(0, 0, 0, 0, 0, 0, 0, 0, 0, 0).
        return MUST(create_temporal_duration(vm, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
    }

    // 9. Let internalDuration be ? DifferenceZonedDateTimeWithRounding(zonedDateTime.[[EpochNanoseconds]], other.[[EpochNanoseconds]], zonedDateTime.[[TimeZone]], zonedDateTime.[[Calendar]], settings.[[LargestUnit]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]]).
    auto internal_duration = TRY(difference_zoned_date_time_with_rounding(vm, zoned_date_time.epoch_nanoseconds()->big_integer(), other->epoch_nanoseconds()->big_integer(), zoned_date_time.time_zone(), zoned_date_time.calendar(), settings.largest_unit, settings.rounding_increment, settings.smallest_unit, settings.rounding_mode));

    // 10. Let result be ! TemporalDurationFromInternal(internalDuration, HOUR).
    auto result = MUST(temporal_duration_from_internal(vm, internal_duration, Unit::Hour));

    // 11. If operation is SINCE, set result to CreateNegatedTemporalDuration(result).
    if (operation == DurationOperation::Since)
        result = create_negated_temporal_duration(vm, result);

    // 12. Return result.
    return result;
}

// 6.5.10 AddDurationToZonedDateTime ( operation, zonedDateTime, temporalDurationLike, options ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtozoneddatetime
ThrowCompletionOr<GC::Ref<ZonedDateTime>> add_duration_to_zoned_date_time(VM& vm, ArithmeticOperation operation, ZonedDateTime const& zoned_date_time, Value temporal_duration_like, Value options)
{
    // 1. Let duration be ? ToTemporalDuration(temporalDurationLike).
    auto duration = TRY(to_temporal_duration(vm, temporal_duration_like));

    // 2. If operation is SUBTRACT, set duration to CreateNegatedTemporalDuration(duration).
    if (operation == ArithmeticOperation::Subtract)
        duration = create_negated_temporal_duration(vm, duration);

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 4. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
    auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

    // 5. Let calendar be zonedDateTime.[[Calendar]].
    auto const& calendar = zoned_date_time.calendar();

    // 6. Let timeZone be zonedDateTime.[[TimeZone]].
    auto const& time_zone = zoned_date_time.time_zone();

    // 7. Let internalDuration be ToInternalDurationRecord(duration).
    auto internal_duration = to_internal_duration_record(vm, duration);

    // 8. Let epochNanoseconds be ? AddZonedDateTime(zonedDateTime.[[EpochNanoseconds]], timeZone, calendar, internalDuration, overflow).
    auto epoch_nanoseconds = TRY(add_zoned_date_time(vm, zoned_date_time.epoch_nanoseconds()->big_integer(), time_zone, calendar, internal_duration, overflow));

    // 9. Return ! CreateTemporalZonedDateTime(epochNanoseconds, timeZone, calendar).
    return MUST(create_temporal_zoned_date_time(vm, BigInt::create(vm, move(epoch_nanoseconds)), time_zone, calendar));
}

}
