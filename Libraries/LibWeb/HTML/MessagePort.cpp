/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RustSerialization.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::HTML {

extern "C" {

// Defined in Bindings/Rust/src/html/exports.rs.
void* web_rust_message_port_create(JSVM*, void* realm);
void web_rust_message_port_entangle(JSVM*, void* port, void* remote_port);
void* web_rust_message_port_entangled_port(void* port);
void web_rust_message_port_set_message_event_target(void* port, DOM::EventTarget*);
JSCompletion web_rust_message_port_post_message(JSVM*, void* realm, void* port, JSValue message, JSObject* const* transfer, size_t count);
void web_rust_message_port_enable(JSVM*, void* port);
void web_rust_message_port_close(void* port);
void web_rust_message_port_discard_pending_messages(void* port);
size_t web_rust_message_port_pending_outgoing_message_count(void* port);
void web_rust_message_port_fail_next_transfer_for_testing(void* port);
void web_rust_message_port_disentangle_ports_of_realm(JSVM*, void* realm);

// The entry points of the generated bindings of the interface.
bool web_rust_message_port_is_detached(void* port);
JSCompletion web_rust_message_port_transfer_steps(JSVM*, void* realm, void* port, Bindings::RustTransferDataWriter*);
JSCompletion web_rust_message_port_transfer_receiving_steps(JSVM*, void* realm, Bindings::RustTransferDataReader*);
}

static JSVM* vm_abi()
{
    return JS::HostABI::vm_to_abi(JS::VM::the());
}

GC::Ref<MessagePort> MessagePort::create(JS::Realm& realm)
{
    return GC::Ref { *static_cast<MessagePort*>(web_rust_message_port_create(vm_abi(), &realm)) };
}

GC::Ref<MessagePort> MessagePort::create_from_transfer(JS::Realm& realm, TransferDataDecoder& data_holder)
{
    Bindings::RustTransferDataReader reader { .realm = realm, .data_holder = data_holder };
    auto wrapper = MUST(JS::HostABI::completion_from_abi<JS::Value>(web_rust_message_port_transfer_receiving_steps(vm_abi(), &realm, &reader)));
    return *Bindings::impl_from<MessagePort>(&wrapper.as_object());
}

void MessagePort::disentangle_ports_of_realm(JS::Realm& realm)
{
    web_rust_message_port_disentangle_ports_of_realm(vm_abi(), &realm);
}

void MessagePort::entangle_with(MessagePort& remote_port)
{
    web_rust_message_port_entangle(vm_abi(), this, &remote_port);
}

GC::Ptr<MessagePort> MessagePort::entangled_port()
{
    return static_cast<MessagePort*>(web_rust_message_port_entangled_port(this));
}

void MessagePort::set_message_event_target(DOM::EventTarget& target)
{
    web_rust_message_port_set_message_event_target(this, &target);
}

WebIDL::ExceptionOr<void> MessagePort::post_message(JS::Realm& realm, JS::Value message, GC::RootVector<GC::Ref<JS::Object>> const& transfer)
{
    Vector<JSObject*> transfer_abi;
    transfer_abi.ensure_capacity(transfer.size());
    for (auto const& transferable : transfer)
        transfer_abi.unchecked_append(JS::HostABI::object_to_abi(transferable.ptr()));
    TRY(JS::HostABI::completion_from_abi<void>(web_rust_message_port_post_message(vm_abi(), &realm, this, JS::HostABI::value_to_abi(message), transfer_abi.data(), transfer_abi.size())));
    return {};
}

void MessagePort::enable()
{
    web_rust_message_port_enable(vm_abi(), this);
}

void MessagePort::close()
{
    web_rust_message_port_close(this);
}

void MessagePort::discard_pending_messages()
{
    web_rust_message_port_discard_pending_messages(this);
}

WebIDL::ExceptionOr<void> MessagePort::transfer_steps(JS::Realm& realm, TransferDataEncoder& data_holder)
{
    Bindings::RustTransferDataWriter writer { .realm = realm, .data_holder = data_holder };
    TRY(JS::HostABI::completion_from_abi<void>(web_rust_message_port_transfer_steps(vm_abi(), &realm, this, &writer)));
    return {};
}

bool MessagePort::is_detached() const
{
    return web_rust_message_port_is_detached(const_cast<MessagePort*>(this));
}

size_t MessagePort::pending_outgoing_message_count() const
{
    return web_rust_message_port_pending_outgoing_message_count(const_cast<MessagePort*>(this));
}

void MessagePort::fail_next_transfer_for_testing()
{
    web_rust_message_port_fail_next_transfer_for_testing(this);
}

}

namespace Web::Bindings {

GC::RootVector<GC::Ref<HTML::MessagePort>> message_ports_from_transferred_values(Vector<GC::Root<JS::Object>> const& transferred_values)
{
    GC::RootVector<GC::Ref<HTML::MessagePort>> ports;
    for (auto const& object : transferred_values) {
        if (auto* message_port = impl_from<HTML::MessagePort>(object.ptr()))
            ports.append(*message_port);
    }
    return ports;
}

}
