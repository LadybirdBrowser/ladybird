/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>

#if !defined(AK_OS_WINDOWS)
#    include <sys/socket.h>
#endif

namespace IPC {

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)

TransportHandle::TransportHandle(Core::MachPort receive_right, Core::MachPort send_right)
    : m_receive_right(move(receive_right))
    , m_send_right(move(send_right))
{
    VERIFY(MACH_PORT_VALID(m_receive_right.port()));
    // NB: The send right is MACH_PORT_DEAD if the peer closed its end while this handle was in flight. The transport
    // built from it has nobody to send to — but it still reads whatever the peer sent before closing, and then the
    // no-senders notification reports the peer gone. WebKit's Connection::receiveSourceEventHandler does the same for
    // a connection whose send right arrives dead.
    VERIFY(m_send_right.port() != MACH_PORT_NULL);
}

ErrorOr<NonnullOwnPtr<Transport>> TransportHandle::create_transport() const
{
    VERIFY(MACH_PORT_VALID(m_receive_right.port()));
    VERIFY(m_send_right.port() != MACH_PORT_NULL);
    return make<Transport>(move(m_receive_right), move(m_send_right));
}

TransportHandle::MachRights TransportHandle::release_mach_rights()
{
    VERIFY(MACH_PORT_VALID(m_receive_right.port()));
    VERIFY(m_send_right.port() != MACH_PORT_NULL);
    return { move(m_receive_right), move(m_send_right) };
}

ErrorOr<TransportHandlePair> TransportHandle::create_pair()
{
    auto port_a_receive_right = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_a_send_right = TRY(port_a_receive_right.insert_right(Core::MachPort::MessageRight::MakeSend));
    auto port_b_receive_right = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_b_send_right = TRY(port_b_receive_right.insert_right(Core::MachPort::MessageRight::MakeSend));

    // Either end may be written to before the other has constructed its transport.
    TransportMachPort::raise_receive_queue_limit(port_a_receive_right);
    TransportMachPort::raise_receive_queue_limit(port_b_receive_right);

    return TransportHandlePair {
        TransportHandle { move(port_a_receive_right), move(port_b_send_right) },
        TransportHandle { move(port_b_receive_right), move(port_a_send_right) },
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, TransportHandle const& handle)
{
    mach_port_t send_port = handle.m_send_right.port();
    mach_port_t receive_port = handle.m_receive_right.port();
    // NB: A dead send right is fine to pass along; the next owner finds the peer gone, same as this one did.
    if (send_port == MACH_PORT_NULL || receive_port == MACH_PORT_NULL || receive_port == MACH_PORT_DEAD)
        return Error::from_string_literal("TransportHandle::encode: Invalid Mach port(s)");

    TRY(encoder.append_attachment(Attachment::from_mach_port(move(handle.m_receive_right), Core::MachPort::MessageRight::MoveReceive)));
    TRY(encoder.append_attachment(Attachment::from_mach_port(move(handle.m_send_right), Core::MachPort::MessageRight::MoveSend)));
    return {};
}

template<>
ErrorOr<TransportHandle> decode(Decoder& decoder)
{
    auto& attachments = decoder.attachments();
    VERIFY(attachments.size() >= 2);
    auto recv_attachment = attachments.dequeue();
    auto send_attachment = attachments.dequeue();
    VERIFY(recv_attachment.message_right() == Core::MachPort::MessageRight::MoveReceive);
    VERIFY(send_attachment.message_right() == Core::MachPort::MessageRight::MoveSend);
    auto receive_right = recv_attachment.release_mach_port();
    auto send_right = send_attachment.release_mach_port();
    return TransportHandle { move(receive_right), move(send_right) };
}

#else

TransportHandle::TransportHandle(File file)
    : m_file(move(file))
{
}

ErrorOr<NonnullOwnPtr<Transport>> TransportHandle::create_transport() const
{
    return Transport::adopt_fd(m_file.take_fd());
}

int TransportHandle::release_fd()
{
    return m_file.take_fd();
}

ErrorOr<TransportHandlePair> TransportHandle::create_pair()
{
#    if defined(AK_OS_WINDOWS)
    auto handles = TRY(Core::System::duplex_pipe());
    return TransportHandlePair { TransportHandle { File::adopt_fd(handles[0]) }, TransportHandle { File::adopt_fd(handles[1]) } };
#    else
    int fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));
    return TransportHandlePair { TransportHandle { File::adopt_fd(fds[0]) }, TransportHandle { File::adopt_fd(fds[1]) } };
#    endif
}

template<>
ErrorOr<void> encode(Encoder& encoder, TransportHandle const& handle)
{
    return encoder.encode(handle.m_file);
}

template<>
ErrorOr<TransportHandle> decode(Decoder& decoder)
{
    auto file = TRY(decoder.decode<File>());
    return TransportHandle { move(file) };
}

#endif

}
