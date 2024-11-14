/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <AK/Variant.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/InstantConstructor.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(Instant);

// 8 Temporal.Instant Objects, https://tc39.es/proposal-temporal/#sec-temporal-instant-objects
Instant::Instant(BigInt const& nanoseconds, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_nanoseconds(nanoseconds)
{
}

void Instant::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_nanoseconds);
}

// 8.5.1 IsValidEpochNanoseconds ( epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidepochnanoseconds
bool is_valid_epoch_nanoseconds(BigInt const& epoch_nanoseconds)
{
    return is_valid_epoch_nanoseconds(epoch_nanoseconds.big_integer());
}

// 8.5.1 IsValidEpochNanoseconds ( epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-isvalidepochnanoseconds
bool is_valid_epoch_nanoseconds(Crypto::SignedBigInteger const& epoch_nanoseconds)
{
    // 1. Assert: Type(epochNanoseconds) is BigInt.

    // 2. If ℝ(epochNanoseconds) < nsMinInstant or ℝ(epochNanoseconds) > nsMaxInstant, then
    if (epoch_nanoseconds < ns_min_instant || epoch_nanoseconds > ns_max_instant) {
        // a. Return false.
        return false;
    }

    // 3. Return true.
    return true;
}

// 8.5.2 CreateTemporalInstant ( epochNanoseconds [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporalinstant
ThrowCompletionOr<Instant*> create_temporal_instant(VM& vm, BigInt const& epoch_nanoseconds, FunctionObject const* new_target)
{
    auto& realm = *vm.current_realm();

    // 1. Assert: Type(epochNanoseconds) is BigInt.

    // 2. Assert: ! IsValidEpochNanoseconds(epochNanoseconds) is true.
    VERIFY(is_valid_epoch_nanoseconds(epoch_nanoseconds));

    // 3. If newTarget is not present, set newTarget to %Temporal.Instant%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_instant_constructor();

    // 4. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.Instant.prototype%", « [[InitializedTemporalInstant]], [[Nanoseconds]] »).
    // 5. Set object.[[Nanoseconds]] to epochNanoseconds.
    auto object = TRY(ordinary_create_from_constructor<Instant>(vm, *new_target, &Intrinsics::temporal_instant_prototype, epoch_nanoseconds));

    // 6. Return object.
    return object.ptr();
}

// 8.5.3 ToTemporalInstant ( item ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalinstant
ThrowCompletionOr<Instant*> to_temporal_instant(VM& vm, Value item)
{
    // 1. If Type(item) is Object, then
    if (item.is_object()) {
        // a. If item has an [[InitializedTemporalInstant]] internal slot, then
        if (is<Instant>(item.as_object())) {
            // i. Return item.
            return &static_cast<Instant&>(item.as_object());
        }

        // b. If item has an [[InitializedTemporalZonedDateTime]] internal slot, then
        if (is<ZonedDateTime>(item.as_object())) {
            auto& zoned_date_time = static_cast<ZonedDateTime&>(item.as_object());

            // i. Return ! CreateTemporalInstant(item.[[Nanoseconds]]).
            return create_temporal_instant(vm, zoned_date_time.nanoseconds());
        }
    }

    // 2. Let string be ? ToString(item).
    auto string = TRY(item.to_string(vm));

    // 3. Let epochNanoseconds be ? ParseTemporalInstant(string).
    auto* epoch_nanoseconds = TRY(parse_temporal_instant(vm, string));

    // 4. Return ! CreateTemporalInstant(ℤ(epochNanoseconds)).
    return create_temporal_instant(vm, *epoch_nanoseconds);
}

// 8.5.4 ParseTemporalInstant ( isoString ), https://tc39.es/proposal-temporal/#sec-temporal-parsetemporalinstant
ThrowCompletionOr<BigInt*> parse_temporal_instant(VM& vm, StringView iso_string)
{
    // 1. Assert: Type(isoString) is String.

    // 2. Let result be ? ParseTemporalInstantString(isoString).
    auto result = TRY(parse_temporal_instant_string(vm, iso_string));

    // 3. Let offsetString be result.[[TimeZoneOffsetString]].
    auto& offset_string = result.time_zone_offset;

    // 4. Assert: offsetString is not undefined.
    VERIFY(offset_string.has_value());

    // 5. Let utc be GetUTCEpochNanoseconds(result.[[Year]], result.[[Month]], result.[[Day]], result.[[Hour]], result.[[Minute]], result.[[Second]], result.[[Millisecond]], result.[[Microsecond]], result.[[Nanosecond]]).
    auto utc = get_utc_epoch_nanoseconds(result.year, result.month, result.day, result.hour, result.minute, result.second, result.millisecond, result.microsecond, result.nanosecond);

    // 6. If IsTimeZoneOffsetString(offsetString) is false, throw a RangeError exception.
    if (!is_time_zone_offset_string(*offset_string))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeZoneName, *offset_string);

    // 7. Let offsetNanoseconds be ParseTimeZoneOffsetString(offsetString).
    auto offset_nanoseconds = parse_time_zone_offset_string(*offset_string);

    // 7. Let result be utc - ℤ(offsetNanoseconds).
    auto result_ns = utc.minus(Crypto::SignedBigInteger { offset_nanoseconds });

    // 8. If ! IsValidEpochNanoseconds(result) is false, then
    if (!is_valid_epoch_nanoseconds(result_ns)) {
        // a. Throw a RangeError exception.
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);
    }

    // 9. Return result.
    return BigInt::create(vm, move(result_ns)).ptr();
}

// 8.5.5 CompareEpochNanoseconds ( epochNanosecondsOne, epochNanosecondsTwo ), https://tc39.es/proposal-temporal/#sec-temporal-compareepochnanoseconds
i32 compare_epoch_nanoseconds(BigInt const& epoch_nanoseconds_one, BigInt const& epoch_nanoseconds_two)
{
    // 1. If epochNanosecondsOne > epochNanosecondsTwo, return 1.
    if (epoch_nanoseconds_one.big_integer() > epoch_nanoseconds_two.big_integer())
        return 1;

    // 2. If epochNanosecondsOne < epochNanosecondsTwo, return -1.
    if (epoch_nanoseconds_one.big_integer() < epoch_nanoseconds_two.big_integer())
        return -1;

    // 3. Return 0.
    return 0;
}

// 8.5.6 AddInstant ( epochNanoseconds, hours, minutes, seconds, milliseconds, microseconds, nanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal-addinstant
ThrowCompletionOr<BigInt*> add_instant(VM& vm, BigInt const& epoch_nanoseconds, double hours, double minutes, double seconds, double milliseconds, double microseconds, double nanoseconds)
{
    VERIFY(hours == trunc(hours) && minutes == trunc(minutes) && seconds == trunc(seconds) && milliseconds == trunc(milliseconds) && microseconds == trunc(microseconds) && nanoseconds == trunc(nanoseconds));

    // 1. Let result be epochNanoseconds + ℤ(nanoseconds) + ℤ(microseconds) × 1000ℤ + ℤ(milliseconds) × 10^6ℤ + ℤ(seconds) × 10^9ℤ + ℤ(minutes) × 60ℤ × 10^9ℤ + ℤ(hours) × 3600ℤ × 10^9ℤ.
    auto result = BigInt::create(vm,
        epoch_nanoseconds.big_integer()
            .plus(Crypto::SignedBigInteger { nanoseconds })
            .plus(Crypto::SignedBigInteger { microseconds }.multiplied_by(Crypto::SignedBigInteger { 1'000 }))
            .plus(Crypto::SignedBigInteger { milliseconds }.multiplied_by(Crypto::SignedBigInteger { 1'000'000 }))
            .plus(Crypto::SignedBigInteger { seconds }.multiplied_by(Crypto::SignedBigInteger { 1'000'000'000 }))
            .plus(Crypto::SignedBigInteger { minutes }.multiplied_by(Crypto::SignedBigInteger { 60 }).multiplied_by(Crypto::SignedBigInteger { 1'000'000'000 }))
            .plus(Crypto::SignedBigInteger { hours }.multiplied_by(Crypto::SignedBigInteger { 3600 }).multiplied_by(Crypto::SignedBigInteger { 1'000'000'000 })));

    // 2. If ! IsValidEpochNanoseconds(result) is false, throw a RangeError exception.
    if (!is_valid_epoch_nanoseconds(*result))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

    // 3. Return result.
    return result.ptr();
}

// 8.5.6 DifferenceInstant ( ns1, ns2, roundingIncrement, smallestUnit, largestUnit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-differenceinstant
TimeDurationRecord difference_instant(VM& vm, BigInt const& nanoseconds1, BigInt const& nanoseconds2, u64 rounding_increment, StringView smallest_unit, StringView largest_unit, StringView rounding_mode)
{
    static Crypto::UnsignedBigInteger const BIGINT_ONE_THOUSAND { 1'000 };

    // 1. Let difference be ℝ(ns2) - ℝ(ns1).
    auto difference = nanoseconds2.big_integer().minus(nanoseconds1.big_integer());

    // 2. Let nanoseconds be remainder(difference, 1000).
    auto nanoseconds = remainder(difference, BIGINT_ONE_THOUSAND);

    // 3. Let microseconds be remainder(truncate(difference / 1000), 1000).
    auto microseconds = remainder(difference.divided_by(BIGINT_ONE_THOUSAND).quotient, BIGINT_ONE_THOUSAND);

    // 4. Let milliseconds be remainder(truncate(difference / 10^6), 1000).
    auto milliseconds = remainder(difference.divided_by(Crypto::UnsignedBigInteger { 1'000'000 }).quotient, BIGINT_ONE_THOUSAND);

    // 5. Let seconds be truncate(difference / 10^9).
    auto seconds = difference.divided_by(Crypto::UnsignedBigInteger { 1'000'000'000 }).quotient;

    // 6. If smallestUnit is "nanosecond" and roundingIncrement is 1, then
    if (smallest_unit == "nanosecond"sv && rounding_increment == 1) {
        // a. Return ! BalanceTimeDuration(0, 0, 0, seconds, milliseconds, microseconds, nanoseconds, largestUnit).
        return MUST(balance_time_duration(vm, 0, 0, 0, seconds.to_double(), milliseconds.to_double(), microseconds.to_double(), nanoseconds, largest_unit));
    }

    // 7. Let roundResult be ! RoundDuration(0, 0, 0, 0, 0, 0, seconds, milliseconds, microseconds, nanoseconds, roundingIncrement, smallestUnit, roundingMode).
    auto round_result = MUST(round_duration(vm, 0, 0, 0, 0, 0, 0, seconds.to_double(), milliseconds.to_double(), microseconds.to_double(), nanoseconds.to_double(), rounding_increment, smallest_unit, rounding_mode)).duration_record;

    // 8. Assert: roundResult.[[Days]] is 0.
    VERIFY(round_result.days == 0);

    // 9. Return ! BalanceTimeDuration(0, roundResult.[[Hours]], roundResult.[[Minutes]], roundResult.[[Seconds]], roundResult.[[Milliseconds]], roundResult.[[Microseconds]], roundResult.[[Nanoseconds]], largestUnit).
    return MUST(balance_time_duration(vm, 0, round_result.hours, round_result.minutes, round_result.seconds, round_result.milliseconds, round_result.microseconds, Crypto::SignedBigInteger { round_result.nanoseconds }, largest_unit));
}

// 8.5.8 RoundTemporalInstant ( ns, increment, unit, roundingMode ), https://tc39.es/proposal-temporal/#sec-temporal-roundtemporalinstant
BigInt* round_temporal_instant(VM& vm, BigInt const& nanoseconds, u64 increment, StringView unit, StringView rounding_mode)
{
    // 1. Assert: Type(ns) is BigInt.

    u64 increment_nanoseconds;
    // 2. If unit is "hour", then
    if (unit == "hour"sv) {
        // a. Let incrementNs be increment × 3.6 × 10^12.
        increment_nanoseconds = increment * 3600000000000;
    }
    // 3. Else if unit is "minute", then
    else if (unit == "minute"sv) {
        // a. Let incrementNs be increment × 6 × 10^10.
        increment_nanoseconds = increment * 60000000000;
    }
    // 4. Else if unit is "second", then
    else if (unit == "second"sv) {
        // a. Let incrementNs be increment × 10^9.
        increment_nanoseconds = increment * 1000000000;
    }
    // 5. Else if unit is "millisecond", then
    else if (unit == "millisecond"sv) {
        // a. Let incrementNs be increment × 10^6.
        increment_nanoseconds = increment * 1000000;
    }
    // 6. Else if unit is "microsecond", then
    else if (unit == "microsecond"sv) {
        // a. Let incrementNs be increment × 10^3.
        increment_nanoseconds = increment * 1000;
    }
    // 7. Else,
    else {
        // a. Assert: unit is "nanosecond".
        VERIFY(unit == "nanosecond"sv);

        // b. Let incrementNs be increment.
        increment_nanoseconds = increment;
    }

    // 8. Return RoundNumberToIncrementAsIfPositive(ℝ(ns), incrementNs, roundingMode).
    return BigInt::create(vm, round_number_to_increment_as_if_positive(nanoseconds.big_integer(), increment_nanoseconds, rounding_mode));
}

// 8.5.9 TemporalInstantToString ( instant, timeZone, precision ), https://tc39.es/proposal-temporal/#sec-temporal-temporalinstanttostring
ThrowCompletionOr<String> temporal_instant_to_string(VM& vm, Instant& instant, Value time_zone, Variant<StringView, u8> const& precision)
{
    // 1. Assert: Type(instant) is Object.
    // 2. Assert: instant has an [[InitializedTemporalInstant]] internal slot.

    // 3. Let outputTimeZone be timeZone.
    auto output_time_zone = time_zone;

    // 4. If outputTimeZone is undefined, then
    if (output_time_zone.is_undefined()) {
        // a. Set outputTimeZone to ! CreateTemporalTimeZone("UTC").
        output_time_zone = MUST_OR_THROW_OOM(create_temporal_time_zone(vm, "UTC"_string));
    }

    // 5. Let isoCalendar be ! GetISO8601Calendar().
    auto* iso_calendar = get_iso8601_calendar(vm);

    // 6. Let dateTime be ? BuiltinTimeZoneGetPlainDateTimeFor(outputTimeZone, instant, isoCalendar).
    auto* date_time = TRY(builtin_time_zone_get_plain_date_time_for(vm, output_time_zone, instant, *iso_calendar));

    // 7. Let dateTimeString be ! TemporalDateTimeToString(dateTime.[[ISOYear]], dateTime.[[ISOMonth]], dateTime.[[ISODay]], dateTime.[[ISOHour]], dateTime.[[ISOMinute]], dateTime.[[ISOSecond]], dateTime.[[ISOMillisecond]], dateTime.[[ISOMicrosecond]], dateTime.[[ISONanosecond]], undefined, precision, "never").
    auto date_time_string = MUST_OR_THROW_OOM(temporal_date_time_to_string(vm, date_time->iso_year(), date_time->iso_month(), date_time->iso_day(), date_time->iso_hour(), date_time->iso_minute(), date_time->iso_second(), date_time->iso_millisecond(), date_time->iso_microsecond(), date_time->iso_nanosecond(), nullptr, precision, "never"sv));

    String time_zone_string;

    // 8. If timeZone is undefined, then
    if (time_zone.is_undefined()) {
        // a. Let timeZoneString be "Z".
        time_zone_string = "Z"_string;
    }
    // 9. Else,
    else {
        auto time_zone_record = TRY(create_time_zone_methods_record(vm, GC::Ref<Object> { time_zone.as_object() }, { { TimeZoneMethod::GetOffsetNanosecondsFor } }));

        // a. Let offsetNs be ? GetOffsetNanosecondsFor(timeZone, instant).
        auto offset_ns = TRY(get_offset_nanoseconds_for(vm, time_zone_record, instant));

        // b. Let timeZoneString be ! FormatISOTimeZoneOffsetString(offsetNs).
        time_zone_string = MUST_OR_THROW_OOM(format_iso_time_zone_offset_string(vm, offset_ns));
    }

    // 10. Return the string-concatenation of dateTimeString and timeZoneString.
    return TRY_OR_THROW_OOM(vm, String::formatted("{}{}", date_time_string, time_zone_string));
}

// 8.5.9 DifferenceTemporalInstant ( operation, instant, other, options ), https://tc39.es/proposal-temporal/#sec-temporal-differencetemporalinstant
ThrowCompletionOr<GC::Ref<Duration>> difference_temporal_instant(VM& vm, DifferenceOperation operation, Instant const& instant, Value other_value, Value options)
{
    // 1. If operation is since, let sign be -1. Otherwise, let sign be 1.
    i8 sign = operation == DifferenceOperation::Since ? -1 : 1;

    // 2. Set other to ? ToTemporalInstant(other).
    auto* other = TRY(to_temporal_instant(vm, other_value));

    // 3. Let resolvedOptions be ? SnapshotOwnProperties(? GetOptionsObject(options), null).
    auto resolved_options = TRY(TRY(get_options_object(vm, options))->snapshot_own_properties(vm, nullptr));

    // 4. Let settings be ? GetDifferenceSettings(operation, resolvedOptions, time, « », "nanosecond", "second").
    auto settings = TRY(get_difference_settings(vm, operation, resolved_options, UnitGroup::Time, {}, { "nanosecond"sv }, "second"sv));

    // 5. Let result be DifferenceInstant(instant.[[Nanoseconds]], other.[[Nanoseconds]], settings.[[RoundingIncrement]], settings.[[SmallestUnit]], settings.[[LargestUnit]], settings.[[RoundingMode]]).
    auto result = difference_instant(vm, instant.nanoseconds(), other->nanoseconds(), settings.rounding_increment, settings.smallest_unit, settings.largest_unit, settings.rounding_mode);

    // 6. Return ! CreateTemporalDuration(0, 0, 0, 0, sign × result.[[Hours]], sign × result.[[Minutes]], sign × result.[[Seconds]], sign × result.[[Milliseconds]], sign × result.[[Microseconds]], sign × result.[[Nanoseconds]]).
    return MUST(create_temporal_duration(vm, 0, 0, 0, 0, sign * result.hours, sign * result.minutes, sign * result.seconds, sign * result.milliseconds, sign * result.microseconds, sign * result.nanoseconds));
}

// 8.5.11 AddDurationToOrSubtractDurationFromInstant ( operation, instant, temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal-adddurationtoorsubtractdurationfrominstant
ThrowCompletionOr<Instant*> add_duration_to_or_subtract_duration_from_instant(VM& vm, ArithmeticOperation operation, Instant const& instant, Value temporal_duration_like)
{
    // 1. If operation is subtract, let sign be -1. Otherwise, let sign be 1.
    i8 sign = operation == ArithmeticOperation::Subtract ? -1 : 1;

    // 2. Let duration be ? ToTemporalDurationRecord(temporalDurationLike).
    auto duration = TRY(to_temporal_duration_record(vm, temporal_duration_like));

    // 3. If duration.[[Days]] is not 0, throw a RangeError exception.
    if (duration.days != 0)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDurationPropertyValueNonZero, "days", duration.days);

    // 4. If duration.[[Months]] is not 0, throw a RangeError exception.
    if (duration.months != 0)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDurationPropertyValueNonZero, "months", duration.months);

    // 5. If duration.[[Weeks]] is not 0, throw a RangeError exception.
    if (duration.weeks != 0)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDurationPropertyValueNonZero, "weeks", duration.weeks);

    // 6. If duration.[[Years]] is not 0, throw a RangeError exception.
    if (duration.years != 0)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidDurationPropertyValueNonZero, "years", duration.years);

    // 7. Let ns be ? AddInstant(instant.[[Nanoseconds]], sign × duration.[[Hours]], sign × duration.[[Minutes]], sign × duration.[[Seconds]], sign × duration.[[Milliseconds]], sign × duration.[[Microseconds]], sign × duration.[[Nanoseconds]]).
    auto* ns = TRY(add_instant(vm, instant.nanoseconds(), sign * duration.hours, sign * duration.minutes, sign * duration.seconds, sign * duration.milliseconds, sign * duration.microseconds, sign * duration.nanoseconds));

    // 8. Return ! CreateTemporalInstant(ns).
    return MUST(create_temporal_instant(vm, *ns));
}

}
