/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/ByteString.h>
#include <AK/Mutex.h>
#include <AK/Optional.h>
#include <LibCore/MachPort.h>
#include <LibCore/System.h>
#include <LibIPC/MachBootstrapMessages.h>
#include <LibIPC/TransportBootstrapMach.h>
#include <LibIPC/TransportMachPort.h>
#include <LibThreading/Thread.h>

#include <errno.h>
#include <mach/mach.h>
#include <sys/event.h>
#include <unistd.h>

namespace IPC {

ErrorOr<TransportBootstrapMachPorts> bootstrap_transport_from_server_port(Core::MachPort const& server_port, AK::Duration reply_timeout)
{
    auto reply_port = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));

    // NB: Send only the task name port. It is enough for task_info() statistics, while the task control port from
    //     mach_task_self() would give the server full control over this process, including its memory.
    mach_port_t raw_task_name_port = MACH_PORT_NULL;
    auto const name_port_result = task_get_special_port(mach_task_self(), TASK_NAME_PORT, &raw_task_name_port);
    if (name_port_result != KERN_SUCCESS)
        return Core::mach_error_to_error(name_port_result);
    auto task_name_port = Core::MachPort::adopt_right(raw_task_name_port, Core::MachPort::PortRight::Send);

    MessageWithSelfTaskNamePort message {};
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE) | MACH_MSGH_BITS_COMPLEX;
    message.header.msgh_size = sizeof(message);
    message.header.msgh_remote_port = server_port.port();
    message.header.msgh_local_port = reply_port.port();
    message.header.msgh_id = SELF_TASK_NAME_PORT_MESSAGE_ID;
    message.body.msgh_descriptor_count = 1;
    message.port_descriptor.name = task_name_port.port();
    message.port_descriptor.disposition = MACH_MSG_TYPE_COPY_SEND;
    message.port_descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    mach_msg_timeout_t const send_timeout = 100;
    auto const send_result = mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, message.header.msgh_size, 0, MACH_PORT_NULL, send_timeout, MACH_PORT_NULL);
    if (send_result != KERN_SUCCESS)
        return Core::mach_error_to_error(send_result);

    ReceivedIPCChannelPortsMessage reply {};
    auto const recv_result = mach_msg(&reply.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(reply),
        reply_port.port(), static_cast<mach_msg_timeout_t>(reply_timeout.to_milliseconds()), MACH_PORT_NULL);
    if (recv_result != KERN_SUCCESS)
        return Core::mach_error_to_error(recv_result);

    if (reply.header.msgh_id == MACH_NOTIFY_SEND_ONCE)
        return Error::from_string_literal("Mach bootstrap server closed before replying");

    VERIFY(reply.header.msgh_id == IPC_CHANNEL_PORTS_MESSAGE_ID);
    VERIFY(reply.body.msgh_descriptor_count == 2);
    VERIFY(reply.receive_port.type == MACH_MSG_PORT_DESCRIPTOR);
    VERIFY(reply.receive_port.disposition == MACH_MSG_TYPE_MOVE_RECEIVE);
    VERIFY(reply.send_port.type == MACH_MSG_PORT_DESCRIPTOR);
    VERIFY(reply.send_port.disposition == MACH_MSG_TYPE_MOVE_SEND);

    return TransportBootstrapMachPorts {
        .receive_right = Core::MachPort::adopt_right(reply.receive_port.name, Core::MachPort::PortRight::Receive),
        .send_right = Core::MachPort::adopt_right(reply.send_port.name, Core::MachPort::PortRight::Send),
    };
}

ErrorOr<TransportBootstrapMachPorts> bootstrap_transport_from_mach_server(StringView server_name)
{
    auto server_port = TRY(Core::MachPort::look_up_from_bootstrap_server(ByteString { server_name }));
    return bootstrap_transport_from_server_port(server_port);
}

ErrorOr<void> TransportBootstrapMachServer::send_transport_ports_to_child(Core::MachPort reply_port, TransportBootstrapMachPorts ports)
{
    MessageWithIPCChannelPorts message {};
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
    message.header.msgh_size = sizeof(message);
    message.header.msgh_remote_port = reply_port.release();
    message.header.msgh_local_port = MACH_PORT_NULL;
    message.header.msgh_id = IPC_CHANNEL_PORTS_MESSAGE_ID;
    message.body.msgh_descriptor_count = 2;
    message.receive_port.name = ports.receive_right.release();
    message.receive_port.disposition = MACH_MSG_TYPE_MOVE_RECEIVE;
    message.receive_port.type = MACH_MSG_PORT_DESCRIPTOR;
    message.send_port.name = ports.send_right.release();
    message.send_port.disposition = MACH_MSG_TYPE_MOVE_SEND;
    message.send_port.type = MACH_MSG_PORT_DESCRIPTOR;

    mach_msg_timeout_t const timeout = 5000;
    auto const ret = mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(message), 0, MACH_PORT_NULL, timeout, MACH_PORT_NULL);
    if (ret != KERN_SUCCESS) {
        // NB: These errors leave the message's rights with the sender. The child may have exited before the reply.
        if (ret == MACH_SEND_INVALID_DEST || ret == MACH_SEND_TIMED_OUT || ret == MACH_SEND_INTERRUPTED || ret == MACH_SEND_NO_BUFFER)
            mach_msg_destroy(&message.header);
        return Core::mach_error_to_error(ret);
    }
    return {};
}

ErrorOr<TransportBootstrapMachPorts> TransportBootstrapMachServer::create_on_demand_local_transport(Core::MachPort reply_port)
{
    auto local_receive_right = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto local_send_right = TRY(local_receive_right.insert_right(Core::MachPort::MessageRight::MakeSend));

    auto remote_receive_right = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto remote_send_right = TRY(remote_receive_right.insert_right(Core::MachPort::MessageRight::MakeSend));

    // Either peer may send messages before the other has constructed its transport.
    TransportMachPort::raise_receive_queue_limit(local_receive_right);
    TransportMachPort::raise_receive_queue_limit(remote_receive_right);

    TRY(send_transport_ports_to_child(move(reply_port), TransportBootstrapMachPorts {
                                                            .receive_right = move(remote_receive_right),
                                                            .send_right = move(local_send_right),
                                                        }));

    return TransportBootstrapMachPorts {
        .receive_right = move(local_receive_right),
        .send_right = move(remote_send_right),
    };
}

// What wakes the child exit watcher when the server goes away.
static constexpr uintptr_t stop_watching_event_identifier = 1;

TransportBootstrapMachServer::TransportBootstrapMachServer()
    : m_child_exit_queue(kqueue())
{
    if (m_child_exit_queue < 0) {
        dbgln("Unable to create the queue that reports children that exit before they take their transport: {}", Error::from_errno(errno));
        return;
    }

    struct kevent change {};
    EV_SET(&change, stop_watching_event_identifier, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    VERIFY(kevent(m_child_exit_queue, &change, 1, nullptr, 0, nullptr) == 0);
}

TransportBootstrapMachServer::~TransportBootstrapMachServer()
{
    if (m_child_exit_watcher) {
        struct kevent change {};
        EV_SET(&change, stop_watching_event_identifier, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        VERIFY(kevent(m_child_exit_queue, &change, 1, nullptr, 0, nullptr) == 0);
        (void)m_child_exit_watcher->join();
    }
    if (m_child_exit_queue >= 0)
        (void)::close(m_child_exit_queue);
}

void TransportBootstrapMachServer::register_child_transport(pid_t pid, TransportBootstrapMachPorts ports)
{
    // NB: Ports that are still registered for this PID are those of an earlier child that exited before it took them,
    //     and whose exit has not been handled yet. They are replaced.
    auto serial = m_next_child_transport_serial++;

    if (m_child_exit_queue >= 0) {
        if (!m_child_exit_watcher) {
            m_child_exit_watcher = Threading::Thread::construct("ChildExitWatch"sv, [this]() -> intptr_t {
                child_exit_watcher_loop();
                return 0;
            });
            m_child_exit_watcher->start();
        }

        // NB: The serial tells the watcher which registration an exit belongs to, as a later child can get the PID of
        //     an earlier one.
        struct kevent change {};
        EV_SET(&change, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, reinterpret_cast<void*>(static_cast<uintptr_t>(serial)));
        if (kevent(m_child_exit_queue, &change, 1, nullptr, 0, nullptr) < 0) {
            // The child is gone already, so it never takes these ports. Letting go of them tells the other end.
            if (errno == ESRCH)
                return;
            dbgln("Unable to watch for the exit of child {}: {}", pid, Error::from_errno(errno));
        }
    }

    m_child_transports.set(pid, ChildTransport { move(ports), serial });
}

void TransportBootstrapMachServer::stop_watching_for_child_exit(pid_t pid)
{
    if (m_child_exit_queue < 0)
        return;
    // NB: This fails for a child whose exit was reported already, which is fine.
    struct kevent change {};
    EV_SET(&change, pid, EVFILT_PROC, EV_DELETE, 0, 0, nullptr);
    (void)kevent(m_child_exit_queue, &change, 1, nullptr, 0, nullptr);
}

void TransportBootstrapMachServer::unregister_child_transport(pid_t pid)
{
    MutexLocker locker(m_child_registration_mutex);
    if (m_child_transports.remove(pid))
        stop_watching_for_child_exit(pid);
}

void TransportBootstrapMachServer::child_exit_watcher_loop()
{
    for (;;) {
        struct kevent event {};
        auto count = kevent(m_child_exit_queue, nullptr, 0, &event, 1, nullptr);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            dbgln("Unable to wait for children to exit: {}", Error::from_errno(errno));
            return;
        }
        if (count == 0)
            continue;
        if (event.filter == EVFILT_USER)
            return;
        if (event.filter != EVFILT_PROC)
            continue;

        auto pid = static_cast<pid_t>(event.ident);
        auto serial = static_cast<u64>(reinterpret_cast<uintptr_t>(event.udata));

        MutexLocker locker(m_child_registration_mutex);
        auto child_transport = m_child_transports.get(pid);
        if (child_transport.has_value() && child_transport->serial == serial)
            m_child_transports.remove(pid);
    }
}

ErrorOr<TransportBootstrapMachServer::BootstrapRequestResult> TransportBootstrapMachServer::handle_bootstrap_request(pid_t pid, Core::MachPort reply_port)
{
    Optional<TransportBootstrapMachPorts> child_transport;
    {
        MutexLocker locker(m_child_registration_mutex);
        if (auto registered = m_child_transports.take(pid); registered.has_value()) {
            stop_watching_for_child_exit(pid);
            child_transport = move(registered->ports);
        }
    }

    if (child_transport.has_value()) {
        TRY(send_transport_ports_to_child(move(reply_port), child_transport.release_value()));
        return ChildTransportHandled {};
    }

    return OnDemandTransport { .ports = TRY(create_on_demand_local_transport(move(reply_port))) };
}

}
