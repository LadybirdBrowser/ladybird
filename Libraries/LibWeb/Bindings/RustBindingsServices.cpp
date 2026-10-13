/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/WebIDL/Promise.h>

// What the Rust bindings of the interfaces implemented in Rust ask of LibWeb's C++ bindings, declared on the Rust
// side in Libraries/LibWeb/Bindings/Rust/src/bindings/services.rs.

namespace Web::Bindings {

constexpr JSHostClass web_bindings_rust_interface_wrapper_host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "RustInterface"sv,
    nullptr, nullptr, nullptr, JS_HOST_CLASS_IS_PLATFORM_OBJECT);

extern "C" {

void* web_bindings_wrapper_world(void* realm, bool* is_main_world);
JSObject* web_bindings_rust_wrapper_for(void* realm, void* cell);
void web_bindings_set_rust_wrapper(void* realm, void* cell, JSObject* wrapper);
JSObject* web_bindings_rejected_promise(void* current_realm, JSValue this_value, JSValue reason);

void* web_bindings_wrapper_world(void* realm, bool* is_main_world)
{
    auto& wrapper_world = host_defined_wrapper_world(*static_cast<JS::Realm*>(realm));
    *is_main_world = wrapper_world.is_main_world();
    return &wrapper_world;
}

JSObject* web_bindings_rust_wrapper_for(void* realm, void* cell)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto wrapper = host_defined_wrapper_world(js_realm).rust_wrapper_for(*static_cast<RustInterfaceCell*>(cell), js_realm);
    return JS::HostABI::object_to_abi(wrapper.ptr());
}

void web_bindings_set_rust_wrapper(void* realm, void* cell, JSObject* wrapper)
{
    auto& host_object = as<JS::HostObject>(*JS::HostABI::object_from_abi(wrapper));
    host_defined_wrapper_world(*static_cast<JS::Realm*>(realm)).set_rust_wrapper(*static_cast<RustInterfaceCell*>(cell), host_object);
}

JSObject* web_bindings_rejected_promise(void* current_realm, JSValue this_value, JSValue reason)
{
    auto& realm = *static_cast<JS::Realm*>(current_realm);
    auto promise_realm_this_value = JS::HostABI::value_from_abi(this_value);
    if (promise_realm_this_value.is_nullish())
        promise_realm_this_value = &realm.global_object();
    auto& promise_realm = this_value_realm(realm, promise_realm_this_value);
    auto promise = WebIDL::create_rejected_promise(promise_realm, JS::HostABI::value_from_abi(reason));
    return JS::HostABI::object_to_abi(promise->promise().ptr());
}
}

}
