/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/DurationPrototype.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(DurationPrototype);

// 7.3 Properties of the Temporal.Duration Prototype Object, https://tc39.es/proposal-temporal/#sec-properties-of-the-temporal-duration-prototype-object
DurationPrototype::DurationPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().object_prototype())
{
}

void DurationPrototype::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 7.3.2 Temporal.Duration.prototype[ %Symbol.toStringTag% ], https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype-%symbol.tostringtag%
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Temporal.Duration"_string), Attribute::Configurable);

#define __JS_ENUMERATE(unit) \
    define_native_accessor(realm, vm.names.unit, unit##_getter, {}, Attribute::Configurable);
    JS_ENUMERATE_DURATION_UNITS
#undef __JS_ENUMERATE

    define_native_accessor(realm, vm.names.sign, sign_getter, {}, Attribute::Configurable);
    define_native_accessor(realm, vm.names.blank, blank_getter, {}, Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.with, with, 1, attr);
    define_native_function(realm, vm.names.negated, negated, 0, attr);
    define_native_function(realm, vm.names.abs, abs, 0, attr);
    define_native_function(realm, vm.names.add, add, 1, attr);
    define_native_function(realm, vm.names.subtract, subtract, 1, attr);
}

// 7.3.3 get Temporal.Duration.prototype.years, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.years
// 7.3.4 get Temporal.Duration.prototype.months, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.months
// 7.3.5 get Temporal.Duration.prototype.weeks, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.weeks
// 7.3.6 get Temporal.Duration.prototype.days, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.days
// 7.3.7 get Temporal.Duration.prototype.hours, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.hours
// 7.3.8 get Temporal.Duration.prototype.minutes, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.minutes
// 7.3.9 get Temporal.Duration.prototype.seconds, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.seconds
// 7.3.10 get Temporal.Duration.prototype.milliseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.milliseconds
// 7.3.11 get Temporal.Duration.prototype.microseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.microseconds
// 7.3.12 get Temporal.Duration.prototype.nanoseconds, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.nanoseconds
#define __JS_ENUMERATE(unit)                                                               \
    JS_DEFINE_NATIVE_FUNCTION(DurationPrototype::unit##_getter)                            \
    {                                                                                      \
        /* 1. Let duration be the this value. */                                           \
        /* 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]). */ \
        auto duration = TRY(typed_this_object(vm));                                        \
                                                                                           \
        /* 3. Return 𝔽(duration.[[<unit>]]). */                                         \
        return duration->unit();                                                           \
    }
JS_ENUMERATE_DURATION_UNITS
#undef __JS_ENUMERATE

// 7.3.13 get Temporal.Duration.prototype.sign, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.sign
JS_DEFINE_NATIVE_FUNCTION(DurationPrototype::sign_getter)
{
    // 1. Let duration be the this value.
    // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
    auto duration = TRY(typed_this_object(vm));

    // 3. Return 𝔽(DurationSign(duration)).
    return duration_sign(duration);
}

// 7.3.14 get Temporal.Duration.prototype.blank, https://tc39.es/proposal-temporal/#sec-get-temporal.duration.prototype.blank
JS_DEFINE_NATIVE_FUNCTION(DurationPrototype::blank_getter)
{
    // 1. Let duration be the this value.
    // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
    auto duration = TRY(typed_this_object(vm));

    // 3. If DurationSign(duration) = 0, return true.
    if (duration_sign(duration) == 0)
        return true;

    // 4. Return false.
    return false;
}

// 7.3.15 Temporal.Duration.prototype.with ( temporalDurationLike ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.with
JS_DEFINE_NATIVE_FUNCTION(DurationPrototype::with)
{
    // 1. Let duration be the this value.
    // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
    auto duration = TRY(typed_this_object(vm));

    // 3. Let temporalDurationLike be ? ToTemporalPartialDurationRecord(temporalDurationLike).
    auto temporal_duration_like = TRY(to_temporal_partial_duration_record(vm, vm.argument(0)));

    // 4. If temporalDurationLike.[[Years]] is not undefined, then
    //     a. Let years be temporalDurationLike.[[Years]].
    // 5. Else,
    //     a. Let years be duration.[[Years]].
    auto years = temporal_duration_like.years.value_or(duration->years());

    // 6. If temporalDurationLike.[[Months]] is not undefined, then
    //     a. Let months be temporalDurationLike.[[Months]].
    // 7. Else,
    //     a. Let months be duration.[[Months]].
    auto months = temporal_duration_like.months.value_or(duration->months());

    // 8. If temporalDurationLike.[[Weeks]] is not undefined, then
    //     a. Let weeks be temporalDurationLike.[[Weeks]].
    // 9. Else,
    //     a. Let weeks be duration.[[Weeks]].
    auto weeks = temporal_duration_like.weeks.value_or(duration->weeks());

    // 10. If temporalDurationLike.[[Days]] is not undefined, then
    //     a. Let days be temporalDurationLike.[[Days]].
    // 11. Else,
    //     a. Let days be duration.[[Days]].
    auto days = temporal_duration_like.days.value_or(duration->days());

    // 12. If temporalDurationLike.[[Hours]] is not undefined, then
    //     a. Let hours be temporalDurationLike.[[Hours]].
    // 13. Else,
    //     a. Let hours be duration.[[Hours]].
    auto hours = temporal_duration_like.hours.value_or(duration->hours());

    // 14. If temporalDurationLike.[[Minutes]] is not undefined, then
    //     a. Let minutes be temporalDurationLike.[[Minutes]].
    // 15. Else,
    //     a. Let minutes be duration.[[Minutes]].
    auto minutes = temporal_duration_like.minutes.value_or(duration->minutes());

    // 16. If temporalDurationLike.[[Seconds]] is not undefined, then
    //     a. Let seconds be temporalDurationLike.[[Seconds]].
    // 17. Else,
    //     a. Let seconds be duration.[[Seconds]].
    auto seconds = temporal_duration_like.seconds.value_or(duration->seconds());

    // 18. If temporalDurationLike.[[Milliseconds]] is not undefined, then
    //     a. Let milliseconds be temporalDurationLike.[[Milliseconds]].
    // 19. Else,
    //     a. Let milliseconds be duration.[[Milliseconds]].
    auto milliseconds = temporal_duration_like.milliseconds.value_or(duration->milliseconds());

    // 20. If temporalDurationLike.[[Microseconds]] is not undefined, then
    //     a. Let microseconds be temporalDurationLike.[[Microseconds]].
    // 21. Else,
    //     a. Let microseconds be duration.[[Microseconds]].
    auto microseconds = temporal_duration_like.microseconds.value_or(duration->microseconds());

    // 22. If temporalDurationLike.[[Nanoseconds]] is not undefined, then
    //     a. Let nanoseconds be temporalDurationLike.[[Nanoseconds]].
    // 23. Else,
    //     a. Let nanoseconds be duration.[[Nanoseconds]].
    auto nanoseconds = temporal_duration_like.nanoseconds.value_or(duration->nanoseconds());

    // 24. Return ? CreateTemporalDuration(years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds).
    return TRY(create_temporal_duration(vm, years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds));
}

// 7.3.16 Temporal.Duration.prototype.negated ( ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.negated
JS_DEFINE_NATIVE_FUNCTION(DurationPrototype::negated)
{
    // 1. Let duration be the this value.
    // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
    auto duration = TRY(typed_this_object(vm));

    // 3. Return CreateNegatedTemporalDuration(duration).
    return create_negated_temporal_duration(vm, duration);
}

// 7.3.17 Temporal.Duration.prototype.abs ( ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.abs
JS_DEFINE_NATIVE_FUNCTION(DurationPrototype::abs)
{
    // 1. Let duration be the this value.
    // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
    auto duration = TRY(typed_this_object(vm));

    // 3. Return ! CreateTemporalDuration(abs(duration.[[Years]]), abs(duration.[[Months]]), abs(duration.[[Weeks]]), abs(duration.[[Days]]), abs(duration.[[Hours]]), abs(duration.[[Minutes]]), abs(duration.[[Seconds]]), abs(duration.[[Milliseconds]]), abs(duration.[[Microseconds]]), abs(duration.[[Nanoseconds]])).
    return MUST(create_temporal_duration(vm, fabs(duration->years()), fabs(duration->months()), fabs(duration->weeks()), fabs(duration->days()), fabs(duration->hours()), fabs(duration->minutes()), fabs(duration->seconds()), fabs(duration->milliseconds()), fabs(duration->microseconds()), fabs(duration->nanoseconds())));
}

// 7.3.18 Temporal.Duration.prototype.add ( other ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.add
JS_DEFINE_NATIVE_FUNCTION(DurationPrototype::add)
{
    auto other = vm.argument(0);

    // 1. Let duration be the this value.
    // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
    auto duration = TRY(typed_this_object(vm));

    // 3. Return ? AddDurations(ADD, duration, other).
    return TRY(add_durations(vm, ArithmeticOperation::Add, duration, other));
}

// 7.3.19 Temporal.Duration.prototype.subtract ( other ), https://tc39.es/proposal-temporal/#sec-temporal.duration.prototype.subtract
JS_DEFINE_NATIVE_FUNCTION(DurationPrototype::subtract)
{
    auto other = vm.argument(0);

    // 1. Let duration be the this value.
    // 2. Perform ? RequireInternalSlot(duration, [[InitializedTemporalDuration]]).
    auto duration = TRY(typed_this_object(vm));

    // 3. Return ? AddDurations(SUBTRACT, duration, other).
    return TRY(add_durations(vm, ArithmeticOperation::Subtract, duration, other));
}

}
