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
#include <LibCore/TCPServer.h>

#include <AK/Windows.h>

namespace Core {

ErrorOr<NonnullRefPtr<TCPServer>> TCPServer::try_create()
{
    int fd = TRY(Core::System::socket(AF_INET, SOCK_STREAM, 0));
    ArmedScopeGuard close_fd { [fd]() {
        MUST(Core::System::close(fd));
    } };

    int option = 1;
    TRY(Core::System::ioctl(fd, FIONBIO, &option));
    TRY(Core::System::setsockopt(fd, SOL_SOCKET, SO_OOBINLINE, &option, sizeof(option)));
    if (SetHandleInformation(to_handle(fd), HANDLE_FLAG_INHERIT, 0) == 0)
        return Error::from_windows_error();
    close_fd.disarm();
    return adopt_nonnull_ref_or_enomem(new (nothrow) TCPServer(fd));
}

TCPServer::TCPServer(int fd)
    : m_fd(fd)
{
    VERIFY(m_fd >= 0);
}

TCPServer::~TCPServer()
{
    MUST(Core::System::close(m_fd));
}

ErrorOr<void> TCPServer::listen(IPv4Address const& address, u16 port, AllowAddressReuse allow_address_reuse)
{
    if (m_listening)
        return Error::from_errno(EADDRINUSE);

    auto socket_address = SocketAddress(address, port);
    auto in = socket_address.to_sockaddr_in();

    if (allow_address_reuse == AllowAddressReuse::Yes) {
        int option = 1;
        TRY(Core::System::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)));
    }

    TRY(Core::System::bind(m_fd, (sockaddr const*)&in, sizeof(in)));
    TRY(Core::System::listen(m_fd, 5));
    m_listening = true;

    m_notifier = Notifier::construct(m_fd, Notifier::Type::Read);
    m_notifier->on_activation = [this] {
        if (on_ready_to_accept)
            on_ready_to_accept();
    };
    return {};
}

ErrorOr<void> TCPServer::set_blocking(bool const blocking)
{
    // NOTE: Blocking does not seem to be supported. Error code returned is WSAEINVAL
    if (!blocking)
        return Error::from_string_literal("Core::TCPServer: WinSock2 does not support blocking");
    int option = 1;
    TRY(Core::System::ioctl(m_fd, FIONBIO, &option));
    return {};
}

ErrorOr<NonnullOwnPtr<TCPSocket>> TCPServer::accept()
{
    VERIFY(m_listening);
    sockaddr_in in;
    socklen_t in_size = sizeof(in);
    int accepted_fd = TRY(Core::System::accept(m_fd, (sockaddr*)&in, &in_size));
    return TRY(TCPSocket::adopt_fd(accepted_fd));
}

Optional<IPv4Address> TCPServer::local_address() const
{
    if (m_fd == -1)
        return {};

    sockaddr_in address;
    socklen_t len = sizeof(address);
    if (Core::System::getsockname(m_fd, (sockaddr*)&address, &len).is_error())
        return {};

    return IPv4Address(address.sin_addr.s_addr);
}

Optional<u16> TCPServer::local_port() const
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
