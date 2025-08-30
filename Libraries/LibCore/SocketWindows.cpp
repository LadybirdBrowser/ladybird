/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Socket.h>
#include <LibCore/System.h>

#include <AK/Windows.h>

#define MSG_DONTWAIT 0x40

namespace Core {

static WSABUF make_wsa_buf(ReadonlyBytes bytes)
{
    WSABUF buffer {};
    buffer.buf = reinterpret_cast<CHAR*>(const_cast<u8*>(bytes.data()));
    buffer.len = static_cast<ULONG>(bytes.size());
    return buffer;
}

ErrorOr<Bytes> PosixSocketHelper::read(Bytes buffer, int flags)
{
    if (!is_open())
        return Error::from_errno(ENOTCONN);

    // FIXME: also take into account if PosixSocketHelper is blocking/non-blocking (see set_blocking)
    bool non_blocking = flags & MSG_DONTWAIT;

    if (non_blocking && !TRY(can_read_without_blocking(0)))
        return Error::from_errno(EWOULDBLOCK);

    WSABUF buf = make_wsa_buf(buffer);
    DWORD nread = 0;
    DWORD fl = 0;

    if (WSARecv(m_fd, &buf, 1, &nread, &fl, NULL, NULL) == SOCKET_ERROR) {
        if (GetLastError() == WSAECONNRESET)
            return Error::from_errno(ECONNRESET);
        return Error::from_windows_error();
    }

    if (nread == 0)
        did_reach_eof_on_read();

    return buffer.trim(nread);
}

void PosixSocketHelper::did_reach_eof_on_read()
{
    m_last_read_was_eof = true;

    // If a socket read is EOF, then no more data can be read from it because
    // the protocol has disconnected. In this case, we can just disable the
    // notifier if we have one.
    if (m_notifier)
        m_notifier->set_enabled(false);
}

ErrorOr<size_t> PosixSocketHelper::write(ReadonlyBytes buffer, int flags)
{
    if (!is_open())
        return Error::from_errno(ENOTCONN);

    // FIXME: Implement non-blocking PosixSocketHelper::write
    (void)flags;
    WSABUF buf = make_wsa_buf(buffer);
    DWORD nwritten = 0;

    if (WSASend(m_fd, &buf, 1, &nwritten, 0, NULL, NULL) == SOCKET_ERROR)
        return Error::from_windows_error();

    return nwritten;
}

ErrorOr<bool> PosixSocketHelper::can_read_without_blocking(int timeout) const
{
    struct pollfd pollfd = {
        .fd = static_cast<SOCKET>(m_fd),
        .events = POLLIN,
        .revents = 0
    };

    auto result = WSAPoll(&pollfd, 1, timeout);
    if (result == SOCKET_ERROR)
        return Error::from_windows_error();
    return result;
}

ErrorOr<void> PosixSocketHelper::set_blocking(bool)
{
    // FIXME: Implement Core::PosixSocketHelper::set_blocking
    // Currently does nothing, sockets are always blocking.
    return {};
}

ErrorOr<void> PosixSocketHelper::set_close_on_exec(bool enabled)
{
    return System::set_close_on_exec(m_fd, enabled);
}

ErrorOr<void> PosixSocketHelper::set_receive_timeout(AK::Duration timeout)
{
    auto timeout_spec = timeout.to_timespec();
    return System::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout_spec, sizeof(timeout_spec));
}

ErrorOr<size_t> PosixSocketHelper::pending_bytes() const
{
    if (!is_open()) {
        return Error::from_windows_error(WSAENOTCONN);
    }

    u_long value = 0;
    TRY(System::ioctl(m_fd, FIONREAD, &value));
    return value;
}

void PosixSocketHelper::setup_notifier()
{
    if (!m_notifier)
        m_notifier = Notifier::construct(m_fd, Notifier::Type::Read);
}

void PosixSocketHelper::close()
{
    if (!is_open())
        return;

    if (m_notifier)
        m_notifier->set_enabled(false);

    // shutdown is required for another end to receive FD_CLOSE
    shutdown(m_fd, SD_BOTH);
    closesocket(m_fd);
    m_fd = -1;
}

ErrorOr<Bytes> LocalSocket::read_without_waiting(Bytes buffer)
{
    return m_helper.read(buffer, MSG_DONTWAIT);
}

ErrorOr<NonnullOwnPtr<LocalSocket>> LocalSocket::adopt_fd(int fd, PreventSIGPIPE prevent_sigpipe)
{
    if (fd == -1)
        return Error::from_errno(EBADF);

    auto socket = adopt_own(*new LocalSocket(prevent_sigpipe));
    socket->m_helper.set_fd(fd);
    socket->setup_notifier();
    return socket;
}

Optional<int> LocalSocket::fd() const
{
    if (!is_open())
        return {};
    return m_helper.fd();
}

ErrorOr<int> LocalSocket::release_fd()
{
    if (!is_open()) {
        return Error::from_errno(ENOTCONN);
    }

    auto fd = m_helper.fd();
    m_helper.set_fd(-1);
    return fd;
}

ErrorOr<NonnullOwnPtr<LocalSocket>> LocalSocket::connect(ByteString const& path, PreventSIGPIPE prevent_sigpipe)
{
    auto socket = TRY(adopt_nonnull_own_or_enomem(new (nothrow) LocalSocket(prevent_sigpipe)));

    auto fd = TRY(create_fd(SocketDomain::Local, SocketType::Stream));
    socket->m_helper.set_fd(fd);

    TRY(connect_local(fd, path));

    socket->setup_notifier();
    return socket;
}

ErrorOr<int> Socket::create_fd(SocketDomain domain, SocketType type)
{
    int socket_domain;
    switch (domain) {
    case SocketDomain::Inet:
        socket_domain = AF_INET;
        break;
    case SocketDomain::Inet6:
        socket_domain = AF_INET6;
        break;
    case SocketDomain::Local:
        socket_domain = AF_LOCAL;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    int socket_type;
    switch (type) {
    case SocketType::Stream:
        socket_type = SOCK_STREAM;
        break;
    case SocketType::Datagram:
        socket_type = SOCK_DGRAM;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    auto fd = TRY(System::socket(socket_domain, socket_type, 0));
    (void)System::set_close_on_exec(fd, true);
    return fd;
}

ErrorOr<Vector<Variant<IPv4Address, IPv6Address>>> Socket::resolve_host(ByteString const& host, SocketType type)
{
    int socket_type;
    switch (type) {
    case SocketType::Stream:
        socket_type = SOCK_STREAM;
        break;
    case SocketType::Datagram:
        socket_type = SOCK_DGRAM;
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socket_type;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;

    auto const results = TRY(System::getaddrinfo(host.characters(), nullptr, hints));

    Vector<Variant<IPv4Address, IPv6Address>> addresses;

    for (auto const& result : results.addresses()) {
        if (result.ai_family == AF_INET6) {
            auto* socket_address = bit_cast<struct sockaddr_in6*>(result.ai_addr);
            auto address = IPv6Address { socket_address->sin6_addr.s6_addr };
            addresses.append(address);
        }

        if (result.ai_family == AF_INET) {
            auto* socket_address = bit_cast<struct sockaddr_in*>(result.ai_addr);
            NetworkOrdered<u32> const network_ordered_address { socket_address->sin_addr.s_addr };
            addresses.append(IPv4Address { network_ordered_address });
        }
    }

    if (addresses.is_empty())
        return Error::from_string_literal("Could not resolve to IPv4 or IPv6 address");

    return addresses;
}

ErrorOr<void> Socket::connect_inet(int fd, SocketAddress const& address)
{
    if (address.type() == SocketAddress::Type::IPv6) {
        auto addr = address.to_sockaddr_in6();
        return System::connect(fd, bit_cast<struct sockaddr*>(&addr), sizeof(addr));
    } else {
        auto addr = address.to_sockaddr_in();
        return System::connect(fd, bit_cast<struct sockaddr*>(&addr), sizeof(addr));
    }
}

ErrorOr<void> Socket::connect_local(int fd, ByteString const& path)
{
    auto address = SocketAddress::local(path);
    auto maybe_sockaddr = address.to_sockaddr_un();
    if (!maybe_sockaddr.has_value()) {
        dbgln("Core::Socket::connect_local: Could not obtain a sockaddr_un");
        return Error::from_errno(EINVAL);
    }

    auto addr = maybe_sockaddr.release_value();
    return System::connect(fd, bit_cast<struct sockaddr*>(&addr), sizeof(addr));
}

ErrorOr<NonnullOwnPtr<UDPSocket>> UDPSocket::connect(SocketAddress const& address, Optional<AK::Duration> timeout)
{
    auto socket = adopt_own(*new UDPSocket);

    auto socket_domain = SocketDomain::Inet6;
    if (address.type() == SocketAddress::Type::IPv4)
        socket_domain = SocketDomain::Inet;

    auto fd = TRY(create_fd(socket_domain, SocketType::Datagram));
    socket->m_helper.set_fd(fd);
    if (timeout.has_value()) {
        TRY(socket->m_helper.set_receive_timeout(timeout.value()));
    }

    TRY(connect_inet(fd, address));

    socket->setup_notifier();
    return socket;
}

ErrorOr<Bytes> UDPSocket::read_some(Bytes buffer)
{
    auto pending_bytes = TRY(this->pending_bytes());
    if (pending_bytes > buffer.size()) {
        // With UDP datagrams, reading a datagram into a buffer that's
        // smaller than the datagram's size will cause the rest of the
        // datagram to be discarded. That's not very nice, so let's bail
        // early, telling the caller that he should allocate a bigger
        // buffer.
        return Error::from_errno(WSAEMSGSIZE);
    }

    return m_helper.read(buffer, default_flags());
}

ErrorOr<NonnullOwnPtr<TCPSocket>> TCPSocket::connect(ByteString const& host, u16 port)
{
    auto ip_addresses = TRY(resolve_host(host, SocketType::Stream));

    // It should return an error instead of an empty vector.
    VERIFY(!ip_addresses.is_empty());

    // FIXME: Support trying to connect to multiple IP addresses (e.g. if one of them doesn't seem to be working, try another one)
    return ip_addresses.first().visit([port](auto address) { return connect(SocketAddress { address, port }); });
}

ErrorOr<NonnullOwnPtr<TCPSocket>> TCPSocket::connect(SocketAddress const& address)
{
    auto socket = adopt_own(*new TCPSocket);

    auto socket_domain = SocketDomain::Inet6;
    if (address.type() == SocketAddress::Type::IPv4)
        socket_domain = SocketDomain::Inet;

    auto fd = TRY(create_fd(socket_domain, SocketType::Stream));
    socket->m_helper.set_fd(fd);

    TRY(connect_inet(fd, address));

    socket->setup_notifier();
    return socket;
}

ErrorOr<NonnullOwnPtr<TCPSocket>> TCPSocket::adopt_fd(int fd)
{
    if (static_cast<SOCKET>(fd) == INVALID_SOCKET)
        return Error::from_windows_error();

    auto socket = TRY(adopt_nonnull_own_or_enomem(new (nothrow) TCPSocket()));
    socket->m_helper.set_fd(fd);
    socket->setup_notifier();
    return socket;
}

}
