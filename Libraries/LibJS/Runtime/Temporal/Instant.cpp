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
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/InstantConstructor.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
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
        // FIXME: Handle ZonedDateTime.
        if (is<Instant>(object)) {
            // i. Return ! CreateTemporalInstant(item.[[EpochNanoseconds]]).
            return MUST(create_temporal_instant(vm, static_cast<Instant const&>(object).epoch_nanoseconds()));
        }

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

}
