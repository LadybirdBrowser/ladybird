/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/InstantConstructor.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(Instant);

// 8 Temporal.Instant Objects, https://tc39.es/proposal-temporal/#sec-temporal-instant-objects
Instant::Instant(BigInt const& epoch_nanoseconds, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_epoch_nanoseconds(epoch_nanoseconds)
{
}

void Instant::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_epoch_nanoseconds);
}

// nsMaxInstant = 10**8 × nsPerDay = 8.64 × 10**21
Crypto::SignedBigInteger const NANOSECONDS_MAX_INSTANT = "8640000000000000000000"_sbigint;

// nsMinInstant = -nsMaxInstant = -8.64 × 10**21
Crypto::SignedBigInteger const NANOSECONDS_MIN_INSTANT = "-8640000000000000000000"_sbigint;

// nsPerDay = 10**6 × ℝ(msPerDay) = 8.64 × 10**13
Crypto::UnsignedBigInteger const NANOSECONDS_PER_DAY = 86400000000000_bigint;

// Non-standard:
Crypto::UnsignedBigInteger const NANOSECONDS_PER_HOUR = 3600000000000_bigint;
Crypto::UnsignedBigInteger const NANOSECONDS_PER_MINUTE = 60000000000_bigint;
Crypto::UnsignedBigInteger const NANOSECONDS_PER_SECOND = 1000000000_bigint;
Crypto::UnsignedBigInteger const NANOSECONDS_PER_MILLISECOND = 1000000_bigint;
Crypto::UnsignedBigInteger const NANOSECONDS_PER_MICROSECOND = 1000_bigint;
Crypto::UnsignedBigInteger const NANOSECONDS_PER_NANOSECOND = 1_bigint;

Crypto::UnsignedBigInteger const MICROSECONDS_PER_MILLISECOND = 1000_bigint;
Crypto::UnsignedBigInteger const MILLISECONDS_PER_SECOND = 1000_bigint;
Crypto::UnsignedBigInteger const SECONDS_PER_MINUTE = 60_bigint;
Crypto::UnsignedBigInteger const MINUTES_PER_HOUR = 60_bigint;
Crypto::UnsignedBigInteger const HOURS_PER_DAY = 24_bigint;

// 8.5.1 IsValidEpochNanoseconds ( epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidepochnanoseconds
bool is_valid_epoch_nanoseconds(Crypto::SignedBigInteger const& epoch_nanoseconds)
{
    // 1. If ℝ(epochNanoseconds) < nsMinInstant or ℝ(epochNanoseconds) > nsMaxInstant, then
    if (epoch_nanoseconds < NANOSECONDS_MIN_INSTANT || epoch_nanoseconds > NANOSECONDS_MAX_INSTANT) {
        // a. Return false.
        return false;
    }

    // 2. Return true.
    return true;
}

// 8.5.2 CreateTemporalInstant ( epochNanoseconds [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidepochnanoseconds
ThrowCompletionOr<GC::Ref<Instant>> create_temporal_instant(VM& vm, BigInt const& epoch_nanoseconds, GC::Ptr<FunctionObject> new_target)
{
    auto& realm = *vm.current_realm();

    // 1.  Assert: IsValidEpochNanoseconds(epochNanoseconds) is true.
    VERIFY(is_valid_epoch_nanoseconds(epoch_nanoseconds.big_integer()));

    // 2. If newTarget is not present, set newTarget to %Temporal.Instant%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_instant_constructor();

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.Instant.prototype%", « [[InitializedTemporalInstant]], [[EpochNanoseconds]] »).
    // 4. Set object.[[EpochNanoseconds]] to epochNanoseconds.
    auto object = TRY(ordinary_create_from_constructor<Instant>(vm, *new_target, &Intrinsics::temporal_instant_prototype, epoch_nanoseconds));

    // 5. Return object.
    return object;
}

// 8.5.3 ToTemporalInstant ( item ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalinstant
ThrowCompletionOr<GC::Ref<Instant>> to_temporal_instant(VM& vm, Value item)
{
    // 1. If item is an Object, then
    if (item.is_object()) {
        auto const& object = item.as_object();

        // a. If item has an [[InitializedTemporalInstant]] or [[InitializedTemporalZonedDateTime]] internal slot, then
        //     i. Return ! CreateTemporalInstant(item.[[EpochNanoseconds]]).
        if (is<Instant>(object))
            return MUST(create_temporal_instant(vm, static_cast<Instant const&>(object).epoch_nanoseconds()));
        if (is<ZonedDateTime>(object))
            return MUST(create_temporal_instant(vm, static_cast<ZonedDateTime const&>(object).epoch_nanoseconds()));

        // b. NOTE: This use of ToPrimitive allows Instant-like objects to be converted.
        // c. Set item to ? ToPrimitive(item, STRING).
        item = TRY(item.to_primitive(vm, Value::PreferredType::String));
    }

    // 2. If item is not a String, throw a TypeError exception.
    if (!item.is_string())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidInstantString, item);

    // 3. Let parsed be ? ParseISODateTime(item, « TemporalInstantString »).
    auto parsed = TRY(parse_iso_date_time(vm, item.as_string().utf8_string_view(), { { Production::TemporalInstantString } }));

    // 4. Assert: Either parsed.[[TimeZone]].[[OffsetString]] is not empty or parsed.[[TimeZone]].[[Z]] is true, but not both.
    auto const& offset_string = parsed.time_zone.offset_string;
    auto z_designator = parsed.time_zone.z_designator;

    VERIFY(offset_string.has_value() || z_designator);
    VERIFY(!offset_string.has_value() || !z_designator);

    // 5. If parsed.[[TimeZone]].[[Z]] is true, let offsetNanoseconds be 0; otherwise, let offsetNanoseconds be
    //    ! ParseDateTimeUTCOffset(parsed.[[TimeZone]].[[OffsetString]]).
    auto offset_nanoseconds = z_designator ? 0.0 : parse_date_time_utc_offset(*offset_string);

    // 6. If parsed.[[Time]] is START-OF-DAY, let time be MidnightTimeRecord(); else let time be parsed.[[Time]].
    auto time = parsed.time.has<ParsedISODateTime::StartOfDay>() ? midnight_time_record() : parsed.time.get<Time>();

    // 7. Let balanced be BalanceISODateTime(parsed.[[Year]], parsed.[[Month]], parsed.[[Day]], time.[[Hour]], time.[[Minute]], time.[[Second]], time.[[Millisecond]], time.[[Microsecond]], time.[[Nanosecond]] - offsetNanoseconds).
    auto balanced = balance_iso_date_time(*parsed.year, parsed.month, parsed.day, time.hour, time.minute, time.second, time.millisecond, time.microsecond, time.nanosecond - offset_nanoseconds);

    // 8. Perform ? CheckISODaysRange(balanced.[[ISODate]]).
    TRY(check_iso_days_range(vm, balanced.iso_date));

    // 9. Let epochNanoseconds be GetUTCEpochNanoseconds(balanced).
    auto epoch_nanoseconds = get_utc_epoch_nanoseconds(balanced);

    // 10. If IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
    if (!is_valid_epoch_nanoseconds(epoch_nanoseconds))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

    // 11. Return ! CreateTemporalInstant(epochNanoseconds).
    return MUST(create_temporal_instant(vm, BigInt::create(vm, move(epoch_nanoseconds))));
}

// 8.5.4 CompareEpochNanoseconds ( epochNanosecondsOne, epochNanosecondsTwo ), https://tc39.es/proposal-temporal/#sec-temporal-compareepochnanoseconds
i8 compare_epoch_nanoseconds(Crypto::SignedBigInteger const& epoch_nanoseconds_one, Crypto::SignedBigInteger const& epoch_nanoseconds_two)
{
    // 1. If epochNanosecondsOne > epochNanosecondsTwo, return 1.
    if (epoch_nanoseconds_one > epoch_nanoseconds_two)
        return 1;

    // 2. If epochNanosecondsOne < epochNanosecondsTwo, return -1.
    if (epoch_nanoseconds_one < epoch_nanoseconds_two)
        return -1;

    // 3. Return 0.
    return 0;
}

// 8.5.5 AddInstant ( epochNanoseconds, timeDuration ), https://tc39.es/proposal-temporal/#sec-temporal-addinstant
ThrowCompletionOr<Crypto::SignedBigInteger> add_instant(VM& vm, Crypto::SignedBigInteger const& epoch_nanoseconds, TimeDuration const& time_duration)
{
    // 1. Let result be AddTimeDurationToEpochNanoseconds(timeDuration, epochNanoseconds).
    auto result = add_time_duration_to_epoch_nanoseconds(time_duration, epoch_nanoseconds);

    // 2. If IsValidEpochNanoseconds(result) is false, throw a RangeError exception.
    if (!is_valid_epoch_nanoseconds(result))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

    // 3. Return result.
    return result;
}

// 8.5.6 DifferenceInstant ( ns1, ns2, roundingIncrement, smallestUnit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-differenceinstant
InternalDuration difference_instant(VM& vm, Crypto::SignedBigInteger const& nanoseconds1, Crypto::SignedBigInteger const& nanoseconds2, u64 rounding_increment, Unit smallest_unit, RoundingMode rounding_mode)
{
    // 1. Let timeDuration be TimeDurationFromEpochNanosecondsDifference(ns2, ns1).
    auto time_duration = time_duration_from_epoch_nanoseconds_difference(nanoseconds2, nanoseconds1);

    // 2. Set timeDuration to ! RoundTimeDuration(timeDuration, roundingIncrement, smallestUnit, roundingMode).
    time_duration = MUST(round_time_duration(vm, time_duration, Crypto::UnsignedBigInteger { rounding_increment }, smallest_unit, rounding_mode));

    // 3. Return ! CombineDateAndTimeDuration(ZeroDateDuration(), timeDuration).
    return MUST(combine_date_and_time_duration(vm, zero_date_duration(vm), move(time_duration)));
}

// 8.5.7 RoundTemporalInstant ( ns, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundtemporalinstant
Crypto::SignedBigInteger round_temporal_instant(Crypto::SignedBigInteger const& nanoseconds, u64 increment, Unit unit, RoundingMode rounding_mode)
{
    // 1. Let unitLength be the value in the "Length in Nanoseconds" column of the row of Table 21 whose "Value" column contains unit.
    auto unit_length = temporal_unit_length_in_nanoseconds(unit);

    // 2. Let incrementNs be increment × unitLength.
    auto increment_nanoseconds = Crypto::UnsignedBigInteger { increment }.multiplied_by(unit_length);

    // 3. Return ℤ(RoundNumberToIncrementAsIfPositive(ℝ(ns), incrementNs, roundingMode)).
    return round_number_to_increment_as_if_positive(nanoseconds, increment_nanoseconds, rounding_mode);
}

// 8.5.8 TemporalInstantToString ( instant, timeZone, precision ), https://tc39.es/proposal-temporal/#sec-temporal-temporalinstanttostring
String temporal_instant_to_string(Instant const& instant, Optional<StringView> time_zone, SecondsStringPrecision::Precision precision)
{
    // 1. Let outputTimeZone be timeZone.
    // 2. If outputTimeZone is undefined, set outputTimeZone to "UTC".
    auto output_time_zone = time_zone.value_or("UTC"sv);

    // 3. Let epochNs be instant.[[EpochNanoseconds]].
    auto const& epoch_nanoseconds = instant.epoch_nanoseconds()->big_integer();

    // 4. Let isoDateTime be GetISODateTimeFor(outputTimeZone, epochNs).
    auto iso_date_time = get_iso_date_time_for(output_time_zone, epoch_nanoseconds);

    // 5. Let dateTimeString be ISODateTimeToString(isoDateTime, "iso8601", precision, NEVER).
    auto date_time_string = iso_date_time_to_string(iso_date_time, "iso8601"sv, precision, ShowCalendar::Never);

    String time_zone_string;

    // 6. If timeZone is undefined, then
    if (!time_zone.has_value()) {
        // a. Let timeZoneString be "Z".
        time_zone_string = "Z"_string;
    }
    // 7. Else,
    else {
        // a. Let offsetNanoseconds be GetOffsetNanosecondsFor(outputTimeZone, epochNs).
        auto offset_nanoseconds = get_offset_nanoseconds_for(output_time_zone, epoch_nanoseconds);

        // b. Let timeZoneString be FormatDateTimeUTCOffsetRounded(offsetNanoseconds).
        time_zone_string = format_date_time_utc_offset_rounded(offset_nanoseconds);
    }

    // 8. Return the string-concatenation of dateTimeString and timeZoneString.
    return MUST(String::formatted("{}{}", date_time_string, time_zone_string));
}

// 8.5.9 DifferenceTemporalInstant ( operation, instant, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalinstant
ThrowCompletionOr<GC::Ref<Duration>> difference_temporal_instant(VM& vm, DurationOperation operation, Instant const& instant, Value other_value, Value options)
{
    // 1. Set other to ? ToTemporalInstant(other).
    auto other = TRY(to_temporal_instant(vm, other_value));

    // 2. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 3. Let settings be ? GetDifferenceSettings(operation, resolvedOptions, TIME, « », NANOSECOND, SECOND).
    auto settings = TRY(get_difference_settings(vm, operation, resolved_options, UnitGroup::Time, {}, Unit::Nanosecond, Unit::Second));

    // 4. Let internalDuration be DifferenceInstant(instant.[[EpochNanoseconds]], other.[[EpochNanoseconds]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[RoundingMode]]).
    auto internal_duration = difference_instant(vm, instant.epoch_nanoseconds()->big_integer(), other->epoch_nanoseconds()->big_integer(), settings.rounding_increment, settings.smallest_unit, settings.rounding_mode);

    // 5. Let result be ! TemporalDurationFromInternal(internalDuration, settings.[[LargestUnit]]).
    auto result = MUST(temporal_duration_from_internal(vm, internal_duration, settings.largest_unit));

    // 6. If operation is SINCE, set result to CreateNegatedTemporalDuration(result).
    if (operation == DurationOperation::Since)
        result = create_negated_temporal_duration(vm, result);

    // 7. Return result.
    return result;
}

// 8.5.10 AddDurationToInstant ( operation, instant, temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtoinstant
ThrowCompletionOr<GC::Ref<Instant>> add_duration_to_instant(VM& vm, ArithmeticOperation operation, Instant const& instant, Value temporal_duration_like)
{
    // 1. Let duration be ? ToTemporalDuration(temporalDurationLike).
    auto duration = TRY(to_temporal_duration(vm, temporal_duration_like));

    // 2. If operation is SUBTRACT, set duration to CreateNegatedTemporalDuration(duration).
    if (operation == ArithmeticOperation::Subtract)
        duration = create_negated_temporal_duration(vm, duration);

    // 3. Let largestUnit be DefaultTemporalLargestUnit(duration).
    auto largest_unit = default_temporal_largest_unit(duration);

    // 4. If TemporalUnitCategory(largestUnit) is DATE, throw a RangeError exception.
    if (temporal_unit_category(largest_unit) == UnitCategory::Date)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidLargestUnit, temporal_unit_to_string(largest_unit));

    // 5. Let internalDuration be ToInternalDurationRecordWith24HourDays(duration).
    auto internal_duration = to_internal_duration_record_with_24_hour_days(vm, duration);

    // 6. Let ns be ? AddInstant(instant.[[EpochNanoseconds]], internalDuration.[[Time]]).
    auto nanoseconds = TRY(add_instant(vm, instant.epoch_nanoseconds()->big_integer(), internal_duration.time));

    // 7. Return ! CreateTemporalInstant(ns).
    return MUST(create_temporal_instant(vm, BigInt::create(vm, move(nanoseconds))));
}

}
