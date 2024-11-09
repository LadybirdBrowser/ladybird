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

JS_DEFINE_ALLOCATOR(BigIntConstructor);

static Crypto::SignedBigInteger const BIGINT_ONE { 1 };

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
ThrowCompletionOr<NonnullGCPtr<Object>> BigIntConstructor::construct(FunctionObject&)
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

    // 3. Let mod be ℝ(bigint) modulo 2^bits.
    // FIXME: For large values of `bits`, this can likely be improved with a SignedBigInteger API to
    //        drop the most significant bits.
    auto bits_shift_left = BIGINT_ONE.shift_left(bits);
    auto mod = modulo(bigint->big_integer(), bits_shift_left);

    // 4. If mod ≥ 2^(bits-1), return ℤ(mod - 2^bits); otherwise, return ℤ(mod).
    // NOTE: Some of the below conditionals are non-standard, but are to protect SignedBigInteger from
    //       allocating an absurd amount of memory if `bits - 1` overflows to NumericLimits<size_t>::max.
    if ((bits == 0) && (mod >= BIGINT_ONE))
        return BigInt::create(vm, mod.minus(bits_shift_left));
    if ((bits > 0) && (mod >= BIGINT_ONE.shift_left(bits - 1)))
        return BigInt::create(vm, mod.minus(bits_shift_left));

    return BigInt::create(vm, mod);
}

// 21.2.2.2 BigInt.asUintN ( bits, bigint ), https://tc39.es/ecma262/#sec-bigint.asuintn
JS_DEFINE_NATIVE_FUNCTION(BigIntConstructor::as_uint_n)
{
    // 1. Set bits to ? ToIndex(bits).
    auto bits = TRY(vm.argument(0).to_index(vm));

    // 2. Set bigint to ? ToBigInt(bigint).
    auto bigint = TRY(vm.argument(1).to_bigint(vm));

    // 3. Return the BigInt value that represents ℝ(bigint) modulo 2bits.
    // FIXME: For large values of `bits`, this can likely be improved with a SignedBigInteger API to
    //        drop the most significant bits.
    return BigInt::create(vm, modulo(bigint->big_integer(), BIGINT_ONE.shift_left(bits)));
}

}
