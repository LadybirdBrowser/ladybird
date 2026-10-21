/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/DOMException.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWeb/WebIDL/Promise.h>

// What the Rust bindings of the interfaces implemented in Rust ask of LibWeb's C++ bindings, declared on the Rust
// side in Libraries/LibWeb/Bindings/Rust/src/bindings/services.rs.

// The wrapper of an object of an interface implemented in Rust that the C++ bindings name, in the wrapper world of
// the realm, which the Rust bindings generated for those interfaces define.
extern "C" JSObject* web_rust_bindings_wrap(JSVM*, void* realm, u16 interface_name, void* cell);

namespace Web::Bindings {

GC::Ref<JS::HostObject> wrap_rust_interface_object(WrapperWorld& wrapper_world, JS::Realm& preferred_realm, InterfaceName interface_name, RustInterfaceCell& cell)
{
    // NB: The Rust bindings find the wrapper world of an object through the realm that asks for its wrapper.
    VERIFY(&wrapper_world == &host_defined_wrapper_world(preferred_realm));
    auto* wrapper = web_rust_bindings_wrap(JS::HostABI::vm_to_abi(preferred_realm.vm()), &preferred_realm, to_underlying(interface_name), &cell);
    return as<JS::HostObject>(*JS::HostABI::object_from_abi(wrapper));
}

constexpr JSHostClass web_bindings_rust_interface_wrapper_host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "RustInterface"sv,
    nullptr, nullptr, nullptr, JS_HOST_CLASS_IS_PLATFORM_OBJECT);

bool rust_interface_object_implements_interface(JS::Object const& object, StringView interface_name)
{
    if (!JS::is_host_instance_of(object, web_bindings_rust_interface_wrapper_host_class))
        return false;
    // NB: The class of the wrappers of an interface has the name of the interface, and the class of the wrappers of
    //     the interface that it inherits from as its parent.
    for (auto const* host_class = &as<JS::HostObject>(object).host_class(); host_class != &web_bindings_rust_interface_wrapper_host_class; host_class = host_class->parent) {
        if (StringView { host_class->name, host_class->name_length } == interface_name)
            return true;
    }
    return false;
}

JSCompletion completion_to_abi(JS::Realm& realm, WebIDL::ExceptionOr<JS::Value> result)
{
    if (result.is_exception())
        return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { WebIDL::exception_to_throw_completion(realm.vm(), realm, result.exception()) });
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<JS::Value> { result.release_value() });
}

extern "C" {

void* web_bindings_wrapper_world(void* realm, bool* is_main_world);
JSObject* web_bindings_rust_wrapper_for(void* realm, void* cell);
void web_bindings_set_rust_wrapper(void* realm, void* cell, JSObject* wrapper);
JSObject* web_bindings_rejected_promise(void* current_realm, JSValue this_value, JSValue reason);
bool web_bindings_object_implements_interface(JSObject* object, u16 interface_name);
JSObject* web_bindings_create_dom_exception(void* realm, ReadonlyBytes name, FlatPtr message);
JSCompletion web_bindings_serialize_value(RustSerializationWriter*, JSValue);
void web_bindings_serialize_u32(RustSerializationWriter*, u32);
JSCompletion web_bindings_deserialize_value(RustSerializationReader*);
JSCompletion web_bindings_deserialize_u32(RustSerializationReader*, u32* out_value);

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

bool web_bindings_object_implements_interface(JSObject* object, u16 interface_name)
{
    auto* wrappable = wrappable_impl_from(JS::HostABI::object_from_abi(object));
    return wrappable && wrappable->implements_interface(static_cast<InterfaceName>(interface_name));
}

JSObject* web_bindings_create_dom_exception(void* realm, ReadonlyBytes name, FlatPtr message)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto exception = WebIDL::DOMException::create(FlyString::from_utf8_without_validation(name), Utf16String::from_raw(message));
    return JS::HostABI::object_to_abi(wrap(host_defined_wrapper_world(js_realm), js_realm, exception).ptr());
}

// StructuredSerializeInternal of a value that the serialized form of an object of an interface implemented in Rust
// holds, https://html.spec.whatwg.org/multipage/structured-data.html#sub-serialization
JSCompletion web_bindings_serialize_value(RustSerializationWriter* writer, JSValue value)
{
    auto result = HTML::structured_serialize_internal(writer->realm->vm(), writer->serialized, JS::HostABI::value_from_abi(value), writer->for_storage, writer->memory);
    if (result.is_exception())
        return completion_to_abi(writer->realm, result.exception());
    return completion_to_abi(writer->realm, JS::js_undefined());
}

void web_bindings_serialize_u32(RustSerializationWriter* writer, u32 value)
{
    writer->serialized.encode(value);
}

// StructuredDeserialize of a value that the serialized form of an object holds, https://html.spec.whatwg.org/multipage/structured-data.html#sub-deserialization
JSCompletion web_bindings_deserialize_value(RustSerializationReader* reader)
{
    return completion_to_abi(reader->realm, HTML::structured_deserialize_internal(reader->realm->vm(), reader->serialized, reader->realm, reader->memory));
}

JSCompletion web_bindings_deserialize_u32(RustSerializationReader* reader, u32* out_value)
{
    auto value = HTML::decode_or_throw_data_clone_error<u32>(reader->realm, reader->serialized);
    if (value.is_exception())
        return completion_to_abi(reader->realm, value.exception());
    *out_value = value.release_value();
    return completion_to_abi(reader->realm, JS::js_undefined());
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
