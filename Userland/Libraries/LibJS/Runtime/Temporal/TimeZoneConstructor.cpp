/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/TimeZoneConstructor.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(TimeZoneConstructor);

// 11.2 The Temporal.TimeZone Constructor, https://tc39.es/proposal-temporal/#sec-temporal-timezone-constructor
TimeZoneConstructor::TimeZoneConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.TimeZone.as_string(), realm.intrinsics().function_prototype())
{
}

void TimeZoneConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 11.3.1 Temporal.TimeZone.prototype, https://tc39.es/proposal-temporal/#sec-temporal.timezone.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().temporal_time_zone_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.from, from, 1, attr);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// 11.2.1 Temporal.TimeZone ( identifier ), https://tc39.es/proposal-temporal/#sec-temporal.timezone
ThrowCompletionOr<Value> TimeZoneConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, then
    // a. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Temporal.TimeZone");
}

// 11.2.1 Temporal.TimeZone ( identifier ), https://tc39.es/proposal-temporal/#sec-temporal.timezone
ThrowCompletionOr<GC::Ref<Object>> TimeZoneConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 2. Set identifier to ? ToString(identifier).
    auto identifier = TRY(vm.argument(0).to_string(vm));

    // 3. If IsTimeZoneOffsetString(identifier) is false, then
    if (!is_time_zone_offset_string(identifier)) {
        // a. If IsAvailableTimeZoneName(identifier) is false, then
        if (!is_available_time_zone_name(identifier)) {
            // i. Throw a RangeError exception.
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidTimeZoneName, identifier);
        }

        // b. Set identifier to ! CanonicalizeTimeZoneName(identifier).
        identifier = MUST_OR_THROW_OOM(canonicalize_time_zone_name(vm, identifier));
    }

    // 4. Return ? CreateTemporalTimeZone(identifier, NewTarget).
    return *TRY(create_temporal_time_zone(vm, move(identifier), &new_target));
}

// 11.3.2 Temporal.TimeZone.from ( item ), https://tc39.es/proposal-temporal/#sec-temporal.timezone.from
JS_DEFINE_NATIVE_FUNCTION(TimeZoneConstructor::from)
{
    auto item = vm.argument(0);

    // 1. Return ? ToTemporalTimeZone(item).
    return TRY(to_temporal_time_zone(vm, item));
}

}
