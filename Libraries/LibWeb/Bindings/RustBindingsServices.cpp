/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/HostObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibURL/RustIntegration.h>
#include <LibWeb/Bindings/DOMException.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
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

RustInterfaceCell* rust_interface_cell_of_wrapper(JS::Object& object, JSHostClass const& wrapper_host_class)
{
    auto* wrapper = as_if<JS::HostObject>(object);
    if (!wrapper || &wrapper->host_class() != &wrapper_host_class)
        return nullptr;
    return GC::static_cell_cast<RustInterfaceCell>(JS::host_data_of(object).ptr());
}

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

// A value of the type that the record of a structured deserialization holds next.
template<typename T>
static JSCompletion deserialize_plain_value(RustSerializationReader* reader, T* out_value)
{
    auto value = HTML::decode_or_throw_data_clone_error<T>(reader->realm, reader->serialized);
    if (value.is_exception())
        return completion_to_abi(reader->realm, value.exception());
    *out_value = value.release_value();
    return completion_to_abi(reader->realm, JS::js_undefined());
}

extern "C" {

void* web_bindings_wrapper_world(void* realm, bool* is_main_world);
JSObject* web_bindings_rust_wrapper_for(void* realm, void* cell);
void web_bindings_set_rust_wrapper(void* realm, void* cell, JSObject* wrapper);
JSObject* web_bindings_rejected_promise(void* current_realm, JSValue this_value, JSValue reason);
bool web_bindings_realm_is_window(void* realm);
void web_bindings_with_temporary_execution_context(void* realm, void* context, void (*steps)(void*));
bool web_bindings_object_implements_interface(JSObject* object, u16 interface_name);
JSObject* web_bindings_create_dom_exception(void* realm, ReadonlyBytes name, FlatPtr message);
bool web_bindings_extract_an_origin(JSObject* object, void* ctx, void (*on_origin)(void*, URL::FFI::FfiOrigin const*));
JSCompletion web_bindings_serialize_value(RustSerializationWriter*, JSValue);
void web_bindings_serialize_u32(RustSerializationWriter*, u32);
void web_bindings_serialize_f64(RustSerializationWriter*, double);
void web_bindings_serialize_bool(RustSerializationWriter*, bool);
void web_bindings_serialize_i64(RustSerializationWriter*, i64);
void web_bindings_serialize_utf16_string(RustSerializationWriter*, FlatPtr string);
void web_bindings_serialize_bytes(RustSerializationWriter*, ReadonlyBytes);
JSCompletion web_bindings_deserialize_value(RustSerializationReader*);
JSCompletion web_bindings_deserialize_u32(RustSerializationReader*, u32* out_value);
JSCompletion web_bindings_deserialize_f64(RustSerializationReader*, double* out_value);
JSCompletion web_bindings_deserialize_bool(RustSerializationReader*, bool* out_value);
JSCompletion web_bindings_deserialize_i64(RustSerializationReader*, i64* out_value);
JSCompletion web_bindings_deserialize_utf16_string(RustSerializationReader*, FlatPtr* out_string);
void web_bindings_serialize_inline_object_header(RustSerializationWriter*, u16 interface_name);
JSCompletion web_bindings_deserialize_inline_object_header(RustSerializationReader*, u16 interface_name);

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

bool web_bindings_realm_is_window(void* realm)
{
    return HTML::window_from_global_object(static_cast<JS::Realm*>(realm)->global_object());
}

void web_bindings_with_temporary_execution_context(void* realm, void* context, void (*steps)(void*))
{
    HTML::TemporaryExecutionContext execution_context { *static_cast<JS::Realm*>(realm) };
    steps(context);
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

// https://html.spec.whatwg.org/multipage/browsers.html#extract-an-origin
bool web_bindings_extract_an_origin(JSObject* object, void* ctx, void (*on_origin)(void*, URL::FFI::FfiOrigin const*))
{
    auto* js_object = JS::HostABI::object_from_abi(object);
    Optional<URL::Origin> origin;

    // NB: IDL only ever sees HTML::WindowProxy but we want to use HTML::Window.
    if (auto* window_proxy = HTML::WindowProxy::from_object(*js_object)) {
        if (auto window = window_proxy->window())
            origin = window->extract_an_origin();
    } else if (auto const* wrappable = wrappable_impl_from(js_object)) {
        origin = wrappable->extract_an_origin();
    }

    if (!origin.has_value())
        return false;
    URL::RustIntegration::lend_origin_to_ffi(*origin, [&](URL::FFI::FfiOrigin const& ffi_origin) {
        on_origin(ctx, &ffi_origin);
    });
    return true;
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

void web_bindings_serialize_f64(RustSerializationWriter* writer, double value)
{
    writer->serialized.encode(value);
}

void web_bindings_serialize_bool(RustSerializationWriter* writer, bool value)
{
    writer->serialized.encode(value);
}

void web_bindings_serialize_i64(RustSerializationWriter* writer, i64 value)
{
    writer->serialized.encode(value);
}

void web_bindings_serialize_utf16_string(RustSerializationWriter* writer, FlatPtr string)
{
    writer->serialized.encode(Utf16String::from_raw(string));
}

void web_bindings_serialize_bytes(RustSerializationWriter* writer, ReadonlyBytes bytes)
{
    writer->serialized.encode(bytes);
}

// StructuredDeserialize of a value that the serialized form of an object holds, https://html.spec.whatwg.org/multipage/structured-data.html#sub-deserialization
JSCompletion web_bindings_deserialize_value(RustSerializationReader* reader)
{
    return completion_to_abi(reader->realm, HTML::structured_deserialize_internal(reader->realm->vm(), reader->serialized, reader->realm, reader->memory));
}

JSCompletion web_bindings_deserialize_u32(RustSerializationReader* reader, u32* out_value)
{
    return deserialize_plain_value(reader, out_value);
}

JSCompletion web_bindings_deserialize_f64(RustSerializationReader* reader, double* out_value)
{
    return deserialize_plain_value(reader, out_value);
}

JSCompletion web_bindings_deserialize_bool(RustSerializationReader* reader, bool* out_value)
{
    return deserialize_plain_value(reader, out_value);
}

JSCompletion web_bindings_deserialize_i64(RustSerializationReader* reader, i64* out_value)
{
    return deserialize_plain_value(reader, out_value);
}

JSCompletion web_bindings_deserialize_utf16_string(RustSerializationReader* reader, FlatPtr* out_string)
{
    Utf16String string;
    auto result = deserialize_plain_value(reader, &string);
    *out_string = move(string).into_raw();
    return result;
}

// The record of a serializable object of the interface as StructuredSerializeInternal starts it, for an object that
// steps serialize in place, without a position in the memory of the serialization.
void web_bindings_serialize_inline_object_header(RustSerializationWriter* writer, u16 interface_name)
{
    writer->serialized.encode(to_underlying(HTML::ValueTag::SerializableObject));
    writer->serialized.encode(static_cast<InterfaceName>(interface_name));
    if (writer->serialized.type() == HTML::SerializationType::Storage)
        writer->serialized.encode(HTML::serializable_payload_version);
}

// Reads what web_bindings_serialize_inline_object_header() writes, and throws a "DataCloneError" DOMException if it is
// not the start of the record of a serializable object of the interface.
JSCompletion web_bindings_deserialize_inline_object_header(RustSerializationReader* reader, u16 interface_name)
{
    auto result = [&]() -> WebIDL::ExceptionOr<JS::Value> {
        auto tag = TRY(HTML::decode_or_throw_data_clone_error<u8>(reader->realm, reader->serialized));
        auto name = TRY(HTML::decode_or_throw_data_clone_error<InterfaceName>(reader->realm, reader->serialized));
        if (tag != to_underlying(HTML::ValueTag::SerializableObject) || name != static_cast<InterfaceName>(interface_name))
            return WebIDL::DataCloneError::create(reader->realm, "Unexpected serializable object"_utf16);
        if (reader->serialized.type() == HTML::SerializationType::Storage) {
            auto version = TRY(HTML::decode_or_throw_data_clone_error<u64>(reader->realm, reader->serialized));
            if (version != HTML::serializable_payload_version)
                return WebIDL::DataCloneError::create(reader->realm, "Unsupported serializable payload version"_utf16);
        }
        return JS::js_undefined();
    }();
    return completion_to_abi(reader->realm, move(result));
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
