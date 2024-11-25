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
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
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
            if (candidate_offset.compare_to_double(offset_nanoseconds) == Crypto::UnsignedBigInteger::CompareResult::DoubleEqualsBigInt) {
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
        offset_string = move(fields.offset);

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

}
