/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Utf16View.h>
#include <LibJS/HostObjectABI.h>
#include <LibJS/Runtime/HostFunction.h>
#include <LibJS/Runtime/Object.h>

namespace Web::Bindings {

struct InterfaceObjectMetadata {
    using EnsurePrototypeFunction = JS::Object& (*)(JS::Realm&);
    using InitializeConstructorFunction = void (*)(JS::Realm&, JS::NativeFunction&);
    using InitializePrototypeFunction = void (*)(JS::Realm&, JS::Object&);
    using ConstructFunction = JS::ThrowCompletionOr<GC::Ref<JS::Object>> (*)(JS::HostFunction&, JS::FunctionObject&);

    StringView namespaced_name;
    Utf16View utf16_name;
    Utf16View utf16_namespaced_name;
    // The value of a legacy factory function's "prototype" property: the prototype of the interface it constructs.
    EnsurePrototypeFunction ensure_interface_prototype_object { nullptr };
    InitializeConstructorFunction initialize_constructor { nullptr };
    InitializePrototypeFunction initialize_prototype { nullptr };
    ConstructFunction construct { nullptr };
    i32 function_length { 0 };
    // The classes of the interface prototype object and of the interface object or legacy factory function, whose user
    // data is this metadata.
    JSHostClass prototype_host_class {};
    JSHostClass constructor_host_class {};
};

// The [[Call]] and [[Construct]] behavior of every interface object and legacy factory function.
extern JSHostFunctionHooks const interface_constructor_hooks;

}
