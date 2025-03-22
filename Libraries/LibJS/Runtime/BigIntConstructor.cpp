/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/BigIntConstructor.h>
#include <LibJS/Runtime/BigIntObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

GC_DEFINE_ALLOCATOR(BigIntConstructor);

static Crypto::SignedBigInteger const BIGINT_ONE { 1 };
static Crypto::SignedBigInteger const BIGINT_ZERO { 0 };

BigIntConstructor::BigIntConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.BigInt.as_string(), realm.intrinsics().function_prototype())
{
}

void BigIntConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 21.2.2.3 BigInt.prototype, https://tc39.es/ecma262/#sec-bigint.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().bigint_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.asIntN, as_int_n, 2, attr);
    define_native_function(realm, vm.names.asUintN, as_uint_n, 2, attr);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// 21.2.1.1 BigInt ( value ), https://tc39.es/ecma262/#sec-bigint-constructor-number-value
ThrowCompletionOr<Value> BigIntConstructor::call()
{
    auto& vm = this->vm();

    auto value = vm.argument(0);

    // 2. Let prim be ? ToPrimitive(value, number).
    auto primitive = TRY(value.to_primitive(vm, Value::PreferredType::Number));

    // 3. If Type(prim) is Number, return ? NumberToBigInt(prim).
    if (primitive.is_number())
        return TRY(number_to_bigint(vm, primitive));

    // 4. Otherwise, return ? ToBigInt(prim).
    return TRY(primitive.to_bigint(vm));
}

// 21.2.1.1 BigInt ( value ), https://tc39.es/ecma262/#sec-bigint-constructor-number-value
ThrowCompletionOr<GC::Ref<Object>> BigIntConstructor::construct(FunctionObject&)
{
    return vm().throw_completion<TypeError>(ErrorType::NotAConstructor, "BigInt");
}

// 21.2.2.1 BigInt.asIntN ( bits, bigint ), https://tc39.es/ecma262/#sec-bigint.asintn
JS_DEFINE_NATIVE_FUNCTION(BigIntConstructor::as_int_n)
{
    // 1. Set bits to ? ToIndex(bits).
    auto bits = TRY(vm.argument(0).to_index(vm));

    // 2. Set bigint to ? ToBigInt(bigint).
    auto bigint = TRY(vm.argument(1).to_bigint(vm));

    // OPTIMIZATION: mod = bigint (mod 2^0) = 0 < 2^(0-1) = 0.5
    if (bits == 0)
        return BigInt::create(vm, BIGINT_ZERO);

    // 3. Let mod be ℝ(bigint) modulo 2^bits.
    auto const mod = TRY_OR_THROW_OOM(vm, bigint->big_integer().mod_power_of_two(bits));

    // OPTIMIZATION: mod < 2^(bits-1)
    if (mod.is_zero())
        return BigInt::create(vm, BIGINT_ZERO);

    // 4. If mod ≥ 2^(bits-1), return ℤ(mod - 2^bits); ...
    if (auto top_bit_index = mod.unsigned_value().one_based_index_of_highest_set_bit(); top_bit_index >= bits) {
        // twos complement decode
        auto decoded = TRY_OR_THROW_OOM(vm, mod.unsigned_value().try_bitwise_not_fill_to_one_based_index(bits)).plus(1);
        return BigInt::create(vm, Crypto::SignedBigInteger { std::move(decoded), true });
    }

    // ... otherwise, return ℤ(mod).
    return BigInt::create(vm, mod);
}

// 21.2.2.2 BigInt.asUintN ( bits, bigint ), https://tc39.es/ecma262/#sec-bigint.asuintn
JS_DEFINE_NATIVE_FUNCTION(BigIntConstructor::as_uint_n)
{
    // 1. Set bits to ? ToIndex(bits).
    auto bits = TRY(vm.argument(0).to_index(vm));

    // 2. Set bigint to ? ToBigInt(bigint).
    auto bigint = TRY(vm.argument(1).to_bigint(vm));

    // 3. Return the BigInt value that represents ℝ(bigint) modulo 2^bits.
    auto const mod = TRY_OR_THROW_OOM(vm, bigint->big_integer().mod_power_of_two(bits));

    return BigInt::create(vm, mod);
}

}
