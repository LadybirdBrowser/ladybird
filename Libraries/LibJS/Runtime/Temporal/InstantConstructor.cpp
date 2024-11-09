/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/InstantConstructor.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(InstantConstructor);

// 8.1 The Temporal.Instant Constructor, https://tc39.es/proposal-temporal/#sec-temporal-instant-constructor
InstantConstructor::InstantConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Instant.as_string(), realm.intrinsics().function_prototype())
{
}

void InstantConstructor::initialize(Realm& realm)
{
    Base::initialize(realm);

    auto& vm = this->vm();

    // 8.2.1 Temporal.Instant.prototype, https://tc39.es/proposal-temporal/#sec-temporal.instant.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().temporal_instant_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.from, from, 1, attr);
    define_native_function(realm, vm.names.fromEpochSeconds, from_epoch_seconds, 1, attr);
    define_native_function(realm, vm.names.fromEpochMilliseconds, from_epoch_milliseconds, 1, attr);
    define_native_function(realm, vm.names.fromEpochMicroseconds, from_epoch_microseconds, 1, attr);
    define_native_function(realm, vm.names.fromEpochNanoseconds, from_epoch_nanoseconds, 1, attr);
    define_native_function(realm, vm.names.compare, compare, 2, attr);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// 8.1.1 Temporal.Instant ( epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal.instant
ThrowCompletionOr<Value> InstantConstructor::call()
{
    auto& vm = this->vm();

    // 1. If NewTarget is undefined, then
    // a. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, "Temporal.Instant");
}

// 8.1.1 Temporal.Instant ( epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal.instant
ThrowCompletionOr<NonnullGCPtr<Object>> InstantConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    // 2. Let epochNanoseconds be ? ToBigInt(epochNanoseconds).
    auto epoch_nanoseconds = TRY(vm.argument(0).to_bigint(vm));

    // 3. If ! IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
    if (!is_valid_epoch_nanoseconds(epoch_nanoseconds))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

    // 4. Return ? CreateTemporalInstant(epochNanoseconds, NewTarget).
    return *TRY(create_temporal_instant(vm, epoch_nanoseconds, &new_target));
}

// 8.2.2 Temporal.Instant.from ( item ), https://tc39.es/proposal-temporal/#sec-temporal.instant.from
JS_DEFINE_NATIVE_FUNCTION(InstantConstructor::from)
{
    auto item = vm.argument(0);

    // 1. If Type(item) is Object and item has an [[InitializedTemporalInstant]] internal slot, then
    if (item.is_object() && is<Instant>(item.as_object())) {
        // a. Return ! CreateTemporalInstant(item.[[Nanoseconds]]).
        return MUST(create_temporal_instant(vm, BigInt::create(vm, static_cast<Instant&>(item.as_object()).nanoseconds().big_integer())));
    }

    // 2. Return ? ToTemporalInstant(item).
    return TRY(to_temporal_instant(vm, item));
}

// 8.2.3 Temporal.Instant.fromEpochSeconds ( epochSeconds ), https://tc39.es/proposal-temporal/#sec-temporal.instant.fromepochseconds
JS_DEFINE_NATIVE_FUNCTION(InstantConstructor::from_epoch_seconds)
{
    // 1. Set epochSeconds to ? ToNumber(epochSeconds).
    auto epoch_seconds_value = TRY(vm.argument(0).to_number(vm));

    // 2. Set epochSeconds to ? NumberToBigInt(epochSeconds).
    auto* epoch_seconds = TRY(number_to_bigint(vm, epoch_seconds_value));

    // 3. Let epochNanoseconds be epochSeconds × 10^9ℤ.
    auto epoch_nanoseconds = BigInt::create(vm, epoch_seconds->big_integer().multiplied_by(Crypto::UnsignedBigInteger { 1'000'000'000 }));

    // 4. If ! IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
    if (!is_valid_epoch_nanoseconds(epoch_nanoseconds))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

    // 5. Return ! CreateTemporalInstant(epochNanoseconds).
    return MUST(create_temporal_instant(vm, epoch_nanoseconds));
}

// 8.2.4 Temporal.Instant.fromEpochMilliseconds ( epochMilliseconds ), https://tc39.es/proposal-temporal/#sec-temporal.instant.fromepochmilliseconds
JS_DEFINE_NATIVE_FUNCTION(InstantConstructor::from_epoch_milliseconds)
{
    // 1. Set epochMilliseconds to ? ToNumber(epochMilliseconds).
    auto epoch_milliseconds_value = TRY(vm.argument(0).to_number(vm));

    // 2. Set epochMilliseconds to ? NumberToBigInt(epochMilliseconds).
    auto* epoch_milliseconds = TRY(number_to_bigint(vm, epoch_milliseconds_value));

    // 3. Let epochNanoseconds be epochMilliseconds × 10^6ℤ.
    auto epoch_nanoseconds = BigInt::create(vm, epoch_milliseconds->big_integer().multiplied_by(Crypto::UnsignedBigInteger { 1'000'000 }));

    // 4. If ! IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
    if (!is_valid_epoch_nanoseconds(epoch_nanoseconds))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

    // 5. Return ! CreateTemporalInstant(epochNanoseconds).
    return MUST(create_temporal_instant(vm, epoch_nanoseconds));
}

// 8.2.5 Temporal.Instant.fromEpochMicroseconds ( epochMicroseconds ), https://tc39.es/proposal-temporal/#sec-temporal.instant.fromepochmicroseconds
JS_DEFINE_NATIVE_FUNCTION(InstantConstructor::from_epoch_microseconds)
{
    // 1. Set epochMicroseconds to ? ToBigInt(epochMicroseconds).
    auto epoch_microseconds = TRY(vm.argument(0).to_bigint(vm));

    // 2. Let epochNanoseconds be epochMicroseconds × 1000ℤ.
    auto epoch_nanoseconds = BigInt::create(vm, epoch_microseconds->big_integer().multiplied_by(Crypto::UnsignedBigInteger { 1'000 }));

    // 3. If ! IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
    if (!is_valid_epoch_nanoseconds(epoch_nanoseconds))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

    // 4. Return ! CreateTemporalInstant(epochNanoseconds).
    return MUST(create_temporal_instant(vm, epoch_nanoseconds));
}

// 8.2.6 Temporal.Instant.fromEpochNanoseconds ( epochNanoseconds ), https://tc39.es/proposal-temporal/#sec-temporal.instant.fromepochnanoseconds
JS_DEFINE_NATIVE_FUNCTION(InstantConstructor::from_epoch_nanoseconds)
{
    // 1. Set epochNanoseconds to ? ToBigInt(epochNanoseconds).
    auto epoch_nanoseconds = TRY(vm.argument(0).to_bigint(vm));

    // 2. If ! IsValidEpochNanoseconds(epochNanoseconds) is false, throw a RangeError exception.
    if (!is_valid_epoch_nanoseconds(epoch_nanoseconds))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidEpochNanoseconds);

    // 3. Return ! CreateTemporalInstant(epochNanoseconds).
    return MUST(create_temporal_instant(vm, epoch_nanoseconds));
}

// 8.2.7 Temporal.Instant.compare ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal.instant.compare
JS_DEFINE_NATIVE_FUNCTION(InstantConstructor::compare)
{
    // 1. Set one to ? ToTemporalInstant(one).
    auto* one = TRY(to_temporal_instant(vm, vm.argument(0)));

    // 2. Set two to ? ToTemporalInstant(two).
    auto* two = TRY(to_temporal_instant(vm, vm.argument(1)));

    // 3. Return 𝔽(! CompareEpochNanoseconds(one.[[Nanoseconds]], two.[[Nanoseconds]])).
    return Value(compare_epoch_nanoseconds(one->nanoseconds(), two->nanoseconds()));
}

}
