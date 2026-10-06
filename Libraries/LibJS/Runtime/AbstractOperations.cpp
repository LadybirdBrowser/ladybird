/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ObjectEnvironment.h>

namespace JS {

using namespace EmbeddingABI;

static_assert(sizeof(Value) == sizeof(JSValue));
static_assert(alignof(Value) == alignof(JSValue));

// The runtime reads the arguments where they are, and the caller keeps them reachable until the call returns.
static JSValue const* values_to_abi(ReadonlySpan<Value> values)
{
    return reinterpret_cast<JSValue const*>(values.data());
}

// 9.1.2.3 NewObjectEnvironment ( O, W, E ), https://tc39.es/ecma262/#sec-newobjectenvironment
GC::Ref<ObjectEnvironment> new_object_environment(Object& object, bool is_with_environment, GC::Ptr<Environment> environment)
{
    auto* outer_environment = environment ? cell_to_abi<JSEnvironment>(*environment) : nullptr;
    return cell_ref_from_abi<ObjectEnvironment>(js_environment_new_object_environment(vm_to_abi(object.vm()), object_to_abi(object), is_with_environment, outer_environment));
}

// 9.13 CanBeHeldWeakly ( v ), https://tc39.es/ecma262/#sec-canbeheldweakly
bool can_be_held_weakly(Value value)
{
    return js_value_can_be_held_weakly(value_to_abi(value));
}

// 7.3.14 Call ( F, V [ , argumentsList ] ), https://tc39.es/ecma262/#sec-call
ThrowCompletionOr<Value> call_impl(VM& vm, Value function, Value this_value, ReadonlySpan<Value> arguments_list)
{
    return completion_from_abi<Value>(js_function_call(vm_to_abi(vm), value_to_abi(function), value_to_abi(this_value), values_to_abi(arguments_list), arguments_list.size()));
}

// 7.3.14 Call ( F, V [ , argumentsList ] ), https://tc39.es/ecma262/#sec-call
ThrowCompletionOr<Value> call_impl(VM& vm, FunctionObject& function, Value this_value, ReadonlySpan<Value> arguments_list)
{
    return completion_from_abi<Value>(js_function_call(vm_to_abi(vm), value_to_abi(&function), value_to_abi(this_value), values_to_abi(arguments_list), arguments_list.size()));
}

// 7.3.15 Construct ( F [ , argumentsList [ , newTarget ] ] ), https://tc39.es/ecma262/#sec-construct
ThrowCompletionOr<GC::Ref<Object>> construct_impl(VM& vm, FunctionObject& function, ReadonlySpan<Value> arguments_list, FunctionObject* new_target)
{
    return completion_from_abi<GC::Ref<Object>>(js_function_construct(vm_to_abi(vm), object_to_abi(function), values_to_abi(arguments_list), arguments_list.size(), new_target ? object_to_abi(*new_target) : nullptr));
}

// 7.3.18 LengthOfArrayLike ( obj ), https://tc39.es/ecma262/#sec-lengthofarraylike
ThrowCompletionOr<size_t> length_of_array_like(VM& vm, Object const& object)
{
    u64 length = 0;
    TRY(completion_from_abi<void>(js_array_length_of_array_like(vm_to_abi(vm), object_to_abi(object), &length)));
    return length;
}

// 7.3.24 GetFunctionRealm ( obj ), https://tc39.es/ecma262/#sec-getfunctionrealm
ThrowCompletionOr<Realm*> get_function_realm(VM& vm, FunctionObject const& function)
{
    auto realm = TRY(completion_from_abi<GC::Ref<Realm>>(js_realm_get_function_realm(vm_to_abi(vm), object_to_abi(function))));
    return realm.ptr();
}

}
