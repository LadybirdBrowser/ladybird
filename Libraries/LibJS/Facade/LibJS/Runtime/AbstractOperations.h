/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Concepts.h>
#include <AK/Forward.h>
#include <AK/Span.h>
#include <LibCrypto/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Export.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/ErrorTypes.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/ModuleRequest.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>
#include <math.h>

namespace JS {

JS_API GC::Ref<ObjectEnvironment> new_object_environment(Object&, bool is_with_environment, GC::Ptr<Environment>);
JS_API bool can_be_held_weakly(Value);
JS_API ThrowCompletionOr<Value> call_impl(VM&, Value function, Value this_value, ReadonlySpan<Value> arguments = {});
JS_API ThrowCompletionOr<Value> call_impl(VM&, FunctionObject& function, Value this_value, ReadonlySpan<Value> arguments = {});
JS_API ThrowCompletionOr<GC::Ref<Object>> construct_impl(VM&, FunctionObject&, ReadonlySpan<Value> arguments = {}, FunctionObject* new_target = nullptr);
JS_API ThrowCompletionOr<size_t> length_of_array_like(VM&, Object const&);
JS_API ThrowCompletionOr<Realm*> get_function_realm(VM&, FunctionObject const&);

// 7.3.14 Call ( F, V [ , argumentsList ] ), https://tc39.es/ecma262/#sec-call
ALWAYS_INLINE ThrowCompletionOr<Value> call(VM& vm, Value function, Value this_value, ReadonlySpan<Value> arguments_list)
{
    return call_impl(vm, function, this_value, arguments_list);
}

ALWAYS_INLINE ThrowCompletionOr<Value> call(VM& vm, Value function, Value this_value, Span<Value> arguments_list)
{
    return call_impl(vm, function, this_value, static_cast<ReadonlySpan<Value>>(arguments_list));
}

template<typename... Args>
ALWAYS_INLINE ThrowCompletionOr<Value> call(VM& vm, Value function, Value this_value, Args&&... args)
{
    constexpr auto argument_count = sizeof...(Args);
    if constexpr (argument_count > 0) {
        AK::Array<Value, argument_count> arguments { forward<Args>(args)... };
        return call_impl(vm, function, this_value, static_cast<ReadonlySpan<Value>>(arguments.span()));
    }

    return call_impl(vm, function, this_value);
}

ALWAYS_INLINE ThrowCompletionOr<Value> call(VM& vm, FunctionObject& function, Value this_value, ReadonlySpan<Value> arguments_list)
{
    return call_impl(vm, function, this_value, arguments_list);
}

ALWAYS_INLINE ThrowCompletionOr<Value> call(VM& vm, FunctionObject& function, Value this_value, Span<Value> arguments_list)
{
    return call_impl(vm, function, this_value, static_cast<ReadonlySpan<Value>>(arguments_list));
}

template<typename... Args>
ALWAYS_INLINE ThrowCompletionOr<Value> call(VM& vm, FunctionObject& function, Value this_value, Args&&... args)
{
    constexpr auto argument_count = sizeof...(Args);
    if constexpr (argument_count > 0) {
        AK::Array<Value, argument_count> arguments { forward<Args>(args)... };
        return call_impl(vm, function, this_value, static_cast<ReadonlySpan<Value>>(arguments.span()));
    }

    return call_impl(vm, function, this_value);
}

// 7.3.15 Construct ( F [ , argumentsList [ , newTarget ] ] ), https://tc39.es/ecma262/#sec-construct
template<typename... Args>
ALWAYS_INLINE ThrowCompletionOr<GC::Ref<Object>> construct(VM& vm, FunctionObject& function, Args&&... args)
{
    constexpr auto argument_count = sizeof...(Args);
    if constexpr (argument_count > 0) {
        AK::Array<Value, argument_count> arguments { forward<Args>(args)... };
        return construct_impl(vm, function, static_cast<ReadonlySpan<Value>>(arguments.span()));
    }

    return construct_impl(vm, function);
}

ALWAYS_INLINE ThrowCompletionOr<GC::Ref<Object>> construct(VM& vm, FunctionObject& function, ReadonlySpan<Value> arguments_list, FunctionObject* new_target = nullptr)
{
    return construct_impl(vm, function, arguments_list, new_target);
}

ALWAYS_INLINE ThrowCompletionOr<GC::Ref<Object>> construct(VM& vm, FunctionObject& function, Span<Value> arguments_list, FunctionObject* new_target = nullptr)
{
    return construct_impl(vm, function, static_cast<ReadonlySpan<Value>>(arguments_list), new_target);
}

// x modulo y, https://tc39.es/ecma262/#eqn-modulo
template<Arithmetic T, Arithmetic U>
auto modulo(T x, U y)
{
    // The notation “x modulo y” (y must be finite and non-zero) computes a value k of the same sign as y (or zero) such that abs(k) < abs(y) and x - k = q × y for some integer q.
    VERIFY(y != 0);
    if constexpr (IsFloatingPoint<T> || IsFloatingPoint<U>) {
        if constexpr (IsFloatingPoint<U>)
            VERIFY(isfinite(y));
        auto r = fmod(x, y);
        return r < 0 ? r + y : r;
    } else {
        return ((x % y) + y) % y;
    }
}

auto modulo(Crypto::BigInteger auto const& x, Crypto::BigInteger auto const& y)
{
    VERIFY(!y.is_zero());
    auto result = x.remainder(y);
    if (result.is_negative())
        result = result.plus(y);
    return result;
}

}
