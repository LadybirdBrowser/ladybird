/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/HostModule.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/ContentSecurityPolicy/BlockingAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebAssembly/WebAssembly.h>
#include <LibWeb/WebIDL/Promise.h>

extern "C" {

// The entry points of the WebAssembly JavaScript Interface, in Libraries/LibWeb/Bindings/Rust/src/web_assembly.
void* web_rust_web_assembly_create_compile_error(JSVM*, void* realm, FlatPtr message);
JSCompletion web_rust_web_assembly_host_resize_array_buffer(JSVM*, JSObject* buffer, size_t new_length, bool* handled);
JSCompletion web_rust_web_assembly_host_grow_shared_array_buffer(JSVM*, JSObject* buffer, size_t new_length, bool* handled);
JSCompletion web_rust_web_assembly_parse_a_webassembly_module(JSVM*, void* realm, u8 const* bytes, size_t length, u8 const* filename, size_t filename_length, void* host_defined, JSModule** module_record);

// What the WebAssembly JavaScript Interface asks of LibWeb, declared on the Rust side in
// Libraries/LibWeb/Bindings/Rust/src/web_assembly.
JSCompletion web_bindings_ensure_csp_does_not_block_wasm_byte_compilation(void* realm);
JSCompletion web_bindings_consume_webassembly_response(void* realm, JSValue unwrapped_source, void** body_promise);
bool web_bindings_current_agent_cluster(u64* agent_cluster);
bool web_bindings_agent_cluster_of_realm(void* realm, u64* agent_cluster);
}

namespace Web::WebAssembly {

using namespace JS::HostABI;

GC::Ref<JS::Object> create_compile_error(JS::Realm& realm, Utf16String const& message)
{
    auto* error = web_rust_web_assembly_create_compile_error(vm_to_abi(realm.vm()), &realm, message.raw_identity());
    return *object_from_abi(static_cast<JSObject*>(error));
}

static JS::ThrowCompletionOr<JS::HandledByHost> handled_by_host(JSCompletion completion, bool handled)
{
    TRY(completion_from_abi<void>(completion));
    return handled ? JS::HandledByHost::Handled : JS::HandledByHost::Unhandled;
}

JS::ThrowCompletionOr<JS::HandledByHost> host_resize_array_buffer(JS::VM& vm, JS::ArrayBuffer& buffer, size_t new_length)
{
    bool handled = false;
    auto completion = web_rust_web_assembly_host_resize_array_buffer(vm_to_abi(vm), object_to_abi(&buffer), new_length, &handled);
    return handled_by_host(completion, handled);
}

JS::ThrowCompletionOr<JS::HandledByHost> host_grow_shared_array_buffer(JS::VM& vm, JS::ArrayBuffer& buffer, size_t new_length)
{
    bool handled = false;
    auto completion = web_rust_web_assembly_host_grow_shared_array_buffer(vm_to_abi(vm), object_to_abi(&buffer), new_length, &handled);
    return handled_by_host(completion, handled);
}

JS::ThrowCompletionOr<GC::Ref<JS::HostModule>> parse_a_webassembly_module(ReadonlyBytes bytes, JS::Realm& realm, StringView filename, GC::Ptr<GC::Cell> host_defined)
{
    JSModule* module_record = nullptr;
    auto completion = web_rust_web_assembly_parse_a_webassembly_module(vm_to_abi(realm.vm()), &realm, bytes.data(), bytes.size(),
        reinterpret_cast<u8 const*>(filename.characters_without_null_termination()), filename.length(), host_defined.ptr(), &module_record);
    TRY(completion_from_abi<void>(completion));
    return static_cast<JS::HostModule&>(*module_from_abi(module_record));
}

}

extern "C" {

JSCompletion web_bindings_ensure_csp_does_not_block_wasm_byte_compilation(void* realm)
{
    return JS::HostABI::completion_to_abi(Web::ContentSecurityPolicy::ensure_csp_does_not_block_wasm_byte_compilation(*static_cast<JS::Realm*>(realm)));
}

static bool write_agent_cluster(Optional<u64> agent_cluster_id, u64* agent_cluster)
{
    if (!agent_cluster_id.has_value())
        return false;
    *agent_cluster = *agent_cluster_id;
    return true;
}

// The current Realm's corresponding agent cluster.
bool web_bindings_current_agent_cluster(u64* agent_cluster)
{
    return write_agent_cluster(Web::HTML::current_settings_object().agent_cluster_id(), agent_cluster);
}

// The realm's corresponding agent cluster.
bool web_bindings_agent_cluster_of_realm(void* realm, u64* agent_cluster)
{
    auto& settings = Web::Bindings::principal_host_defined_environment_settings_object(*static_cast<JS::Realm*>(realm));
    return write_agent_cluster(settings.agent_cluster_id(), agent_cluster);
}

// https://webassembly.github.io/spec/web-api/index.html#compile-a-potential-webassembly-response
JSCompletion web_bindings_consume_webassembly_response(void* realm_pointer, JSValue unwrapped_source_value, void** body_promise)
{
    using namespace Web;
    auto& realm = *static_cast<JS::Realm*>(realm_pointer);
    auto& vm = realm.vm();
    auto unwrapped_source = JS::HostABI::value_from_abi(unwrapped_source_value);
    auto type_error = [&](Utf16String message) {
        return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<void> { vm.throw_completion<JS::TypeError>(move(message)) });
    };

    // 1. Let response be unwrappedSource’s response.
    auto* response_object = unwrapped_source.is_object()
        ? Bindings::impl_from<Fetch::Response>(&unwrapped_source.as_object())
        : nullptr;
    if (!response_object)
        return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<void> { vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Response") });
    auto response = response_object->response();

    // 2. Let mimeType be the result of getting `Content-Type` from response’s header list.
    // 3. If mimeType is null, reject returnValue with a TypeError and abort these substeps.
    // 4. Remove all HTTP tab or space byte from the start and end of mimeType.
    // 5. If mimeType is not a byte-case-insensitive match for `application/wasm`, reject returnValue with a TypeError and abort these substeps.
    // Note: extra parameters are not allowed, including the empty `application/wasm;`.
    // FIXME: Validate these extra constraints that are not checked by extract_mime_type()
    if (auto mime = Fetch::Infrastructure::extract_mime_type(response->header_list()); !mime.has_value() || mime.value().essence() != "application/wasm"sv)
        return type_error("Response does not match the application/wasm MIME type"_utf16);

    // 6. If response is not CORS-same-origin, reject returnValue with a TypeError and abort these substeps.
    if (!response->is_cors_same_origin())
        return type_error("Response is not CORS-same-origin"_utf16);

    // 7. If response’s status is not an ok status, reject returnValue with a TypeError and abort these substeps.
    if (!response_object->ok())
        return type_error("Response does not represent an ok status"_utf16);

    // 8. Consume response’s body as an ArrayBuffer, and let bodyPromise be the result.
    auto promise = WebIDL::create_promise(realm);
    response_object->array_buffer(promise);
    *body_promise = promise.ptr();
    return JS::HostABI::completion_to_abi(JS::ThrowCompletionOr<void> {});
}
}
