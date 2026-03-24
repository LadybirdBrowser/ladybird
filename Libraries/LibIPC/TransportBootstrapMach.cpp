/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <LibCore/MachPort.h>
#include <LibCore/System.h>
#include <LibIPC/MachBootstrapMessages.h>
#include <LibIPC/TransportBootstrapMach.h>

#include <mach/mach.h>

namespace IPC {

ErrorOr<TransportBootstrapMachPorts> bootstrap_transport_from_server_port(Core::MachPort const& server_port)
{
    auto reply_port = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));

    MessageWithSelfTaskPort message {};
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE) | MACH_MSGH_BITS_COMPLEX;
    message.header.msgh_size = sizeof(message);
    message.header.msgh_remote_port = server_port.port();
    message.header.msgh_local_port = reply_port.port();
    message.header.msgh_id = SELF_TASK_PORT_MESSAGE_ID;
    message.body.msgh_descriptor_count = 1;
    message.port_descriptor.name = mach_task_self();
    message.port_descriptor.disposition = MACH_MSG_TYPE_COPY_SEND;
    message.port_descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    mach_msg_timeout_t const send_timeout = 100;
    auto const send_result = mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, message.header.msgh_size, 0, MACH_PORT_NULL, send_timeout, MACH_PORT_NULL);
    if (send_result != KERN_SUCCESS)
        return Core::mach_error_to_error(send_result);

    ReceivedIPCChannelPortsMessage reply {};
    mach_msg_timeout_t const reply_timeout = 5000;
    auto const recv_result = mach_msg(&reply.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(reply),
        reply_port.port(), reply_timeout, MACH_PORT_NULL);
    if (recv_result != KERN_SUCCESS)
        return Core::mach_error_to_error(recv_result);

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

void TransportBootstrapMachServer::send_transport_ports_to_child(Core::MachPort reply_port, TransportBootstrapMachPorts ports)
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
    VERIFY(ret == KERN_SUCCESS);
}

ErrorOr<TransportBootstrapMachPorts> TransportBootstrapMachServer::create_on_demand_local_transport(Core::MachPort reply_port)
{
    auto local_receive_right = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto local_send_right = TRY(local_receive_right.insert_right(Core::MachPort::MessageRight::MakeSend));

    auto remote_receive_right = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto remote_send_right = TRY(remote_receive_right.insert_right(Core::MachPort::MessageRight::MakeSend));

    send_transport_ports_to_child(move(reply_port), TransportBootstrapMachPorts {
                                                        .receive_right = move(remote_receive_right),
                                                        .send_right = move(local_send_right),
                                                    });

    return TransportBootstrapMachPorts {
        .receive_right = move(local_receive_right),
        .send_right = move(remote_send_right),
    };
}

void TransportBootstrapMachServer::register_child_transport(pid_t pid, TransportBootstrapMachPorts ports)
{
    VERIFY(!m_child_transports.contains(pid));
    m_child_transports.set(pid, move(ports));
}

ErrorOr<TransportBootstrapMachServer::BootstrapRequestResult> TransportBootstrapMachServer::handle_bootstrap_request(pid_t pid, Core::MachPort reply_port)
{
    Optional<TransportBootstrapMachPorts> child_transport;
    {
        Threading::MutexLocker locker(m_child_registration_mutex);
        child_transport = m_child_transports.take(pid);
    }

    if (child_transport.has_value()) {
        send_transport_ports_to_child(move(reply_port), child_transport.release_value());
        return ChildTransportHandled {};
    }

    return OnDemandTransport { .ports = TRY(create_on_demand_local_transport(move(reply_port))) };
}

}
