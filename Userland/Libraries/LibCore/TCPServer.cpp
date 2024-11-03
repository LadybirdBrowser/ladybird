/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <AK/Types.h>
#include <LibCore/Notifier.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibCore/TCPServer.h>

namespace Core {

ErrorOr<NonnullRefPtr<TCPServer>> TCPServer::try_create(EventReceiver* parent)
{
#ifdef SOCK_NONBLOCK
    int fd = TRY(Core::System::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
#else
    int fd = TRY(Core::System::socket(AF_INET, SOCK_STREAM, 0));
    int option = 1;
    TRY(Core::System::ioctl(fd, FIONBIO, &option));
    TRY(Core::System::fcntl(fd, F_SETFD, FD_CLOEXEC));
#endif

    return adopt_nonnull_ref_or_enomem(new (nothrow) TCPServer(fd, parent));
}

TCPServer::TCPServer(int fd, EventReceiver* parent)
    : EventReceiver(parent)
    , m_fd(fd)
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

    m_notifier = Notifier::construct(m_fd, Notifier::Type::Read, this);
    m_notifier->on_activation = [this] {
        if (on_ready_to_accept)
            on_ready_to_accept();
    };
    return {};
}

ErrorOr<void> TCPServer::set_blocking(bool blocking)
{
    int flags = TRY(Core::System::fcntl(m_fd, F_GETFL, 0));
    if (blocking)
        TRY(Core::System::fcntl(m_fd, F_SETFL, flags & ~O_NONBLOCK));
    else
        TRY(Core::System::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK));
    return {};
}

ErrorOr<NonnullOwnPtr<TCPSocket>> TCPServer::accept()
{
    VERIFY(m_listening);
    sockaddr_in in;
    socklen_t in_size = sizeof(in);
#if !defined(AK_OS_MACOS) && !defined(AK_OS_IOS) && !defined(AK_OS_HAIKU)
    int accepted_fd = TRY(Core::System::accept4(m_fd, (sockaddr*)&in, &in_size, SOCK_NONBLOCK | SOCK_CLOEXEC));
#else
    int accepted_fd = TRY(Core::System::accept(m_fd, (sockaddr*)&in, &in_size));
#endif

    auto socket = TRY(TCPSocket::adopt_fd(accepted_fd));

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS) || defined(AK_OS_HAIKU)
    // FIXME: Ideally, we should let the caller decide whether it wants the
    //        socket to be nonblocking or not, but there are currently places
    //        which depend on this.
    TRY(socket->set_blocking(false));
    TRY(socket->set_close_on_exec(true));
#endif

    return socket;
}

Optional<IPv4Address> TCPServer::local_address() const
{
    if (m_fd == -1)
        return {};

    sockaddr_in address;
    socklen_t len = sizeof(address);
    if (getsockname(m_fd, (sockaddr*)&address, &len) != 0)
        return {};

    return IPv4Address(address.sin_addr.s_addr);
}

Optional<u16> TCPServer::local_port() const
{
    if (m_fd == -1)
        return {};

    sockaddr_in address;
    socklen_t len = sizeof(address);
    if (getsockname(m_fd, (sockaddr*)&address, &len) != 0)
        return {};

    return ntohs(address.sin_port);
}

}
