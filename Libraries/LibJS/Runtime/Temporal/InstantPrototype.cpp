/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/InstantPrototype.h>

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

// 8.3.14 Temporal.Instant.prototype.valueOf ( ), https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(InstantPrototype::value_of)
{
    // 1. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::Convert, "Temporal.Instant", "a primitive value");
}

}
