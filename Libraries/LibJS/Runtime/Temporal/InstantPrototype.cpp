/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/InstantPrototype.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(InstantPrototype);

// 8.3 Properties of the Temporal.Instant Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-instant-prototype-object
InstantPrototype::InstantPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void InstantPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 8.3.2 Temporal.Instant.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-instant-prototype-object
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.Instant"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.epochMilliseconds, epoch_milliseconds_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.epochNanoseconds, epoch_nanoseconds_getter, {}, Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.add, add, 1, attr);
    define_native_function(realm, vm.names.subtract, subtract, 1, attr);
    define_native_function(realm, vm.names.until, until, 1, attr);
    define_native_function(realm, vm.names.since, since, 1, attr);
    define_native_function(realm, vm.names.round, round, 1, attr);
    define_native_function(realm, vm.names.equals, equals, 1, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 0, attr);
    define_native_function(realm, vm.names.valueOf, value_of, 0, attr);
}

// 8.3.3 get Temporal.Instant.prototype.epochMilliseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.instant.prototype.epochmilliseconds
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::epoch_milliseconds_getter)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Let ns be instant.[[EpochNanoseconds]].
    auto nanoseconds = instant->epoch_nanoseconds();

    // 4. Let ms be floor(ℝ(ns) / 10**6).
    auto milliseconds = big_floor(nanoseconds->big_integer(), NANOSECONDS_PER_MILLISECOND);

    // 5. Return 𝔽(ms).
    return milliseconds.to_double();
}

// 8.3.4 get Temporal.Instant.prototype.epochNanoseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.instant.prototype.epochnanoseconds
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::epoch_nanoseconds_getter)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Return instant.[[EpochNanoseconds]].
    return instant->epoch_nanoseconds();
}

// 8.3.5 Temporal.Instant.prototype.add ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.add
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::add)
{
    auto temporal_duration_like = vm.argument(0);

    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToInstant(ADD, instant, temporalDurationLike).
    return TRY(add_duration_to_instant(vm, ArithmeticOperation::Add, instant, temporal_duration_like));
}

// 8.3.6 Temporal.Instant.prototype.subtract ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.subtract
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::subtract)
{
    auto temporal_duration_like = vm.argument(0);

    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToInstant(SUBTRACT, instant, temporalDurationLike).
    return TRY(add_duration_to_instant(vm, ArithmeticOperation::Subtract, instant, temporal_duration_like));
}

// 8.3.7 Temporal.Instant.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.until
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::until)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalInstant(UNTIL, instant, other, options).
    return TRY(difference_temporal_instant(vm, DurationOperation::Until, instant, other, options));
}

// 8.3.8 Temporal.Instant.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.since
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::since)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalInstant(SINCE, instant, other, options).
    return TRY(difference_temporal_instant(vm, DurationOperation::Since, instant, other, options));
}

// 8.3.9 Temporal.Instant.prototype.round ( roundTo ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.round
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::round)
{
    auto& realm = *vm.current_realm();

    auto round_to_value = vm.argument(0);

    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. If roundTo is undefined, then
    if (round_to_value.is_undefined()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::TemporalMissingOptionsObject);
    }

    GC::Ptr<Object> round_to;

    // 4. If roundTo is a String, then
    if (round_to_value.is_string()) {
        // a. Let paramString be roundTo.
        auto param_string = round_to_value;

        // b. Set roundTo to OrdinaryObjectCreate(null).
        round_to = Object::create(realm, nullptr);

        // c. Perform ! CreateDataPropertyOrThrow(roundTo, "smallestUnit", paramString).
        MUST(round_to->create_data_property_or_throw(vm.names.smallestUnit, param_string));
    }
    // 5. Else,
    else {
        // a. Set roundTo to ? GetOptionsObject(roundTo).
        round_to = TRY(get_options_object(vm, round_to_value));
    }

    // 6. NOTE: The following steps read options and perform independent validation in alphabetical order
    //    (GetRoundingIncrementOption reads "roundingIncrement" and GetRoundingModeOption reads "roundingMode").

    // 7. Let roundingIncrement be ? GetRoundingIncrementOption(roundTo).
    auto rounding_increment = TRY(get_rounding_increment_option(vm, *round_to));

    // 8. Let roundingMode be ? GetRoundingModeOption(roundTo, HALF-EXPAND).
    auto rounding_mode = TRY(get_rounding_mode_option(vm, *round_to, RoundingMode::HalfExpand));

    // 9. Let smallestUnit be ? GetTemporalUnitValuedOption(roundTo, "smallestUnit", TIME, REQUIRED).
    auto smallest_unit = TRY(get_temporal_unit_valued_option(vm, *round_to, vm.names.smallestUnit, UnitGroup::Time, Required {}));
    auto smallest_unit_value = smallest_unit.get<Unit>();

    auto maximum = [&]() {
        switch (smallest_unit_value) {
        // 10. If smallestUnit is hour, then
        case Unit::Hour:
            // a. Let maximum be HoursPerDay.
            return hours_per_day;
        // 11. Else if smallestUnit is minute, then
        case Unit::Minute:
            // a. Let maximum be MinutesPerHour × HoursPerDay.
            return minutes_per_hour * hours_per_day;
        // 12. Else if smallestUnit is second, then
        case Unit::Second:
            // a. Let maximum be SecondsPerMinute × MinutesPerHour × HoursPerDay.
            return seconds_per_minute * minutes_per_hour * hours_per_day;
        // 13. Else if smallestUnit is millisecond, then
        case Unit::Millisecond:
            // a. Let maximum be ℝ(msPerDay).
            return ms_per_day;
        // 14. Else if smallestUnit is microsecond, then
        case Unit::Microsecond:
            // a. Let maximum be 10**3 × ℝ(msPerDay).
            return 1000 * ms_per_day;
        // 15. Else,
        case Unit::Nanosecond:
            // a. Assert: smallestUnit is nanosecond.
            // b. Let maximum be nsPerDay.
            return ns_per_day;
        default:
            break;
        }

        VERIFY_NOT_REACHED();
    }();

    // 16. Perform ? ValidateTemporalRoundingIncrement(roundingIncrement, maximum, true).
    TRY(validate_temporal_rounding_increment(vm, rounding_increment, maximum, true));

    // 17. Let roundedNs be RoundTemporalInstant(instant.[[EpochNanoseconds]], roundingIncrement, smallestUnit, roundingMode).
    auto rounded_nanoseconds = round_temporal_instant(instant->epoch_nanoseconds()->big_integer(), rounding_increment, smallest_unit_value, rounding_mode);

    // 18. Return ! CreateTemporalInstant(roundedNs).
    return MUST(create_temporal_instant(vm, BigInt::create(vm, move(rounded_nanoseconds))));
}

// 8.3.10 Temporal.Instant.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.equals
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::equals)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Set other to ? ToTemporalInstant(other).
    auto other = TRY(to_temporal_instant(vm, vm.argument(0)));

    // 4. If instant.[[EpochNanoseconds]] ≠ other.[[EpochNanoseconds]], return false.
    if (instant->epoch_nanoseconds()->big_integer() != other->epoch_nanoseconds()->big_integer())
        return false;

    // 5. Return true.
    return true;
}

// 8.3.11 Temporal.Instant.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::to_string)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, vm.argument(0)));

    // 4. NOTE: The following steps read options and perform independent validation in alphabetical order
    //    (GetTemporalFractionalSecondDigitsOption reads "fractionalSecondDigits" and GetRoundingModeOption reads "roundingMode").

    // 5. Let digits be ? GetTemporalFractionalSecondDigitsOption(resolvedOptions).
    auto digits = TRY(get_temporal_fractional_second_digits_option(vm, resolved_options));

    // 6. Let roundingMode be ? GetRoundingModeOption(resolvedOptions, trunc).
    auto rounding_mode = TRY(get_rounding_mode_option(vm, resolved_options, RoundingMode::Trunc));

    // 7. Let smallestUnit be ? GetTemporalUnitValuedOption(resolvedOptions, "smallestUnit", time, unset).
    auto smallest_unit = TRY(get_temporal_unit_valued_option(vm, resolved_options, vm.names.smallestUnit, UnitGroup::Time, Unset {}));

    // 8. If smallestUnit is HOUR, throw a RangeError exception.
    if (auto const* unit = smallest_unit.get_pointer<Unit>(); unit && *unit == Unit::Hour)
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, temporal_unit_to_string(*unit), vm.names.smallestUnit);

    // 9. Let timeZone be ? Get(resolvedOptions, "timeZone").
    auto time_zone_value = TRY(resolved_options->get(vm.names.timeZone));

    String time_zone_buffer;
    Optional<StringView> time_zone;

    // 10. If timeZone is not undefined, then
    if (!time_zone_value.is_undefined()) {
        // a. Set timeZone to ? ToTemporalTimeZoneIdentifier(timeZone).
        time_zone_buffer = TRY(to_temporal_time_zone_identifier(vm, time_zone_value));
        time_zone = time_zone_buffer;
    }

    // 11. Let precision be ToSecondsStringPrecisionRecord(smallestUnit, digits).
    auto precision = to_seconds_string_precision_record(smallest_unit, digits);

    // 12. Let roundedNs be RoundTemporalInstant(instant.[[EpochNanoseconds]], precision.[[Increment]], precision.[[Unit]], roundingMode).
    auto rounded_nanoseconds = round_temporal_instant(instant->epoch_nanoseconds()->big_integer(), precision.increment, precision.unit, rounding_mode);

    // 13. Let roundedInstant be ! CreateTemporalInstant(roundedNs).
    auto rounded_instant = MUST(create_temporal_instant(vm, BigInt::create(vm, move(rounded_nanoseconds))));

    // 14. Return TemporalInstantToString(roundedInstant, timeZone, precision.[[Precision]]).
    return PrimitiveString::create(vm, temporal_instant_to_string(rounded_instant, time_zone, precision.precision));
}

// 8.3.12 Temporal.Instant.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.tolocalestring
// NOTE: This is the minimum toLocaleString implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::to_locale_string)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Return TemporalInstantToString(instant, undefined, AUTO).
    return PrimitiveString::create(vm, temporal_instant_to_string(instant, {}, Auto {}));
}

// 8.3.13 Temporal.Instant.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::to_json)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Return TemporalInstantToString(instant, undefined, AUTO).
    return PrimitiveString::create(vm, temporal_instant_to_string(instant, {}, Auto {}));
}

// 8.3.14 Temporal.Instant.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.Instant", "a primitive value");
}

}
