/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
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
JSCompletion web_rust_message_port_transfer_steps(JSVM*, void* realm, void* port, bool may_leave_process, TransferDataEncoder** out_data_holder, u64* out_parked_end_token);
void web_rust_message_port_give_up_on_parked_end(u64 token);
void web_rust_message_port_entanglement_kind(void* port, ReadonlyBytes* out_kind);
bool web_rust_message_port_transport_is_closed_for_testing(void* port);
JSCompletion web_rust_message_port_transfer_receiving_steps(JSVM*, void* realm, TransferDataEncoder*);

// Declared in Bindings/Rust/src/html/message_port.rs.
IPC::Transport* web_bindings_message_port_transport_create_paired(IPC::TransportHandle** out_remote_handle);
void web_bindings_message_port_transport_destroy(IPC::Transport*);
Core::WeakEventLoopReference* web_bindings_message_port_current_event_loop();
void web_bindings_message_port_release_event_loop(Core::WeakEventLoopReference*);
void web_bindings_message_port_post_to_event_loop(Core::WeakEventLoopReference*, void (*steps)(u64), u64 argument);
IPC::TransportHandle* web_bindings_message_port_transport_release_for_transfer(IPC::Transport*);
void web_bindings_message_port_transport_set_up_read_hook(IPC::Transport*, void* state, void (*steps)(void*));
bool web_bindings_message_port_transport_is_open(IPC::Transport*);
void web_bindings_message_port_transport_close(IPC::Transport*);
void web_bindings_message_port_transport_close_after_sending_all_pending_messages(IPC::Transport*);
bool web_bindings_message_port_transport_send(IPC::Transport*, ReadonlyBytes, TransferAttachment const*, size_t);
bool web_bindings_message_port_transport_read_messages(IPC::Transport*, void*, bool (*)(void*, ReadonlyBytes, TransferAttachment const*, size_t));
void web_bindings_transport_handle_take(IPC::TransportHandle*, void*, void (*)(void*, ReadonlyBytes, TransferAttachment const*, size_t));
IPC::Transport* web_bindings_transport_handle_create_transport(ReadonlyBytes, TransferAttachment const*, size_t);
}

static JSVM* vm_abi()
{
    return JS::HostABI::vm_to_abi(JS::VM::the());
}

GC::Ref<MessagePort> MessagePort::create(JS::Realm& realm)
{
    return GC::Ref { *static_cast<MessagePort*>(web_rust_message_port_create(vm_abi(), &realm)) };
}

GC::Ref<MessagePort> MessagePort::create_from_transfer(JS::Realm& realm, TransferDataEncoder&& data_holder)
{
    auto wrapper = MUST(JS::HostABI::completion_from_abi<JS::Value>(web_rust_message_port_transfer_receiving_steps(vm_abi(), &realm, new TransferDataEncoder(move(data_holder)))));
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

WebIDL::ExceptionOr<MessagePort::TransferredToWorker> MessagePort::transfer_to_worker(JS::Realm& realm, WorkerMayBeInOtherProcess worker_may_be_in_other_process)
{
    TransferDataEncoder* data_holder = nullptr;
    u64 parked_end_token = 0;
    TRY(JS::HostABI::completion_from_abi<void>(web_rust_message_port_transfer_steps(vm_abi(), &realm, this, worker_may_be_in_other_process == WorkerMayBeInOtherProcess::Yes, &data_holder, &parked_end_token)));
    TransferredToWorker result { move(*data_holder), parked_end_token };
    delete data_holder;
    return result;
}

void MessagePort::give_up_on_parked_end(u64 parked_end_token)
{
    web_rust_message_port_give_up_on_parked_end(parked_end_token);
}

StringView MessagePort::entanglement_kind_for_testing() const
{
    ReadonlyBytes kind;
    web_rust_message_port_entanglement_kind(const_cast<MessagePort*>(this), &kind);
    return StringView { kind };
}

bool MessagePort::transport_is_closed_for_testing() const
{
    return web_rust_message_port_transport_is_closed_for_testing(const_cast<MessagePort*>(this));
}

size_t MessagePort::pending_outgoing_message_count() const
{
    return web_rust_message_port_pending_outgoing_message_count(const_cast<MessagePort*>(this));
}

void MessagePort::fail_next_transfer_for_testing()
{
    web_rust_message_port_fail_next_transfer_for_testing(this);
}

IPC::Transport* web_bindings_message_port_transport_create_paired(IPC::TransportHandle** out_remote_handle)
{
    auto paired = MUST(IPC::Transport::create_paired());
    *out_remote_handle = new IPC::TransportHandle(move(paired.remote_handle));
    return paired.local.leak_ptr();
}

void web_bindings_message_port_transport_destroy(IPC::Transport* transport)
{
    delete transport;
}

// A reference to the event loop of the calling thread, which any thread may use, handed over.
Core::WeakEventLoopReference* web_bindings_message_port_current_event_loop()
{
    return &Core::EventLoop::current_weak().leak_ref();
}

void web_bindings_message_port_release_event_loop(Core::WeakEventLoopReference* event_loop)
{
    event_loop->unref();
}

void web_bindings_message_port_post_to_event_loop(Core::WeakEventLoopReference* event_loop, void (*steps)(u64), u64 argument)
{
    if (auto strong_event_loop = event_loop->take(); strong_event_loop.is_alive())
        strong_event_loop->deferred_invoke([steps, argument] { steps(argument); });
}

// The rights of the transport's end as a handle, handed over, after its IO thread stops.
IPC::TransportHandle* web_bindings_message_port_transport_release_for_transfer(IPC::Transport* transport)
{
    return new IPC::TransportHandle(MUST(transport->release_for_transfer()));
}

void web_bindings_message_port_transport_set_up_read_hook(IPC::Transport* transport, void* state, void (*steps)(void*))
{
    transport->set_up_read_hook([state, steps] { steps(state); });
}

bool web_bindings_message_port_transport_is_open(IPC::Transport* transport)
{
    return transport->is_open();
}

void web_bindings_message_port_transport_close(IPC::Transport* transport)
{
    transport->close();
}

void web_bindings_message_port_transport_close_after_sending_all_pending_messages(IPC::Transport* transport)
{
    transport->close_after_sending_all_pending_messages();
}

void web_bindings_transport_handle_take(IPC::TransportHandle* handle, void* context, void (*receive)(void*, ReadonlyBytes, TransferAttachment const*, size_t))
{
    auto owner = adopt_own(*handle);
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder(buffer);
    MUST(encoder.encode(*owner));
    auto attachments = IPC::release_raw_attachments(buffer.take_attachments());
    receive(context, buffer.data().span(), attachments.data(), attachments.size());
}

IPC::Transport* web_bindings_transport_handle_create_transport(ReadonlyBytes bytes, TransferAttachment const* attachments, size_t count)
{
    FixedMemoryStream stream(bytes);
    Queue<IPC::Attachment> queue;
    for (auto& attachment : IPC::adopt_raw_attachments({ attachments, count }))
        queue.enqueue(move(attachment));
    IPC::Decoder decoder(stream, queue);
    auto handle = MUST(decoder.decode<IPC::TransportHandle>());
    return MUST(handle.create_transport()).leak_ptr();
}

bool web_bindings_message_port_transport_send(IPC::Transport* transport, ReadonlyBytes bytes, TransferAttachment const* attachments, size_t count)
{
    IPC::MessageDataType data;
    MUST(data.try_append(bytes.data(), bytes.size()));
    IPC::MessageBuffer buffer { move(data), IPC::adopt_raw_attachments({ attachments, count }) };
    if (auto result = buffer.transfer_message(*transport); result.is_error()) {
        dbgln("Failed to post message: {}", result.error());
        return false;
    }
    return true;
}

bool web_bindings_message_port_transport_read_messages(IPC::Transport* transport, void* context, bool (*receive)(void*, ReadonlyBytes, TransferAttachment const*, size_t))
{
    bool failed_to_decode = false;
    auto should_shutdown = transport->read_as_many_messages_as_possible_without_blocking([&](auto&& message) {
        // The peer is treated as gone after a malformed record. Later messages still release all their attachments.
        if (failed_to_decode)
            return;
        Vector<IPC::Attachment> attachments;
        while (!message.attachments.is_empty())
            attachments.append(message.attachments.dequeue());
        auto raw_attachments = IPC::release_raw_attachments(move(attachments));
        failed_to_decode = !receive(context, message.bytes.bytes(), raw_attachments.data(), raw_attachments.size());
    });
    return failed_to_decode || should_shutdown == IPC::Transport::ShouldShutdown::Yes;
}

}
