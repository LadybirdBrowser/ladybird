/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/MemoryStream.h>
#include <AK/NeverDestroyed.h>
#include <LibCore/System.h>
#include <LibGC/Heap.h>
#include <LibGC/WeakHashSet.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/Bindings/MessagePort.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

namespace Web::HTML {

constexpr u8 IPC_FILE_TAG = 0xA5;

GC_DEFINE_ALLOCATOR(MessagePort);

static GC::WeakHashSet<MessagePort>& all_message_ports()
{
    static NeverDestroyed<GC::WeakHashSet<MessagePort>> ports;
    return *ports;
}

GC::Ref<MessagePort> MessagePort::create(GC::Ref<DOM::EventTarget> relevant_global_event_target)
{
    return GC::Heap::the().allocate<MessagePort>(relevant_global_event_target);
}

MessagePort::MessagePort(GC::Ref<DOM::EventTarget> relevant_global_event_target)
    : DOM::EventTarget()
    , m_global_event_target(relevant_global_event_target)
{
    all_message_ports().set(*this);
}

MessagePort::~MessagePort() = default;

JS::Object& MessagePort::relevant_global_object() const
{
    return HTML::relevant_global_object(relevant_window_or_worker_global_scope(*m_global_event_target));
}

void MessagePort::for_each_message_port(Function<void(MessagePort&)> callback)
{
    auto ports = all_message_ports();
    for (auto& port : ports)
        callback(port);
}

void MessagePort::finalize()
{
    Base::finalize();
    all_message_ports().remove(*this);
    disentangle();
}

void MessagePort::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_remote_port);
    visitor.visit(m_global_event_target);
    visitor.visit(m_worker_event_target);
}

bool MessagePort::is_entangled() const
{
    return m_transport;
}

void MessagePort::set_worker_event_target(GC::Ref<DOM::EventTarget> target)
{
    m_worker_event_target = target;
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports:transfer-steps
WebIDL::ExceptionOr<void> MessagePort::transfer_steps(JS::Realm&, HTML::TransferDataEncoder& data_holder)
{
    // 1. Set value's has been shipped flag to true.
    m_has_been_shipped = true;

    bool has_remote_port_handle = false;
    IPC::TransportHandle remote_port_handle;

    // 3. If value is entangled with another port remotePort, then:
    if (is_entangled()) {
        // 1. Set remotePort's has been shipped flag to true.

        // NOTE: We have to null check here because we can be entangled with a port living in another agent.
        //       In that case, we'll have a transport, but no remote port object.
        if (m_remote_port)
            m_remote_port->m_has_been_shipped = true;

        // NOTE: release_for_transfer() stops the IO thread before drain_transport() reads the message buffer
        //       below (step 2). Stopping first ensures a consistent snapshot: no new messages arrive between
        //       the drain and the handle being handed to the new owner.
        remote_port_handle = MUST(m_transport->release_for_transfer());
        has_remote_port_handle = true;
    }

    // 2. Set dataHolder.[[PortMessageQueue]] to value's port message queue.

    // Drain any incoming transport state into this port before serializing it so received messages and
    // a pending shutdown move with the transferred port instead of being left behind on the old transport.
    drain_transport();
    data_holder.encode(m_pending_incoming_messages);
    data_holder.encode(m_pending_outgoing_messages);
    data_holder.encode(m_should_shutdown_on_enable);

    if (has_remote_port_handle) {
        m_transport.clear();

        // 2. Set dataHolder.[[RemotePort]] to remotePort.
        data_holder.encode(IPC_FILE_TAG);
        data_holder.encode(remote_port_handle);
    }
    // 4. Otherwise, set dataHolder.[[RemotePort]] to null.
    else {
        data_holder.encode<u8>(0);
    }

    return {};
}

WebIDL::ExceptionOr<void> MessagePort::transfer_receiving_steps(JS::Realm&, HTML::TransferDataDecoder& data_holder)
{
    // 1. Set value's has been shipped flag to true.
    m_has_been_shipped = true;

    // 2. Move all the tasks that are to fire message events in dataHolder.[[PortMessageQueue]] to the port message queue of value,
    //    if any, leaving value's port message queue in its initial disabled state, and, if value's relevant global object is a Window,
    //    associating the moved tasks with value's relevant global object's associated Document.
    m_pending_incoming_messages = data_holder.decode<Vector<SerializedTransferRecord>>();
    m_pending_outgoing_messages = data_holder.decode<Vector<SerializedTransferRecord>>();
    m_should_shutdown_on_enable = data_holder.decode<bool>();
    auto fd_tag = data_holder.decode<u8>();

    // 3. If dataHolder.[[RemotePort]] is not null, then entangle dataHolder.[[RemotePort]] and value.
    //     (This will disentangle dataHolder.[[RemotePort]] from the original port that was transferred.)
    if (fd_tag == IPC_FILE_TAG) {
        auto handle = data_holder.decode<IPC::TransportHandle>();
        m_transport = MUST(handle.create_transport());

        m_transport->set_up_read_hook([strong_this = GC::make_root(this)]() {
            strong_this->read_from_transport();
        });

        flush_pending_outgoing_messages();
    } else if (fd_tag != 0) {
        dbgln("Unexpected byte {:x} in MessagePort transfer data", fd_tag);
        VERIFY_NOT_REACHED();
    }

    return {};
}

void MessagePort::disentangle()
{
    if (m_remote_port) {
        m_remote_port->m_remote_port = nullptr;
        m_remote_port = nullptr;
    }

    if (m_transport) {
        m_transport->close_after_sending_all_pending_messages();
        m_transport.clear();
    }

    m_pending_incoming_messages.clear();
    m_pending_outgoing_messages.clear();
    m_should_shutdown_on_enable = false;
    m_worker_event_target = nullptr;
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#entangle
void MessagePort::entangle_with(MessagePort& remote_port)
{
    if (m_remote_port.ptr() == &remote_port)
        return;

    // 1. If one of the ports is already entangled, then disentangle it and the port that it was entangled with.

    // NB: A port with an active transport should not have pending messages as outgoing messages are flushed when the
    // transport is created, and incoming messages should only be pending on a port that has been transferred.
    if (is_entangled()) {
        VERIFY(m_pending_incoming_messages.is_empty());
        VERIFY(m_pending_outgoing_messages.is_empty());
        disentangle();
    }
    if (remote_port.is_entangled()) {
        VERIFY(remote_port.m_pending_incoming_messages.is_empty());
        VERIFY(remote_port.m_pending_outgoing_messages.is_empty());
        remote_port.disentangle();
    }

    // 2. Associate the two ports to be entangled, so that they form the two parts of a new channel.
    //    (There is no MessageChannel object that represents this channel.)
    remote_port.m_remote_port = this;
    m_remote_port = &remote_port;

    auto paired = MUST(IPC::Transport::create_paired());
    m_transport = move(paired.local);
    m_remote_port->m_transport = MUST(paired.remote_handle.create_transport());

    m_transport->set_up_read_hook([strong_this = GC::make_root(this)]() {
        strong_this->read_from_transport();
    });

    m_remote_port->m_transport->set_up_read_hook([remote_port = GC::make_root(m_remote_port)]() {
        remote_port->read_from_transport();
    });

    flush_pending_outgoing_messages();
    m_remote_port->flush_pending_outgoing_messages();
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage-options
WebIDL::ExceptionOr<void> MessagePort::post_message(JS::Realm& realm, JS::Value message, GC::RootVector<GC::Ref<JS::Object>> const& transfer)
{
    // 1. Let targetPort be the port with which this MessagePort is entangled, if any; otherwise let it be null.
    GC::Ptr<MessagePort> target_port = m_remote_port;

    // 2. Let options be «[ "transfer" → transfer ]».
    auto options = StructuredSerializeOptions { transfer };

    // 3. Run the message port post message steps providing this, targetPort, message and options.
    return message_port_post_message_steps(realm, target_port, message, options);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage
WebIDL::ExceptionOr<void> MessagePort::post_message(JS::Realm& realm, JS::Value message, StructuredSerializeOptions const& options)
{
    // 1. Let targetPort be the port with which this MessagePort is entangled, if any; otherwise let it be null.
    GC::Ptr<MessagePort> target_port = m_remote_port;

    // 2. Run the message port post message steps providing targetPort, message and options.
    return message_port_post_message_steps(realm, target_port, message, options);
}

WebIDL::ExceptionOr<void> MessagePort::post_message(JS::Realm& realm, JS::Value message, Bindings::StructuredSerializeOptions const& options)
{
    return post_message(realm, message, StructuredSerializeOptions { .transfer = options.transfer });
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#message-port-post-message-steps
WebIDL::ExceptionOr<void> MessagePort::message_port_post_message_steps(JS::Realm& realm, GC::Ptr<MessagePort> target_port, JS::Value message, StructuredSerializeOptions const& options)
{
    // 1. Let transfer be options["transfer"].
    auto const& transfer = options.transfer;

    // 2. If transfer contains this MessagePort, then throw a "DataCloneError" DOMException.
    if (Bindings::transfer_list_contains_message_port(transfer, *this))
        return WebIDL::DataCloneError::create("Cannot transfer a MessagePort to itself"_utf16);

    // 3. Let doomed be false.
    bool doomed = false;

    // 4. If targetPort is not null and transfer contains targetPort, then set doomed to true and optionally report to a developer console that the target port was posted to itself, causing the communication channel to be lost.
    if (target_port) {
        if (Bindings::transfer_list_contains_message_port(transfer, *target_port)) {
            doomed = true;
            dbgln("FIXME: Report to a developer console that the target port was posted to itself, causing the communication channel to be lost");
        }
    }

    // 5. Let serializeWithTransferResult be StructuredSerializeWithTransfer(message, transfer). Rethrow any exceptions.
    auto serialize_with_transfer_result = TRY(structured_serialize_with_transfer(realm, message, transfer));

    // 6. If targetPort is null, or if doomed is true, then return.

    // IMPLEMENTATION DEFINED: A port can exist before it has a transport to send on. Keep the serialized
    // record on the port and flush it once the port becomes entangled.
    if (doomed) {
        return {};
    }

    if (!m_transport) {
        if (!is_detached())
            m_pending_outgoing_messages.append(move(serialize_with_transfer_result));
        return {};
    }

    // 7. Add a task that runs the following steps to the port message queue of targetPort:
    post_port_message(serialize_with_transfer_result);

    return {};
}

ErrorOr<void> MessagePort::send_message_on_transport(SerializedTransferRecord const& serialize_with_transfer_result)
{
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder(buffer);
    MUST(encoder.encode(serialize_with_transfer_result));

    TRY(buffer.transfer_message(*m_transport));
    return {};
}

void MessagePort::flush_pending_outgoing_messages()
{
    if (!m_transport || !m_transport->is_open())
        return;

    auto pending_outgoing_messages = move(m_pending_outgoing_messages);
    for (auto const& pending_message : pending_outgoing_messages)
        post_port_message(pending_message);
}

void MessagePort::dispatch_pending_messages()
{
    auto pending_messages = move(m_pending_incoming_messages);
    for (auto& pending_message : pending_messages)
        queue_message_task(move(pending_message));

    if (m_should_shutdown_on_enable) {
        m_should_shutdown_on_enable = false;
        auto& global = relevant_global_object();
        queue_global_task(Task::Source::PostedMessage, global, GC::create_function(GC::Heap::the(), [this] {
            this->close();
        }));
    }
}

void MessagePort::queue_message_task(SerializedTransferRecord&& serialize_with_transfer_result)
{
    auto& global = relevant_global_object();
    queue_global_task(Task::Source::PostedMessage, global,
        GC::create_function(GC::Heap::the(), [this, serialize_with_transfer_result = move(serialize_with_transfer_result)]() mutable {
            this->post_message_task_steps(serialize_with_transfer_result);
        }));
}

void MessagePort::drain_transport()
{
    if (!m_transport)
        return;

    auto schedule_shutdown = m_transport->read_as_many_messages_as_possible_without_blocking([this](auto&& raw_message) {
        FixedMemoryStream stream { raw_message.bytes.span(), FixedMemoryStream::Mode::ReadOnly };
        IPC::Decoder decoder { stream, raw_message.attachments };

        m_pending_incoming_messages.append(MUST(decoder.decode<SerializedTransferRecord>()));
    });

    if (schedule_shutdown == IPC::Transport::ShouldShutdown::Yes)
        m_should_shutdown_on_enable = true;
}

void MessagePort::post_port_message(SerializedTransferRecord const& serialize_with_transfer_result)
{
    if (!m_transport || !m_transport->is_open())
        return;
    if (auto result = send_message_on_transport(serialize_with_transfer_result); result.is_error()) {
        dbgln("Failed to post message: {}", result.error());
        disentangle();
    }
}

void MessagePort::read_from_transport()
{
    if (!is_entangled())
        return;

    drain_transport();

    if (m_enabled)
        dispatch_pending_messages();
}

void MessagePort::post_message_task_steps(SerializedTransferRecord& serialize_with_transfer_result)
{
    // 1. Let finalTargetPort be the MessagePort in whose port message queue the task now finds itself.
    // NOTE: This can be different from targetPort, if targetPort itself was transferred and thus all its tasks moved along with it.
    auto* final_target_port = this;

    // IMPLEMENTATION DEFINED:
    // https://html.spec.whatwg.org/multipage/workers.html#dedicated-workers-and-the-worker-interface
    //      Worker objects act as if they had an implicit MessagePort associated with them.
    //      All messages received by that port must immediately be retargeted at the Worker object.
    // We therefore set a special event target for those implicit ports on the Worker and the WorkerGlobalScope objects
    EventTarget* message_event_target = final_target_port;
    if (m_worker_event_target != nullptr) {
        message_event_target = m_worker_event_target;
    }

    // 2. Let targetRealm be finalTargetPort's relevant realm.
    auto& target_realm = HTML::relevant_realm(final_target_port->relevant_global_object());

    TemporaryExecutionContext context { target_realm };

    // 3. Let deserializeRecord be StructuredDeserializeWithTransfer(serializeWithTransferResult, targetRealm).
    auto deserialize_record_or_error = structured_deserialize_with_transfer(serialize_with_transfer_result, target_realm);
    if (deserialize_record_or_error.is_error()) {
        // If this throws an exception, catch it, fire an event named messageerror at finalTargetPort, using MessageEvent, and then return.
        auto exception = deserialize_record_or_error.release_error();
        MessageEventInit event_init;
        message_event_target->dispatch_event(MessageEvent::create(target_realm.global_object(), HTML::EventNames::messageerror, event_init));
        return;
    }
    auto deserialize_record = deserialize_record_or_error.release_value();

    // 4. Let messageClone be deserializeRecord.[[Deserialized]].
    auto message_clone = deserialize_record.deserialized;

    // 5. Let newPorts be a new frozen array consisting of all MessagePort objects in deserializeRecord.[[TransferredValues]], if any, maintaining their relative order.
    // FIXME: Use a FrozenArray
    auto new_ports = Bindings::message_ports_from_transferred_values(deserialize_record.transferred_values);

    // 6. Fire an event named message at finalTargetPort, using MessageEvent, with the data attribute initialized to messageClone and the ports attribute initialized to newPorts.
    MessageEventInit event_init { {}, message_clone, String {}, String {}, move(new_ports), Empty {} };
    auto event = MessageEvent::create(target_realm.global_object(), HTML::EventNames::message, event_init);
    event->set_is_trusted(true);
    message_event_target->dispatch_event(event);
}

void MessagePort::enable()
{
    if (!m_enabled) {
        m_enabled = true;
        if (m_transport) {
            read_from_transport();
        } else {
            dispatch_pending_messages();
        }
    }
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-start
void MessagePort::start()
{
    // The start() method steps are to enable this's port message queue, if it is not already enabled.
    enable();
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-close
void MessagePort::close()
{
    // 1. Set this MessagePort object's [[Detached]] internal slot value to true.
    set_detached(true);

    // 2. If this MessagePort object is entangled, disentangle it.
    if (is_entangled())
        disentangle();

    m_pending_incoming_messages.clear();
    m_pending_outgoing_messages.clear();
    m_should_shutdown_on_enable = false;
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-messageeventtarget-onmessageerror
void MessagePort::set_onmessageerror(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::messageerror, value);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-messageeventtarget-onmessageerror
GC::Ptr<WebIDL::CallbackType> MessagePort::onmessageerror()
{
    return event_handler_attribute(EventNames::messageerror);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-messageeventtarget-onmessage
void MessagePort::set_onmessage(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::message, value);

    // https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports:handler-messageeventtarget-onmessage
    // The first time a MessagePort object's onmessage IDL attribute is set, the port's port message queue must be enabled,
    // as if the start() method had been called.
    start();
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-messageeventtarget-onmessage
GC::Ptr<WebIDL::CallbackType> MessagePort::onmessage()
{
    return event_handler_attribute(EventNames::message);
}

}

namespace Web::Bindings {

GC::Ref<PlatformObject> message_port(JS::Realm& realm, GC::Ref<HTML::MessagePort> message_port)
{
    return wrap(host_defined_wrapper_world(realm), realm, message_port);
}

HTML::MessagePort* message_port_from_value(JS::Value value)
{
    if (!value.is_object())
        return nullptr;
    return Bindings::impl_from<HTML::MessagePort>(&value.as_object());
}

GC::RootVector<GC::Ref<HTML::MessagePort>> message_ports_from_transferred_values(Vector<GC::Root<JS::Object>> const& transferred_values)
{
    GC::RootVector<GC::Ref<HTML::MessagePort>> ports;
    for (auto const& object : transferred_values) {
        if (auto* message_port = message_port_from_value(object))
            ports.append(*message_port);
    }
    return ports;
}

bool transfer_list_contains_message_port(GC::RootVector<GC::Ref<JS::Object>> const& transfer, HTML::MessagePort const& port)
{
    for (auto const& handle : transfer) {
        if (message_port_from_value(handle) == &port)
            return true;
    }
    return false;
}

HTML::MessagePort* message_port_from_object(JS::Object& object)
{
    return Bindings::impl_from<HTML::MessagePort>(&object);
}

void serialize_message_port_with_transfer(JS::Realm& realm, HTML::TransferDataEncoder& data_holder, GC::Ref<HTML::MessagePort> port)
{
    auto port_wrapper = Bindings::message_port(realm, port);
    auto result = MUST(HTML::structured_serialize_with_transfer(realm, JS::Value { port_wrapper }, { { port_wrapper } }));
    data_holder.extend(move(result.transfer_data_holders));
}

}
