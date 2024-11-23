/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toJSON, to_json, 0, attr);
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

}
