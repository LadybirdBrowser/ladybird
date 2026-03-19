/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <LibCore/MachPort.h>
#include <LibCore/Platform/MachMessageTypes.h>
#include <LibCore/System.h>
#include <LibIPC/TransportBootstrapMach.h>

#include <mach/mach.h>

namespace IPC {

static ErrorOr<void> write_exact(int fd, ReadonlyBytes bytes)
{
    size_t total_written = 0;
    while (total_written < bytes.size()) {
        auto nwritten = TRY(Core::System::write(fd, bytes.slice(total_written)));
        VERIFY(nwritten > 0);
        total_written += nwritten;
    }
    return {};
}

static ErrorOr<void> read_exact(int fd, Bytes bytes)
{
    size_t total_read = 0;
    while (total_read < bytes.size()) {
        auto nread = TRY(Core::System::read(fd, bytes.slice(total_read)));
        VERIFY(nread > 0);
        total_read += nread;
    }
    return {};
}

ErrorOr<TransportBootstrapMachPorts> bootstrap_transport_from_server_port(Core::MachPort const& server_port)
{
    auto reply_port = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));

    Core::Platform::MessageWithSelfTaskPort message {};
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE) | MACH_MSGH_BITS_COMPLEX;
    message.header.msgh_size = sizeof(message);
    message.header.msgh_remote_port = server_port.port();
    message.header.msgh_local_port = reply_port.port();
    message.header.msgh_id = Core::Platform::SELF_TASK_PORT_MESSAGE_ID;
    message.body.msgh_descriptor_count = 1;
    message.port_descriptor.name = mach_task_self();
    message.port_descriptor.disposition = MACH_MSG_TYPE_COPY_SEND;
    message.port_descriptor.type = MACH_MSG_PORT_DESCRIPTOR;

    mach_msg_timeout_t const send_timeout = 100;
    auto const send_result = mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, message.header.msgh_size, 0, MACH_PORT_NULL, send_timeout, MACH_PORT_NULL);
    if (send_result != KERN_SUCCESS)
        return Core::mach_error_to_error(send_result);

    Core::Platform::ReceivedIPCChannelPortsMessage reply {};
    mach_msg_timeout_t const reply_timeout = 5000;
    auto const recv_result = mach_msg(&reply.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(reply),
        reply_port.port(), reply_timeout, MACH_PORT_NULL);
    if (recv_result != KERN_SUCCESS)
        return Core::mach_error_to_error(recv_result);

    VERIFY(reply.header.msgh_id == Core::Platform::IPC_CHANNEL_PORTS_MESSAGE_ID);
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

ErrorOr<TransportBootstrapMachPorts> bootstrap_transport_over_socket(Core::LocalSocket& socket)
{
    auto our_receive_right = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto our_name = ByteString::formatted("org.ladybird.ipc.{}.{}", Core::System::getpid(), our_receive_right.port());
    TRY(our_receive_right.register_with_bootstrap_server(our_name));

    auto socket_fd = socket.fd().value();
    TRY(socket.set_blocking(true));

    auto const name_bytes = our_name.bytes();
    u32 const name_length = static_cast<u32>(name_bytes.size());
    TRY(write_exact(socket_fd, ReadonlyBytes { reinterpret_cast<u8 const*>(&name_length), sizeof(name_length) }));
    TRY(write_exact(socket_fd, name_bytes));

    u32 peer_name_length = 0;
    TRY(read_exact(socket_fd, Bytes { reinterpret_cast<u8*>(&peer_name_length), sizeof(peer_name_length) }));
    VERIFY(peer_name_length > 0);
    VERIFY(peer_name_length <= 256);

    auto peer_name_buffer = TRY(ByteBuffer::create_uninitialized(peer_name_length));
    TRY(read_exact(socket_fd, peer_name_buffer.bytes()));

    auto peer_name = ByteString { peer_name_buffer.bytes() };
    auto peer_send_right = TRY(Core::MachPort::look_up_from_bootstrap_server(peer_name));

    u8 ack = 1;
    TRY(write_exact(socket_fd, { &ack, 1 }));
    TRY(read_exact(socket_fd, { &ack, 1 }));
    VERIFY(ack == 1);

    return TransportBootstrapMachPorts {
        .receive_right = move(our_receive_right),
        .send_right = move(peer_send_right),
    };
}

void TransportBootstrapMachServer::send_transport_ports_to_child(Core::MachPort reply_port, TransportBootstrapMachPorts ports)
{
    Core::Platform::MessageWithIPCChannelPorts message {};
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
    message.header.msgh_size = sizeof(message);
    message.header.msgh_remote_port = reply_port.release();
    message.header.msgh_local_port = MACH_PORT_NULL;
    message.header.msgh_id = Core::Platform::IPC_CHANNEL_PORTS_MESSAGE_ID;
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

void TransportBootstrapMachServer::register_pending_transport(pid_t pid, TransportBootstrapMachPorts ports)
{
    m_pending_bootstrap_mutex.lock();
    auto pending = m_pending_bootstrap.take(pid);
    if (!pending.has_value()) {
        m_pending_bootstrap.set(pid, WaitingForPorts { move(ports) });
        m_pending_bootstrap_mutex.unlock();
        return;
    }

    m_pending_bootstrap_mutex.unlock();
    pending.release_value().visit(
        [&](WaitingForPorts&) {
            VERIFY_NOT_REACHED();
        },
        [&](WaitingForReplyPort& waiting) {
            send_transport_ports_to_child(move(waiting.reply_port), move(ports));
        });
}

void TransportBootstrapMachServer::register_reply_port(pid_t pid, Core::MachPort reply_port)
{
    m_pending_bootstrap_mutex.lock();
    auto pending = m_pending_bootstrap.take(pid);
    if (!pending.has_value()) {
        m_pending_bootstrap.set(pid, WaitingForReplyPort { move(reply_port) });
        m_pending_bootstrap_mutex.unlock();
        return;
    }

    m_pending_bootstrap_mutex.unlock();
    pending.release_value().visit(
        [&](WaitingForPorts& waiting) {
            send_transport_ports_to_child(move(reply_port), move(waiting.ports));
        },
        [&](WaitingForReplyPort&) {
            VERIFY_NOT_REACHED();
        });
}

}
