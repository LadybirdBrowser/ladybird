/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NumberConstructor.h>
#include <LibJS/Runtime/NumberObject.h>
#include <LibJS/Runtime/ValueInlines.h>

#if defined(AK_COMPILER_CLANG)
#    define EPSILON_VALUE AK::exp2(-52.)
#    define MAX_SAFE_INTEGER_VALUE AK::exp2(53.) - 1
#    define MIN_SAFE_INTEGER_VALUE -(AK::exp2(53.) - 1)
#else
constexpr double const EPSILON_VALUE { __builtin_exp2(-52) };
constexpr double const MAX_SAFE_INTEGER_VALUE { __builtin_exp2(53) - 1 };
constexpr double const MIN_SAFE_INTEGER_VALUE { -(__builtin_exp2(53) - 1) };
#endif

namespace JS {

JS_DEFINE_ALLOCATOR(NumberConstructor);

NumberConstructor::NumberConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Number.as_string(), realm.intrinsics().function_prototype())
{
}

void NumberConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 21.1.2.15 Number.prototype, https://tc39.es/ecma262/#sec-number.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().number_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.isFinite, is_finite, 1, attr);
    define_native_function(realm, vm.names.isInteger, is_integer, 1, attr);
    define_native_function(realm, vm.names.isNaN, is_nan, 1, attr);
    define_native_function(realm, vm.names.isSafeInteger, is_safe_integer, 1, attr);
    define_direct_property(vm.names.parseInt, realm.intrinsics().parse_int_function(), attr);
    define_direct_property(vm.names.parseFloat, realm.intrinsics().parse_float_function(), attr);
    define_direct_property(vm.names.EPSILON, Value(EPSILON_VALUE), 0);
    define_direct_property(vm.names.MAX_VALUE, Value(NumericLimits<double>::max()), 0);
    define_direct_property(vm.names.MIN_VALUE, Value(NumericLimits<double>::min_denormal()), 0);
    define_direct_property(vm.names.MAX_SAFE_INTEGER, Value(MAX_SAFE_INTEGER_VALUE), 0);
    define_direct_property(vm.names.MIN_SAFE_INTEGER, Value(MIN_SAFE_INTEGER_VALUE), 0);
    define_direct_property(vm.names.NEGATIVE_INFINITY, js_negative_infinity(), 0);
    define_direct_property(vm.names.POSITIVE_INFINITY, js_infinity(), 0);
    define_direct_property(vm.names.NaN, js_nan(), 0);

    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
}

// Most of 21.1.1.1 Number ( value ) factored into a separate function for sharing between call() and construct().
static ThrowCompletionOr<Value> get_value_from_constructor_argument(VM& vm)
{
    Value number;
    // 1. If value is present, then
    if (vm.argument_count() > 0) {
        // a. Let prim be ? ToNumeric(value).
        auto primitive = TRY(vm.argument(0).to_numeric(vm));

        // b. If Type(prim) is BigInt, let n be 𝔽(ℝ(prim)).
        if (primitive.is_bigint()) {
            number = Value(primitive.as_bigint().big_integer().to_double(Crypto::UnsignedBigInteger::RoundingMode::ECMAScriptNumberValueFor));
        }
        // c. Otherwise, let n be prim.
        else {
            number = primitive;
        }
    }
    // 2. Else,
    else {
        // a. Let n be +0𝔽.
        number = Value(0);
    }
    return number;
}

// 21.1.1.1 Number ( value ), https://tc39.es/ecma262/#sec-number-constructor-number-value
ThrowCompletionOr<Value> NumberConstructor::call()
{
    // NOTE: get_value_from_constructor_argument performs steps 1 and 2 and returns n.
    // 3. If NewTarget is undefined, return n.
    return get_value_from_constructor_argument(vm());
}

// 21.1.1.1 Number ( value ), https://tc39.es/ecma262/#sec-number-constructor-number-value
ThrowCompletionOr<NonnullGCPtr<Object>> NumberConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();
    // NOTE: get_value_from_constructor_argument performs steps 1 and 2 and returns n.
    auto number = TRY(get_value_from_constructor_argument(vm));

    // 4. Let O be ? OrdinaryCreateFromConstructor(NewTarget, "%Number.prototype%", « [[NumberData]] »).
    // 5. Set O.[[NumberData]] to n.
    // 6. Return O.
    return TRY(ordinary_create_from_constructor<NumberObject>(vm, new_target, &Intrinsics::number_prototype, number.as_double()));
}

// 21.1.2.2 Number.isFinite ( number ), https://tc39.es/ecma262/#sec-number.isfinite
JS_DEFINE_NATIVE_FUNCTION(NumberConstructor::is_finite)
{
    auto number = vm.argument(0);

    // 1. If number is not a Number, return false.
    // 2. If number is not finite, return false.
    // 3. Otherwise, return true.
    return Value(number.is_finite_number());
}

// 21.1.2.3 Number.isInteger ( number ), https://tc39.es/ecma262/#sec-number.isinteger
JS_DEFINE_NATIVE_FUNCTION(NumberConstructor::is_integer)
{
    auto number = vm.argument(0);

    // 1. Return IsIntegralNumber(number).
    return Value(number.is_integral_number());
}

// 21.1.2.4 Number.isNaN ( number ), https://tc39.es/ecma262/#sec-number.isnan
JS_DEFINE_NATIVE_FUNCTION(NumberConstructor::is_nan)
{
    auto number = vm.argument(0);

    // 1. If number is not a Number, return false.
    // 2. If number is NaN, return true.
    // 3. Otherwise, return false.
    return Value(number.is_nan());
}

// 21.1.2.5 Number.isSafeInteger ( number ), https://tc39.es/ecma262/#sec-number.issafeinteger
JS_DEFINE_NATIVE_FUNCTION(NumberConstructor::is_safe_integer)
{
    auto number = vm.argument(0);

    // 1. If IsIntegralNumber(number) is true, then
    if (number.is_integral_number()) {
        // a. If abs(ℝ(number)) ≤ 2^53 - 1, return true.
        if (fabs(number.as_double()) <= MAX_SAFE_INTEGER_VALUE)
            return Value(true);
    }

    // 2. Return false.
    return Value(false);
}

}
