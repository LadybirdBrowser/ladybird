/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/InstantPrototype.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(InstantPrototype);

// 8.3 Properties of the Temporal.Instant Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-instant-prototype-object
InstantPrototype::InstantPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void InstantPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 8.3.2 Temporal.Instant.prototype[ @@toStringTag ], https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.Instant"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.epochSeconds, epoch_seconds_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.epochMilliseconds, epoch_milliseconds_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.epochMicroseconds, epoch_microseconds_getter, {}, Attribute::Configurable);
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
    define_native_function(realm, vm.names.toZonedDateTime, to_zoned_date_time, 1, attr);
    define_native_function(realm, vm.names.toZonedDateTimeISO, to_zoned_date_time_iso, 1, attr);
}

// 8.3.3 get Temporal.Instant.prototype.epochSeconds, https://tc39.es/proposal-temporal/#sec-get-temporal.instant.prototype.epochseconds
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::epoch_seconds_getter)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Let ns be instant.[[Nanoseconds]].
    auto& ns = instant->nanoseconds();

    // 4. Let s be truncate(ℝ(ns) / 10^9).
    auto [s, _] = ns.big_integer().divided_by(Crypto::UnsignedBigInteger { 1'000'000'000 });

    // 5. Return 𝔽(s).
    return Value((double)s.to_base_deprecated(10).to_number<i64>().value());
}

// 8.3.4 get Temporal.Instant.prototype.epochMilliseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.instant.prototype.epochmilliseconds
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::epoch_milliseconds_getter)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Let ns be instant.[[Nanoseconds]].
    auto& ns = instant->nanoseconds();

    // 4. Let ms be truncate(ℝ(ns) / 10^6).
    auto [ms, _] = ns.big_integer().divided_by(Crypto::UnsignedBigInteger { 1'000'000 });

    // 5. Return 𝔽(ms).
    return Value((double)ms.to_base_deprecated(10).to_number<i64>().value());
}

// 8.3.5 get Temporal.Instant.prototype.epochMicroseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.instant.prototype.epochmicroseconds
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::epoch_microseconds_getter)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Let ns be instant.[[Nanoseconds]].
    auto& ns = instant->nanoseconds();

    // 4. Let µs be truncate(ℝ(ns) / 10^3).
    auto [us, _] = ns.big_integer().divided_by(Crypto::UnsignedBigInteger { 1'000 });

    // 5. Return ℤ(µs).
    return BigInt::create(vm, move(us));
}

// 8.3.6 get Temporal.Instant.prototype.epochNanoseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.instant.prototype.epochnanoseconds
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::epoch_nanoseconds_getter)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Let ns be instant.[[Nanoseconds]].
    auto& ns = instant->nanoseconds();

    // 4. Return ns.
    return &ns;
}

// 8.3.7 Temporal.Instant.prototype.add ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.add
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::add)
{
    auto temporal_duration_like = vm.argument(0);

    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToOrSubtractDurationFromInstant(add, instant, temporalDurationLike).
    return TRY(add_duration_to_or_subtract_duration_from_instant(vm, ArithmeticOperation::Add, instant, temporal_duration_like));
}

// 8.3.8 Temporal.Instant.prototype.subtract ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.subtract
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::subtract)
{
    auto temporal_duration_like = vm.argument(0);

    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToOrSubtractDurationFromInstant(subtract, instant, temporalDurationLike).
    return TRY(add_duration_to_or_subtract_duration_from_instant(vm, ArithmeticOperation::Subtract, instant, temporal_duration_like));
}

// 8.3.9 Temporal.Instant.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.until
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::until)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalInstant(until, instant, other, options).
    return TRY(difference_temporal_instant(vm, DifferenceOperation::Until, instant, other, options));
}

// 8.3.10 Temporal.Instant.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.since
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::since)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalInstant(since, instant, other, options).
    return TRY(difference_temporal_instant(vm, DifferenceOperation::Since, instant, other, options));
}

// 8.3.11 Temporal.Instant.prototype.round ( roundTo ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.round
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::round)
{
    auto& realm = *vm.current_realm();

    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. If roundTo is undefined, then
    if (vm.argument(0).is_undefined()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::TemporalMissingOptionsObject);
    }

    Object* round_to;

    // 4. If Type(roundTo) is String, then
    if (vm.argument(0).is_string()) {
        // a. Let paramString be roundTo.

        // b. Set roundTo to OrdinaryObjectCreate(null).
        round_to = Object::create(realm, nullptr);

        // c. Perform ! CreateDataPropertyOrThrow(roundTo, "smallestUnit", paramString).
        MUST(round_to->create_data_property_or_throw(vm.names.smallestUnit, vm.argument(0)));
    }
    // 5. Else,
    else {
        // a. Set roundTo to ? GetOptionsObject(roundTo).
        round_to = TRY(get_options_object(vm, vm.argument(0)));
    }

    // 6. Let smallestUnit be ? GetTemporalUnit(roundTo, "smallestUnit", time, required).
    auto smallest_unit_value = TRY(get_temporal_unit(vm, *round_to, vm.names.smallestUnit, UnitGroup::Time, TemporalUnitRequired {}));

    // 6. If smallestUnit is undefined, throw a RangeError exception.
    if (!smallest_unit_value.has_value())
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, vm.names.undefined.as_string(), "smallestUnit");

    // At this point smallest_unit_value can only be a string
    auto& smallest_unit = *smallest_unit_value;

    // 7. Let roundingMode be ? ToTemporalRoundingMode(roundTo, "halfExpand").
    auto rounding_mode = TRY(to_temporal_rounding_mode(vm, *round_to, "halfExpand"sv));

    double maximum;
    // 8. If smallestUnit is "hour", then
    if (smallest_unit == "hour"sv) {
        // a. Let maximum be HoursPerDay.
        maximum = hours_per_day;
    }
    // 9. Else if smallestUnit is "minute", then
    else if (smallest_unit == "minute"sv) {
        // a. Let maximum be MinutesPerHour × HoursPerDay.
        maximum = minutes_per_hour * hours_per_day;
    }
    // 10. Else if smallestUnit is "second", then
    else if (smallest_unit == "second"sv) {
        // a. Let maximum be SecondsPerMinute × MinutesPerHour × HoursPerDay.
        maximum = seconds_per_minute * minutes_per_hour * hours_per_day;
    }
    // 11. Else if smallestUnit is "millisecond", then
    else if (smallest_unit == "millisecond"sv) {
        // a. Let maximum be ℝ(msPerDay).
        maximum = ms_per_day;
    }
    // 12. Else if smallestUnit is "microsecond", then
    else if (smallest_unit == "microsecond"sv) {
        // a. Let maximum be 10^3 × ℝ(msPerDay).
        maximum = 1000 * ms_per_day;
    }
    // 13. Else,
    else {
        // a. Assert: smallestUnit is "nanosecond".
        VERIFY(smallest_unit == "nanosecond"sv);
        // b. Let maximum be nsPerDay.
        maximum = ns_per_day;
    }

    // 14. Let roundingIncrement be ? ToTemporalRoundingIncrement(options).
    auto rounding_increment = TRY(to_temporal_rounding_increment(vm, *round_to));

    // 15. Perform ? ValidateTemporalRoundingIncrement(roundingIncrement, maximum, true).
    TRY(validate_temporal_rounding_increment(vm, rounding_increment, maximum, true));

    // 16. Let roundedNs be ! RoundTemporalInstant(instant.[[Nanoseconds]], roundingIncrement, smallestUnit, roundingMode).
    auto* rounded_ns = round_temporal_instant(vm, instant->nanoseconds(), rounding_increment, smallest_unit, rounding_mode);

    // 17. Return ! CreateTemporalInstant(roundedNs).
    return MUST(create_temporal_instant(vm, *rounded_ns));
}

// 8.3.12 Temporal.Instant.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.equals
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::equals)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Set other to ? ToTemporalInstant(other).
    auto other = TRY(to_temporal_instant(vm, vm.argument(0)));

    // 4. If instant.[[Nanoseconds]] ≠ other.[[Nanoseconds]], return false.
    if (instant->nanoseconds().big_integer() != other->nanoseconds().big_integer())
        return Value(false);

    // 5. Return true.
    return Value(true);
}

// 8.3.13 Temporal.Instant.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::to_string)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Set options to ? GetOptionsObject(options).
    auto const* options = TRY(get_options_object(vm, vm.argument(0)));

    // 4. Let timeZone be ? Get(options, "timeZone").
    auto time_zone = TRY(options->get(vm.names.timeZone));

    // 5. If timeZone is not undefined, then
    if (!time_zone.is_undefined()) {
        // a. Set timeZone to ? ToTemporalTimeZone(timeZone).
        time_zone = TRY(to_temporal_time_zone(vm, time_zone));
    }

    // 6. Let precision be ? ToSecondsStringPrecisionRecord(options).
    auto precision = TRY(to_seconds_string_precision_record(vm, *options));

    // 7. Let roundingMode be ? ToTemporalRoundingMode(options, "trunc").
    auto rounding_mode = TRY(to_temporal_rounding_mode(vm, *options, "trunc"sv));

    // 8. Let roundedNs be ! RoundTemporalInstant(instant.[[Nanoseconds]], precision.[[Increment]], precision.[[Unit]], roundingMode).
    auto* rounded_ns = round_temporal_instant(vm, instant->nanoseconds(), precision.increment, precision.unit, rounding_mode);

    // 9. Let roundedInstant be ! CreateTemporalInstant(roundedNs).
    auto* rounded_instant = MUST(create_temporal_instant(vm, *rounded_ns));

    // 10. Return ? TemporalInstantToString(roundedInstant, timeZone, precision.[[Precision]]).
    return PrimitiveString::create(vm, TRY(temporal_instant_to_string(vm, *rounded_instant, time_zone, precision.precision)));
}

// 8.3.14 Temporal.Instant.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.tolocalestring
// NOTE: This is the minimum toLocaleString implementation for engines without ECMA-402.
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::to_locale_string)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Return ? TemporalInstantToString(instant, undefined, "auto").
    return PrimitiveString::create(vm, TRY(temporal_instant_to_string(vm, instant, js_undefined(), "auto"sv)));
}

// 8.3.15 Temporal.Instant.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::to_json)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Return ? TemporalInstantToString(instant, undefined, "auto").
    return PrimitiveString::create(vm, TRY(temporal_instant_to_string(vm, instant, js_undefined(), "auto"sv)));
}

// 8.3.16 Temporal.Instant.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.Instant", "a primitive value");
}

// 8.3.17 Temporal.Instant.prototype.toZonedDateTime ( item ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.tozoneddatetime
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::to_zoned_date_time)
{
    auto item = vm.argument(0);

    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. If Type(item) is not Object, then
    if (!item.is_object()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, item);
    }

    // 4. Let calendarLike be ? Get(item, "calendar").
    auto calendar_like = TRY(item.as_object().get(vm.names.calendar));

    // 5. If calendarLike is undefined, then
    if (calendar_like.is_undefined()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, vm.names.calendar.as_string());
    }

    // 6. Let calendar be ? ToTemporalCalendar(calendarLike).
    auto* calendar = TRY(to_temporal_calendar(vm, calendar_like));

    // 7. Let temporalTimeZoneLike be ? Get(item, "timeZone").
    auto temporal_time_zone_like = TRY(item.as_object().get(vm.names.timeZone));

    // 8. If temporalTimeZoneLike is undefined, then
    if (temporal_time_zone_like.is_undefined()) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, vm.names.timeZone.as_string());
    }

    // 9. Let timeZone be ? ToTemporalTimeZone(temporalTimeZoneLike).
    auto* time_zone = TRY(to_temporal_time_zone(vm, temporal_time_zone_like));

    // 10. Return ? CreateTemporalZonedDateTime(instant.[[Nanoseconds]], timeZone, calendar).
    return TRY(create_temporal_zoned_date_time(vm, instant->nanoseconds(), *time_zone, *calendar));
}

// 8.3.18 Temporal.Instant.prototype.toZonedDateTimeISO ( timeZone ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.tozoneddatetimeiso
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::to_zoned_date_time_iso)
{
    // 1. Let instant be the this value.
    // 2. Perform ? RequireInternalSlot(instant, [[InitializedTemporalInstant]]).
    auto instant = TRY(typed_this_object(vm));

    // 3. Set timeZone to ? ToTemporalTimeZone(timeZone).
    auto* time_zone = TRY(to_temporal_time_zone(vm, vm.argument(0)));

    // 4. Let calendar be ! GetISO8601Calendar().
    auto* calendar = get_iso8601_calendar(vm);

    // 5. Return ? CreateTemporalZonedDateTime(instant.[[Nanoseconds]], timeZone, calendar).
    return TRY(create_temporal_zoned_date_time(vm, instant->nanoseconds(), *time_zone, *calendar));
}

}
