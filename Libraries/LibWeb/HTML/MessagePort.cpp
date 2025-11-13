/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/MemoryStream.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/File.h>
#include <LibIPC/Transport.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MessagePortPrototype.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/StructuredSerializeOptions.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

namespace Web::HTML {

constexpr u8 IPC_FILE_TAG = 0xA5;

GC_DEFINE_ALLOCATOR(MessagePort);

static HashTable<GC::RawPtr<MessagePort>>& all_message_ports()
{
    static HashTable<GC::RawPtr<MessagePort>> ports;
    return ports;
}

GC::Ref<MessagePort> MessagePort::create(JS::Realm& realm)
{
    return realm.create<MessagePort>(realm);
}

MessagePort::MessagePort(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
    all_message_ports().set(this);
}

MessagePort::~MessagePort() = default;

void MessagePort::for_each_message_port(Function<void(MessagePort&)> callback)
{
    for (auto port : all_message_ports())
        callback(*port);
}

void MessagePort::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MessagePort);
    Base::initialize(realm);
}

void MessagePort::finalize()
{
    Base::finalize();
    all_message_ports().remove(this);
    disentangle();
}

void MessagePort::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_remote_port);
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
WebIDL::ExceptionOr<void> MessagePort::transfer_steps(HTML::TransferDataEncoder& data_holder)
{
    // 1. Set value's has been shipped flag to true.
    m_has_been_shipped = true;

    // FIXME: 2. Set dataHolder.[[PortMessageQueue]] to value's port message queue.
    // FIXME: Support delivery of messages that haven't been delivered yet on the other side

    // 3. If value is entangled with another port remotePort, then:
    if (is_entangled()) {
        // 1. Set remotePort's has been shipped flag to true.

        // NOTE: We have to null check here because we can be entangled with a port living in another agent.
        //       In that case, we'll have a transport, but no remote port object.
        if (m_remote_port)
            m_remote_port->m_has_been_shipped = true;

        auto fd = MUST(m_transport->release_underlying_transport_for_transfer());
        m_transport.clear();

        // 2. Set dataHolder.[[RemotePort]] to remotePort.
        // TODO: Mach IPC
        data_holder.encode(IPC_FILE_TAG);
        data_holder.encode(IPC::File::adopt_fd(fd));
    }
    // 4. Otherwise, set dataHolder.[[RemotePort]] to null.
    else {
        data_holder.encode<u8>(0);
    }

    return {};
}

WebIDL::ExceptionOr<void> MessagePort::transfer_receiving_steps(HTML::TransferDataDecoder& data_holder)
{
    // 1. Set value's has been shipped flag to true.
    m_has_been_shipped = true;

    // 2. FIXME: Move all the tasks that are to fire message events in dataHolder.[[PortMessageQueue]] to the port message queue of value,
    //    if any, leaving value's port message queue in its initial disabled state, and, if value's relevant global object is a Window,
    //    associating the moved tasks with value's relevant global object's associated Document.

    // 3. If dataHolder.[[RemotePort]] is not null, then entangle dataHolder.[[RemotePort]] and value.
    //     (This will disentangle dataHolder.[[RemotePort]] from the original port that was transferred.)
    if (auto fd_tag = data_holder.decode<u8>(); fd_tag == IPC_FILE_TAG) {
        // TODO: Mach IPC
        auto fd = data_holder.decode<IPC::File>();
        m_transport = make<IPC::Transport>(MUST(Core::LocalSocket::adopt_fd(fd.take_fd())));

        m_transport->set_up_read_hook([strong_this = GC::make_root(this)]() {
            if (strong_this->m_enabled)
                strong_this->read_from_transport();
        });
    } else if (fd_tag != 0) {
        dbgln("Unexpected byte {:x} in MessagePort transfer data", fd_tag);
        VERIFY_NOT_REACHED();
    }

    return {};
}

void MessagePort::disentangle()
{
    if (auto remote_port = m_remote_port) {
        // Set the pointers to null before disentangling the remote port to prevent infinite recursion here.
        m_remote_port->m_remote_port = nullptr;
        m_remote_port = nullptr;

        if (remote_port)
            remote_port->disentangle();
    }

    if (m_transport) {
        m_transport->close_after_sending_all_pending_messages();
        m_transport.clear();
    }

    m_worker_event_target = nullptr;
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#entangle
void MessagePort::entangle_with(MessagePort& remote_port)
{
    if (m_remote_port.ptr() == &remote_port)
        return;

    // 1. If one of the ports is already entangled, then disentangle it and the port that it was entangled with.
    if (is_entangled())
        disentangle();
    if (remote_port.is_entangled())
        remote_port.disentangle();

    // 2. Associate the two ports to be entangled, so that they form the two parts of a new channel.
    //    (There is no MessageChannel object that represents this channel.)
    remote_port.m_remote_port = this;
    m_remote_port = &remote_port;

    // FIXME: Abstract such that we can entangle different transport types
    auto create_paired_sockets = []() -> Array<NonnullOwnPtr<Core::LocalSocket>, 2> {
        int fds[2] = {};
        MUST(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));
        auto socket0 = MUST(Core::LocalSocket::adopt_fd(fds[0]));
        MUST(socket0->set_blocking(false));
        MUST(socket0->set_close_on_exec(true));
        auto socket1 = MUST(Core::LocalSocket::adopt_fd(fds[1]));
        MUST(socket1->set_blocking(false));
        MUST(socket1->set_close_on_exec(true));

        return Array { move(socket0), move(socket1) };
    };

    auto sockets = create_paired_sockets();
    m_transport = make<IPC::Transport>(move(sockets[0]));
    m_remote_port->m_transport = make<IPC::Transport>(move(sockets[1]));

    m_transport->set_up_read_hook([strong_this = GC::make_root(this)]() {
        if (strong_this->m_enabled)
            strong_this->read_from_transport();
    });

    m_remote_port->m_transport->set_up_read_hook([remote_port = GC::make_root(m_remote_port)]() {
        if (remote_port->m_enabled)
            remote_port->read_from_transport();
    });
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage-options
WebIDL::ExceptionOr<void> MessagePort::post_message(JS::Value message, Vector<GC::Root<JS::Object>> const& transfer)
{
    // 1. Let targetPort be the port with which this MessagePort is entangled, if any; otherwise let it be null.
    GC::Ptr<MessagePort> target_port = m_remote_port;

    // 2. Let options be «[ "transfer" → transfer ]».
    auto options = StructuredSerializeOptions { transfer };

    // 3. Run the message port post message steps providing this, targetPort, message and options.
    return message_port_post_message_steps(target_port, message, options);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage
WebIDL::ExceptionOr<void> MessagePort::post_message(JS::Value message, StructuredSerializeOptions const& options)
{
    // 1. Let targetPort be the port with which this MessagePort is entangled, if any; otherwise let it be null.
    GC::Ptr<MessagePort> target_port = m_remote_port;

    // 2. Run the message port post message steps providing targetPort, message and options.
    return message_port_post_message_steps(target_port, message, options);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#message-port-post-message-steps
WebIDL::ExceptionOr<void> MessagePort::message_port_post_message_steps(GC::Ptr<MessagePort> target_port, JS::Value message, StructuredSerializeOptions const& options)
{
    auto& realm = this->realm();
    auto& vm = this->vm();

    // 1. Let transfer be options["transfer"].
    auto const& transfer = options.transfer;

    // 2. If transfer contains this MessagePort, then throw a "DataCloneError" DOMException.
    for (auto const& handle : transfer) {
        if (handle == this)
            return WebIDL::DataCloneError::create(realm, "Cannot transfer a MessagePort to itself"_utf16);
    }

    // 3. Let doomed be false.
    bool doomed = false;

    // 4. If targetPort is not null and transfer contains targetPort, then set doomed to true and optionally report to a developer console that the target port was posted to itself, causing the communication channel to be lost.
    if (target_port) {
        for (auto const& handle : transfer) {
            if (handle == target_port.ptr()) {
                doomed = true;
                dbgln("FIXME: Report to a developer console that the target port was posted to itself, causing the communication channel to be lost");
            }
        }
    }

    // 5. Let serializeWithTransferResult be StructuredSerializeWithTransfer(message, transfer). Rethrow any exceptions.
    auto serialize_with_transfer_result = TRY(structured_serialize_with_transfer(vm, message, transfer));

    // 6. If targetPort is null, or if doomed is true, then return.
    // IMPLEMENTATION DEFINED: Actually check the socket here, not the target port.
    //     If there's no target message port in the same realm, we still want to send the message over IPC
    if (!m_transport || doomed) {
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
    VERIFY(m_enabled);

    if (!is_entangled())
        return;

    auto schedule_shutdown = m_transport->read_as_many_messages_as_possible_without_blocking([this](auto&& raw_message) {
        FixedMemoryStream stream { raw_message.bytes.span(), FixedMemoryStream::Mode::ReadOnly };
        IPC::Decoder decoder { stream, raw_message.fds };

        auto serialized_transfer_record = MUST(decoder.decode<SerializedTransferRecord>());

        queue_global_task(Task::Source::PostedMessage, relevant_global_object(*this), GC::create_function(heap(), [this, serialized_transfer_record = move(serialized_transfer_record)]() mutable {
            this->post_message_task_steps(serialized_transfer_record);
        }));
    });

    if (schedule_shutdown == IPC::Transport::ShouldShutdown::Yes) {
        queue_global_task(Task::Source::PostedMessage, relevant_global_object(*this), GC::create_function(heap(), [this] {
            this->close();
        }));
    }
}

void MessagePort::post_message_task_steps(SerializedTransferRecord& serialize_with_transfer_result)
{
    VERIFY(m_enabled);

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
    auto& target_realm = relevant_realm(*final_target_port);

    TemporaryExecutionContext context { target_realm };

    // 3. Let deserializeRecord be StructuredDeserializeWithTransfer(serializeWithTransferResult, targetRealm).
    auto deserialize_record_or_error = structured_deserialize_with_transfer(serialize_with_transfer_result, target_realm);
    if (deserialize_record_or_error.is_error()) {
        // If this throws an exception, catch it, fire an event named messageerror at finalTargetPort, using MessageEvent, and then return.
        auto exception = deserialize_record_or_error.release_error();
        MessageEventInit event_init {};
        message_event_target->dispatch_event(MessageEvent::create(target_realm, HTML::EventNames::messageerror, event_init));
        return;
    }
    auto deserialize_record = deserialize_record_or_error.release_value();

    // 4. Let messageClone be deserializeRecord.[[Deserialized]].
    auto message_clone = deserialize_record.deserialized;

    // 5. Let newPorts be a new frozen array consisting of all MessagePort objects in deserializeRecord.[[TransferredValues]], if any, maintaining their relative order.
    // FIXME: Use a FrozenArray
    Vector<GC::Root<MessagePort>> new_ports;
    for (auto const& object : deserialize_record.transferred_values) {
        if (is<HTML::MessagePort>(*object)) {
            new_ports.append(as<MessagePort>(*object));
        }
    }

    // 6. Fire an event named message at finalTargetPort, using MessageEvent, with the data attribute initialized to messageClone and the ports attribute initialized to newPorts.
    MessageEventInit event_init {};
    event_init.data = message_clone;
    event_init.ports = move(new_ports);
    auto event = MessageEvent::create(target_realm, HTML::EventNames::message, event_init);
    event->set_is_trusted(true);
    message_event_target->dispatch_event(event);
}

void MessagePort::enable()
{
    if (!m_enabled) {
        m_enabled = true;
        read_from_transport();
    }
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-start
void MessagePort::start()
{
    if (!is_entangled())
        return;

    VERIFY(m_transport);

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
