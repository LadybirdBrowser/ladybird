/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/PlainTimePrototype.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainTimePrototype);

// 4.3 Properties of the Temporal.PlainTime Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-plaintime-prototype-object
PlainTimePrototype::PlainTimePrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void PlainTimePrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 4.3.2 Temporal.PlainTime.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.PlainTime"_string), Attribute::Configurable);

    define_native_accessor(realm, vm.names.hour, hour_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.minute, minute_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.second, second_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.millisecond, millisecond_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.microsecond, microsecond_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.nanosecond, nanosecond_getter, {}, Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.add, add, 1, attr);
    define_native_function(realm, vm.names.subtract, subtract, 1, attr);
    define_native_function(realm, vm.names.with, with, 1, attr);
    define_native_function(realm, vm.names.until, until, 1, attr);
    define_native_function(realm, vm.names.since, since, 1, attr);
    define_native_function(realm, vm.names.round, round, 1, attr);
    define_native_function(realm, vm.names.equals, equals, 1, attr);
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 0, attr);
    define_native_function(realm, vm.names.valueOf, value_of, 0, attr);
}

// 4.3.3 get Temporal.PlainTime.prototype.hour, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.hour
// 4.3.4 get Temporal.PlainTime.prototype.minute, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.minute
// 4.3.5 get Temporal.PlainTime.prototype.second, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.second
// 4.3.6 get Temporal.PlainTime.prototype.millisecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.millisecond
// 4.3.7 get Temporal.PlainTime.prototype.microsecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.microsecond
// 4.3.8 get Temporal.PlainTime.prototype.nanosecond, https://tc39.es/proposal-temporal/#sec-get-temporal.plaintime.prototype.microsecond
#define JS_ENUMERATE_PLAIN_TIME_FIELDS \
    __JS_ENUMERATE(hour)               \
    __JS_ENUMERATE(minute)             \
    __JS_ENUMERATE(second)             \
    __JS_ENUMERATE(millisecond)        \
    __JS_ENUMERATE(microsecond)        \
    __JS_ENUMERATE(nanosecond)

#define __JS_ENUMERATE(field)                                                              \
    JS_DEFINE_NATIVE_FUNCTION(PlainTimePrototype::field##_getter)                          \
    {                                                                                      \
        /* 1. Let temporalTime be the this value. */                                       \
        /* 2. Perform ? RequireInternalSlot(temporalTime, [[InitializedTemporalTime]]). */ \
        auto temporal_time = TRY(typed_this_object(vm));                                   \
                                                                                           \
        /* 3. Return 𝔽(temporalTime.[[Time]].[[<field>]]). */                           \
        return temporal_time->time().field;                                                \
    }
JS_ENUMERATE_PLAIN_TIME_FIELDS
#undef __JS_ENUMERATE

// 4.3.9 Temporal.PlainTime.prototype.add ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.add
JS_DEFINE_NATIVE_FUNCTION(PlainTimePrototype::add)
{
    auto temporal_duration_like = vm.argument(0);

    // 1. Let temporalTime be the this value.
    // 2. Perform ? RequireInternalSlot(temporalTime, [[InitializedTemporalTime]]).
    auto temporal_time = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToTime(ADD, temporalTime, temporalDurationLike).
    return TRY(add_duration_to_time(vm, ArithmeticOperation::Add, temporal_time, temporal_duration_like));
}

// 4.3.10 Temporal.PlainTime.prototype.subtract ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.subtract
JS_DEFINE_NATIVE_FUNCTION(PlainTimePrototype::subtract)
{
    auto temporal_duration_like = vm.argument(0);

    // 1. Let temporalTime be the this value.
    // 2. Perform ? RequireInternalSlot(temporalTime, [[InitializedTemporalTime]]).
    auto temporal_time = TRY(typed_this_object(vm));

    // 3. Return ? AddDurationToTime(SUBTRACT, temporalTime, temporalDurationLike).
    return TRY(add_duration_to_time(vm, ArithmeticOperation::Subtract, temporal_time, temporal_duration_like));
}

// 4.3.11 Temporal.PlainTime.prototype.with ( temporalTimeLike [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.with
JS_DEFINE_NATIVE_FUNCTION(PlainTimePrototype::with)
{
    auto temporal_time_like = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let temporalTime be the this value.
    // 2. Perform ? RequireInternalSlot(temporalTime, [[InitializedTemporalTime]]).
    auto temporal_time = TRY(typed_this_object(vm));

    // 3. If ? IsPartialTemporalObject(temporalTimeLike) is false, throw a TypeError exception.
    if (!TRY(is_partial_temporal_object(vm, temporal_time_like)))
        return vm.throw_completion<TypeError>(ErrorType::TemporalObjectMustBePartialTemporalObject);

    // 4. Let partialTime be ? ToTemporalTimeRecord(temporalTimeLike, PARTIAL).
    auto partial_time = TRY(to_temporal_time_record(vm, temporal_time_like.as_object(), Completeness::Partial));

    // 5. If partialTime.[[Hour]] is not undefined, then
    //     a. Let hour be partialTime.[[Hour]].
    // 6. Else,
    //     a. Let hour be temporalTime.[[Time]].[[Hour]].
    auto hour = partial_time.hour.value_or(temporal_time->time().hour);

    // 7. If partialTime.[[Minute]] is not undefined, then
    //     a. Let minute be partialTime.[[Minute]].
    // 8. Else,
    //     a. Let minute be temporalTime.[[Time]].[[Minute]].
    auto minute = partial_time.minute.value_or(temporal_time->time().minute);

    // 9. If partialTime.[[Second]] is not undefined, then
    //     a. Let second be partialTime.[[Second]].
    // 10. Else,
    //     a. Let second be temporalTime.[[Time]].[[Second]].
    auto second = partial_time.second.value_or(temporal_time->time().second);

    // 11. If partialTime.[[Millisecond]] is not undefined, then
    //     a. Let millisecond be partialTime.[[Millisecond]].
    // 12. Else,
    //     a. Let millisecond be temporalTime.[[Time]].[[Millisecond]].
    auto millisecond = partial_time.millisecond.value_or(temporal_time->time().millisecond);

    // 13. If partialTime.[[Microsecond]] is not undefined, then
    //     a. Let microsecond be partialTime.[[Microsecond]].
    // 14. Else,
    //     a. Let microsecond be temporalTime.[[Time]].[[Microsecond]].
    auto microsecond = partial_time.microsecond.value_or(temporal_time->time().microsecond);

    // 15. If partialTime.[[Nanosecond]] is not undefined, then
    //     a. Let nanosecond be partialTime.[[Nanosecond]].
    // 16. Else,
    //     a. Let nanosecond be temporalTime.[[Time]].[[Nanosecond]].
    auto nanosecond = partial_time.nanosecond.value_or(temporal_time->time().nanosecond);

    // 17. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 18. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
    auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

    // 19. Let result be ? RegulateTime(hour, minute, second, millisecond, microsecond, nanosecond, overflow).
    auto result = TRY(regulate_time(vm, hour, minute, second, millisecond, microsecond, nanosecond, overflow));

    // 20. Return ! CreateTemporalTime(result).
    return MUST(create_temporal_time(vm, result));
}

// 4.3.12 Temporal.PlainTime.prototype.until ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.until
JS_DEFINE_NATIVE_FUNCTION(PlainTimePrototype::until)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let temporalTime be the this value.
    // 2. Perform ? RequireInternalSlot(temporalTime, [[InitializedTemporalTime]]).
    auto temporal_time = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalPlainTime(UNTIL, temporalTime, other, options).
    return TRY(difference_temporal_plain_time(vm, DurationOperation::Until, temporal_time, other, options));
}

// 4.3.13 Temporal.PlainTime.prototype.since ( other [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.since
JS_DEFINE_NATIVE_FUNCTION(PlainTimePrototype::since)
{
    auto other = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let temporalTime be the this value.
    // 2. Perform ? RequireInternalSlot(temporalTime, [[InitializedTemporalTime]]).
    auto temporal_time = TRY(typed_this_object(vm));

    // 3. Return ? DifferenceTemporalPlainTime(SINCE, temporalTime, other, options).
    return TRY(difference_temporal_plain_time(vm, DurationOperation::Since, temporal_time, other, options));
}

// 4.3.14 Temporal.PlainTime.prototype.round ( roundTo ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.round
JS_DEFINE_NATIVE_FUNCTION(PlainTimePrototype::round)
{
    auto& realm = *vm.current_realm();

    auto round_to_value = vm.argument(0);

    // 1. Let temporalTime be the this value.
    // 2. Perform ? RequireInternalSlot(temporalTime, [[InitializedTemporalTime]]).
    auto temporal_time = TRY(typed_this_object(vm));

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

    // 10. Let maximum be MaximumTemporalDurationRoundingIncrement(smallestUnit).
    auto maximum = maximum_temporal_duration_rounding_increment(smallest_unit_value);

    // 11. Assert: maximum is not UNSET.
    VERIFY(!maximum.has<Unset>());

    // 12. Perform ? ValidateTemporalRoundingIncrement(roundingIncrement, maximum, false).
    TRY(validate_temporal_rounding_increment(vm, rounding_increment, maximum.get<u64>(), false));

    // 13. Let result be RoundTime(temporalTime.[[Time]], roundingIncrement, smallestUnit, roundingMode).
    auto result = round_time(temporal_time->time(), rounding_increment, smallest_unit_value, rounding_mode);

    // 14. Return ! CreateTemporalTime(result).
    return MUST(create_temporal_time(vm, result));
}

// 4.3.15 Temporal.PlainTime.prototype.equals ( other ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.equals
JS_DEFINE_NATIVE_FUNCTION(PlainTimePrototype::equals)
{
    // 1. Let temporalTime be the this value.
    // 2. Perform ? RequireInternalSlot(temporalTime, [[InitializedTemporalTime]]).
    auto temporal_time = TRY(typed_this_object(vm));

    // 3. Set other to ? ToTemporalTime(other).
    auto other = TRY(to_temporal_time(vm, vm.argument(0)));

    // 4. If CompareTimeRecord(temporalTime.[[Time]], other.[[Time]]) = 0, return true.
    if (compare_time_record(temporal_time->time(), other->time()) == 0)
        return true;

    // 5. Return false.
    return false;
}

// 4.3.16 Temporal.PlainTime.prototype.toString ( [ options ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(PlainTimePrototype::to_string)
{
    // 1. Let temporalTime be the this value.
    // 2. Perform ? RequireInternalSlot(temporalTime, [[InitializedTemporalTime]]).
    auto temporal_time = TRY(typed_this_object(vm));

    // 3. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, vm.argument(0)));

    // 4. NOTE: The following steps read options and perform independent validation in alphabetical order
    //    (GetTemporalFractionalSecondDigitsOption reads "fractionalSecondDigits" and GetRoundingModeOption reads "roundingMode").

    // 5. Let digits be ? GetTemporalFractionalSecondDigitsOption(resolvedOptions).
    auto digits = TRY(get_temporal_fractional_second_digits_option(vm, resolved_options));

    // 6. Let roundingMode be ? GetRoundingModeOption(resolvedOptions, TRUNC).
    auto rounding_mode = TRY(get_rounding_mode_option(vm, resolved_options, RoundingMode::Trunc));

    // 7. Let smallestUnit be ? GetTemporalUnitValuedOption(resolvedOptions, "smallestUnit", TIME, UNSET).
    auto smallest_unit = TRY(get_temporal_unit_valued_option(vm, resolved_options, vm.names.smallestUnit, UnitGroup::Time, Unset {}));

    // 8. If smallestUnit is HOUR, throw a RangeError exception.
    if (auto const* unit = smallest_unit.get_pointer<Unit>(); unit && *unit == Unit::Hour)
        return vm.throw_completion<RangeError>(ErrorType::OptionIsNotValidValue, temporal_unit_to_string(*unit), vm.names.smallestUnit);

    // 9. Let precision be ToSecondsStringPrecisionRecord(smallestUnit, digits).
    auto precision = to_seconds_string_precision_record(smallest_unit, digits);

    // 10. Let roundResult be RoundTime(temporalTime.[[Time]], precision.[[Increment]], precision.[[Unit]], roundingMode).
    auto round_result = round_time(temporal_time->time(), precision.increment, precision.unit, rounding_mode);

    // 11. Return TimeRecordToString(roundResult, precision.[[Precision]]).
    return PrimitiveString::create(vm, time_record_to_string(round_result, precision.precision));
}

// 4.3.17 Temporal.PlainTime.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.tolocalestring
JS_DEFINE_NATIVE_FUNCTION(PlainTimePrototype::to_locale_string)
{
    // 1. Let temporalTime be the this value.
    // 2. Perform ? RequireInternalSlot(temporalTime, [[InitializedTemporalTime]]).
    auto temporal_time = TRY(typed_this_object(vm));

    // 3. Return TimeRecordToString(temporalTime.[[Time]], AUTO).
    return PrimitiveString::create(vm, time_record_to_string(temporal_time->time(), Auto {}));
}

// 4.3.18 Temporal.PlainTime.prototype.toJSON ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.tojson
JS_DEFINE_NATIVE_FUNCTION(PlainTimePrototype::to_json)
{
    // 1. Let temporalTime be the this value.
    // 2. Perform ? RequireInternalSlot(temporalTime, [[InitializedTemporalTime]]).
    auto temporal_time = TRY(typed_this_object(vm));

    // 3. Return TimeRecordToString(temporalTime.[[Time]], AUTO).
    return PrimitiveString::create(vm, time_record_to_string(temporal_time->time(), Auto {}));
}

// 4.3.19 Temporal.PlainTime.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.plaintime.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(PlainTimePrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.PlainTime", "a primitive value");
}

}
