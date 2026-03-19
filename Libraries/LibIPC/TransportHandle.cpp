/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Socket.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>

namespace IPC {

#if defined(AK_OS_MACOS)

TransportHandle::TransportHandle(Core::MachPort receive_right, Core::MachPort send_right)
    : m_receive_right(move(receive_right))
    , m_send_right(move(send_right))
{
    VERIFY(MACH_PORT_VALID(m_receive_right.port()));
    VERIFY(MACH_PORT_VALID(m_send_right.port()));
}

ErrorOr<NonnullOwnPtr<Transport>> TransportHandle::create_transport() const
{
    VERIFY(MACH_PORT_VALID(m_receive_right.port()));
    VERIFY(MACH_PORT_VALID(m_send_right.port()));
    return make<Transport>(move(m_receive_right), move(m_send_right));
}

template<>
ErrorOr<void> encode(Encoder& encoder, TransportHandle const& handle)
{
    VERIFY(MACH_PORT_VALID(handle.m_receive_right.port()));
    VERIFY(MACH_PORT_VALID(handle.m_send_right.port()));
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
    auto socket = TRY(Core::LocalSocket::adopt_fd(m_file.take_fd()));
    TRY(socket->set_blocking(false));
    return make<Transport>(move(socket));
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
