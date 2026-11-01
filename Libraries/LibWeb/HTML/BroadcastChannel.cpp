/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibURL/RustIntegration.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/HTML/BroadcastChannel.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>
#include <LibWebCommon/HTML/BroadcastChannelMessage.h>

// What the Rust implementation of BroadcastChannel asks of LibWeb's C++ code, declared on the Rust side in
// Libraries/LibWeb/Bindings/Rust/src/html/broadcast_channel.rs.

namespace Web::HTML {

extern "C" {

void web_html_broadcast_channel_relevant_settings_object_origin(void* realm, void* ctx, void (*on_origin)(void*, URL::FFI::FfiOrigin const*));
void web_html_broadcast_channel_set_strongly_referenced(void* realm, DOM::EventTarget*, bool strongly_referenced);
void web_html_broadcast_channel_post_message_to_other_processes(void* realm, FlatPtr channel_name, ReadonlyBytes serialized_message, int const* shared_memory, size_t const* shared_memory_sizes, size_t shared_memory_count);

// Defined on the Rust side.
void web_rust_broadcast_channel_deliver_message_locally(JSVM*, URL::FFI::FfiOrigin const* storage_key_origin, FlatPtr channel_name, URL::FFI::FfiOrigin const* source_origin, ReadonlyBytes serialized_message, int const* shared_memory, size_t const* shared_memory_sizes, size_t shared_memory_count);
}

static JS::Object& global_object_of(void* realm)
{
    return static_cast<JS::Realm*>(realm)->global_object();
}

void web_html_broadcast_channel_relevant_settings_object_origin(void* realm, void* ctx, void (*on_origin)(void*, URL::FFI::FfiOrigin const*))
{
    URL::RustIntegration::lend_origin_to_ffi(relevant_settings_object(global_object_of(realm)).origin(), [&](URL::FFI::FfiOrigin const& origin) {
        on_origin(ctx, &origin);
    });
}

void web_html_broadcast_channel_set_strongly_referenced(void* realm, DOM::EventTarget* event_target, bool strongly_referenced)
{
    auto& global_scope = relevant_window_or_worker_global_scope(global_object_of(realm));
    if (strongly_referenced)
        global_scope.add_strong_reference_to_broadcast_channel(*event_target);
    else
        global_scope.remove_strong_reference_to_broadcast_channel(*event_target);
}

void web_html_broadcast_channel_post_message_to_other_processes(void* realm, FlatPtr channel_name, ReadonlyBytes serialized_message, int const* shared_memory, size_t const* shared_memory_sizes, size_t shared_memory_count)
{
    auto& js_realm = *static_cast<JS::Realm*>(realm);
    auto name = Utf16String::from_raw(channel_name);

    Vector<Core::AnonymousBuffer> shared_buffers;
    shared_buffers.ensure_capacity(shared_memory_count);
    for (size_t i = 0; i < shared_memory_count; ++i)
        shared_buffers.unchecked_append(MUST(Core::AnonymousBuffer::create_from_anon_fd(shared_memory[i], shared_memory_sizes[i])));

    // NB: The UI process forwards the message to the other processes hosting an environment with its storage key, and
    //     each delivers it to its own BroadcastChannel objects.
    PostedBroadcastChannelMessage posted_message {
        .environment_id = principal_realm_settings_object(js_realm).id,
        .channel_name = Utf16FlyString::from_utf16(name.utf16_view()),
        .serialized_message = create_ipc_serialization_record(serialized_message, {}),
        .shared_buffers = move(shared_buffers),
    };
    Bindings::principal_host_defined_page(js_realm).client().page_did_post_broadcast_channel_message(posted_message);
}

void deliver_broadcast_channel_message_locally(BroadcastChannelMessage message)
{
    Vector<int> shared_memory;
    Vector<size_t> shared_memory_sizes;
    shared_memory.ensure_capacity(message.shared_buffers.size());
    shared_memory_sizes.ensure_capacity(message.shared_buffers.size());
    for (auto const& shared_buffer : message.shared_buffers) {
        shared_memory.unchecked_append(shared_buffer.fd());
        shared_memory_sizes.unchecked_append(shared_buffer.size());
    }

    auto channel_name = Utf16String::from_utf16(message.channel_name.view());
    URL::RustIntegration::lend_origin_to_ffi(message.storage_key.origin, [&](URL::FFI::FfiOrigin const& storage_key_origin) {
        URL::RustIntegration::lend_origin_to_ffi(message.source_origin, [&](URL::FFI::FfiOrigin const& source_origin) {
            web_rust_broadcast_channel_deliver_message_locally(JS::HostABI::vm_to_abi(Bindings::main_thread_vm()), &storage_key_origin, move(channel_name).into_raw(), &source_origin, message.serialized_message.data.span(), shared_memory.data(), shared_memory_sizes.data(), shared_memory.size());
        });
    });
}

}
