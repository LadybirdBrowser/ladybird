/*
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibJS/ObjectEmbeddingABIConversions.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

static_assert(sizeof(DirectGetterConfiguration) == sizeof(JSDirectGetterConfiguration));
static_assert(offsetof(DirectGetterConfiguration, wrapper_implementation_offset) == offsetof(JSDirectGetterConfiguration, wrapper_implementation_offset));
static_assert(offsetof(DirectGetterConfiguration, implementation_value_offset) == offsetof(JSDirectGetterConfiguration, implementation_value_offset));
static_assert(offsetof(DirectGetterConfiguration, main_world_wrapper_offset) == offsetof(JSDirectGetterConfiguration, main_world_wrapper_offset));
static_assert(offsetof(DirectGetterConfiguration, weak_impl_value_offset) == offsetof(JSDirectGetterConfiguration, weak_impl_value_offset));

// A closure's behaviour lives in a cell of the embedder's heap, which the function keeps alive, and whose captures the
// garbage collector scans conservatively.
using NativeFunctionBehaviour = GC::Function<ThrowCompletionOr<Value>(VM&)>;

static void* behaviour_cell_to_abi(VM& vm, Function<ThrowCompletionOr<Value>(VM&)> behaviour)
{
    VERIFY(behaviour);
    auto cell = NativeFunctionBehaviour::create(vm.heap(), move(behaviour));
    return static_cast<GC::Cell*>(cell.ptr());
}

// The runtime's VM pointer is the address of the embedder's JS::VM, which holds the runtime's VM in its first bytes.
static JSCompletion call_behaviour_cell(void* behaviour_cell, JSVM* vm)
{
    auto& behaviour = static_cast<NativeFunctionBehaviour&>(*static_cast<GC::Cell*>(behaviour_cell));
    return completion_to_abi<Value>(behaviour.function()(*reinterpret_cast<VM*>(vm)));
}

static NativeFunction& native_function_from_abi(JSObject* function)
{
    return static_cast<NativeFunction&>(object_from_abi(function));
}

// 10.3.4 CreateBuiltinFunction ( behaviour, length, name, additionalInternalSlotsList [ , realm [ , prototype [ , prefix ] ] ] ), https://tc39.es/ecma262/#sec-createbuiltinfunction
GC::Ref<NativeFunction> NativeFunction::create(Realm& allocating_realm, Function<ThrowCompletionOr<Value>(VM&)> behaviour, i32 length, PropertyKey const& name, Optional<GC::Ptr<Realm>> realm, Optional<StringView> const& prefix)
{
    auto& vm = allocating_realm.vm();
    PrefixForABI abi_prefix { prefix };
    auto* behaviour_cell = behaviour_cell_to_abi(vm, move(behaviour));
    return native_function_from_abi(js_function_create_closure(vm_to_abi(vm), call_behaviour_cell, behaviour_cell, length, property_key_to_abi(name), builtin_function_realm_to_abi(realm), abi_prefix.characters, abi_prefix.length));
}

GC::Ref<NativeFunction> NativeFunction::create(Realm& allocating_realm, NativeFunctionPointer behaviour, i32 length, PropertyKey const& name, Optional<GC::Ptr<Realm>> realm, Optional<StringView> const& prefix)
{
    auto& vm = allocating_realm.vm();
    PrefixForABI abi_prefix { prefix };
    return native_function_from_abi(js_function_create_native(vm_to_abi(vm), native_function_to_abi(behaviour), length, property_key_to_abi(name), builtin_function_realm_to_abi(realm), abi_prefix.characters, abi_prefix.length));
}

GC::Ref<NativeFunction> NativeFunction::create(Realm& realm, Utf16FlyString const& name, Function<ThrowCompletionOr<Value>(VM&)> behaviour)
{
    auto& vm = realm.vm();
    auto* behaviour_cell = behaviour_cell_to_abi(vm, move(behaviour));
    return native_function_from_abi(js_function_create_closure_with_name(vm_to_abi(vm), cell_to_abi<JSRealm>(realm), utf16_view_to_abi(name.view()), call_behaviour_cell, behaviour_cell));
}

GC::Ref<DirectGetterFunction> DirectGetterFunction::create(Realm& realm, NativeFunctionPointer behaviour, i32 length, PropertyKey const& name, DirectGetterConfiguration configuration, Optional<StringView> const& prefix)
{
    JSDirectGetterConfiguration abi_configuration {
        .wrapper_implementation_offset = configuration.wrapper_implementation_offset,
        .implementation_value_offset = configuration.implementation_value_offset,
        .main_world_wrapper_offset = configuration.main_world_wrapper_offset,
        .weak_impl_value_offset = configuration.weak_impl_value_offset,
    };
    PrefixForABI abi_prefix { prefix };
    auto* function = js_function_create_direct_getter(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), native_function_to_abi(behaviour), length, property_key_to_abi(name), &abi_configuration, abi_prefix.characters, abi_prefix.length);
    return static_cast<DirectGetterFunction&>(object_from_abi(function));
}

}
