/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Alexander Narsudinov <a.narsudinov@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <AK/Types.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibCore/UDPServer.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#ifndef SOCK_NONBLOCK
#    include <fcntl.h>
#    include <sys/ioctl.h>
#endif

namespace Core {

UDPServer::UDPServer(EventReceiver* parent)
    : EventReceiver(parent)
{
#ifdef SOCK_NONBLOCK
    m_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#else
    m_fd = socket(AF_INET, SOCK_DGRAM, 0);
    int option = 1;
    ioctl(m_fd, FIONBIO, &option);
    fcntl(m_fd, F_SETFD, FD_CLOEXEC);
#endif
    VERIFY(m_fd >= 0);
}

UDPServer::~UDPServer()
{
    ::close(m_fd);
}

bool UDPServer::bind(IPv4Address const& address, u16 port)
{
    if (m_bound)
        return false;

    auto saddr = SocketAddress(address, port);
    auto in = saddr.to_sockaddr_in();

    if (::bind(m_fd, (sockaddr const*)&in, sizeof(in)) != 0) {
        perror("UDPServer::bind");
        return false;
    }

    m_bound = true;

    m_notifier = Notifier::construct(m_fd, Notifier::Type::Read, this);
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
    auto bytes_received = TRY(Core::System::recvfrom(m_fd, buf.data(), size, 0, (sockaddr*)&in, &in_len));
    buf.resize(bytes_received);
    return buf;
}

Optional<IPv4Address> UDPServer::local_address() const
{
    if (m_fd == -1)
        return {};

    sockaddr_in address;
    socklen_t len = sizeof(address);
    if (getsockname(m_fd, (sockaddr*)&address, &len) != 0)
        return {};

    return IPv4Address(address.sin_addr.s_addr);
}

Optional<u16> UDPServer::local_port() const
{
    if (m_fd == -1)
        return {};

    sockaddr_in address;
    socklen_t len = sizeof(address);
    if (getsockname(m_fd, (sockaddr*)&address, &len) != 0)
        return {};

    return ntohs(address.sin_port);
}

ErrorOr<size_t> UDPServer::send(ReadonlyBytes buffer, sockaddr_in const& to)
{
    if (m_fd < 0) {
        return Error::from_errno(EBADF);
    }

    auto result = ::sendto(m_fd, buffer.data(), buffer.size(), 0, (sockaddr const*)&to, sizeof(to));
    if (result < 0) {
        return Error::from_errno(errno);
    }

    return result;
}

}
