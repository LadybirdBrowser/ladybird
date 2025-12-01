/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <AK/Types.h>
#include <LibCore/Notifier.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibCore/UDPServer.h>

#include <AK/Windows.h>

namespace Core {

UDPServer::UDPServer()
{
    m_fd = MUST(Core::System::socket(AF_INET, SOCK_DGRAM, 0));
    int option = 1;
    MUST(Core::System::ioctl(m_fd, FIONBIO, option));
    auto const ret = SetHandleInformation(to_handle(m_fd), HANDLE_FLAG_INHERIT, 0);
    VERIFY(ret != 0);
}

UDPServer::~UDPServer()
{
    MUST(Core::System::close(m_fd));
}

bool UDPServer::bind(IPv4Address const& address, u16 port)
{
    if (m_bound)
        return false;

    auto socket_address = SocketAddress(address, port);
    auto in = socket_address.to_sockaddr_in();
    auto bind_result = Core::System::bind(m_fd, (sockaddr const*)&in, sizeof(in));
    if (bind_result.is_error()) {
        perror("UDPServer::bind");
        return false;
    }

    m_bound = true;

    m_notifier = Notifier::construct(m_fd, Notifier::Type::Read);
    m_notifier->on_activation = [this] {
        if (on_ready_to_receive)
            on_ready_to_receive();
    };
    return true;
}

ErrorOr<ByteBuffer> UDPServer::receive(size_t size, sockaddr_in& in)
{
    auto buf = TRY(ByteBuffer::create_uninitialized(size));
    socklen_t in_len = sizeof(in);
    auto bytes_received = TRY(Core::System::recvfrom(m_fd, buf, 0, (sockaddr*)&in, &in_len));
    buf.resize(bytes_received);
    return buf;
}

ErrorOr<ByteBuffer> UDPServer::receive(size_t size)
{
    struct sockaddr_in saddr;
    return receive(size, saddr);
}

ErrorOr<size_t> UDPServer::send(ReadonlyBytes buffer, sockaddr_in const& to)
{
    socklen_t to_len = sizeof(to);
    return Core::System::sendto(m_fd, buffer, 0, (sockaddr const*)&to, to_len);
}

Optional<IPv4Address> UDPServer::local_address() const
{
    if (m_fd == -1)
        return {};

    sockaddr_in address;
    socklen_t len = sizeof(address);
    if (Core::System::getsockname(m_fd, (sockaddr*)&address, &len).is_error())
        return {};

    return IPv4Address(address.sin_addr.s_addr);
}

Optional<u16> UDPServer::local_port() const
{
    if (m_fd == -1)
        return {};

    sockaddr_in address;
    socklen_t len = sizeof(address);
    if (Core::System::getsockname(m_fd, (sockaddr*)&address, &len).is_error())
        return {};

    return ntohs(address.sin_port);
}

}
