/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Socket.h>
#include <LibCore/System.h>

namespace Core {

// FIXME: This limit has been chosen arbitrarily
size_t const LocalSocket::MAX_TRANSFER_FDS = 64;

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

    // Let's have a safe default of CLOEXEC. :^)
#ifdef SOCK_CLOEXEC
    return System::socket(socket_domain, socket_type | SOCK_CLOEXEC, 0);
#else
    auto fd = TRY(System::socket(socket_domain, socket_type, 0));
    TRY(System::fcntl(fd, F_SETFD, FD_CLOEXEC));
    return fd;
#endif
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

    auto const results = TRY(Core::System::getaddrinfo(host.characters(), nullptr, hints));

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

ErrorOr<Bytes> PosixSocketHelper::read(Bytes buffer, int flags)
{
    if (!is_open()) {
        return Error::from_errno(ENOTCONN);
    }

    ssize_t nread = TRY(System::recv(m_fd, buffer.data(), buffer.size(), flags));
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
    if (!is_open()) {
        return Error::from_errno(ENOTCONN);
    }

    return TRY(System::send(m_fd, buffer.data(), buffer.size(), flags));
}

void PosixSocketHelper::close()
{
    if (!is_open()) {
        return;
    }

    if (m_notifier)
        m_notifier->set_enabled(false);

    ErrorOr<void> result;
    do {
        result = System::close(m_fd);
    } while (result.is_error() && result.error().code() == EINTR);

    VERIFY(!result.is_error());
    m_fd = -1;
}

ErrorOr<bool> PosixSocketHelper::can_read_without_blocking(int timeout) const
{
    struct pollfd the_fd = { .fd = m_fd, .events = POLLIN, .revents = 0 };

    ErrorOr<int> result { 0 };
    do {
        result = Core::System::poll({ &the_fd, 1 }, timeout);
    } while (result.is_error() && result.error().code() == EINTR);

    if (result.is_error())
        return result.release_error();

    return (the_fd.revents & POLLIN) > 0;
}

ErrorOr<void> PosixSocketHelper::set_blocking(bool enabled)
{
    int value = enabled ? 0 : 1;
    return System::ioctl(m_fd, FIONBIO, &value);
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

void PosixSocketHelper::setup_notifier()
{
    if (!m_notifier)
        m_notifier = Core::Notifier::construct(m_fd, Core::Notifier::Type::Read);
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
    auto socket = TRY(adopt_nonnull_own_or_enomem(new (nothrow) TCPSocket()));

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
    if (fd < 0) {
        return Error::from_errno(EBADF);
    }

    auto socket = TRY(adopt_nonnull_own_or_enomem(new (nothrow) TCPSocket()));
    socket->m_helper.set_fd(fd);
    socket->setup_notifier();
    return socket;
}

ErrorOr<size_t> PosixSocketHelper::pending_bytes() const
{
    if (!is_open()) {
        return Error::from_errno(ENOTCONN);
    }

    int value;
    TRY(System::ioctl(m_fd, FIONREAD, &value));
    return static_cast<size_t>(value);
}

ErrorOr<NonnullOwnPtr<UDPSocket>> UDPSocket::connect(ByteString const& host, u16 port, Optional<AK::Duration> timeout)
{
    auto ip_addresses = TRY(resolve_host(host, SocketType::Datagram));

    // It should return an error instead of an empty vector.
    VERIFY(!ip_addresses.is_empty());

    // FIXME: Support trying to connect to multiple IP addresses (e.g. if one of them doesn't seem to be working, try another one)
    return ip_addresses.first().visit([port, timeout](auto address) { return connect(SocketAddress { address, port }, timeout); });
}

ErrorOr<NonnullOwnPtr<UDPSocket>> UDPSocket::connect(SocketAddress const& address, Optional<AK::Duration> timeout)
{
    auto socket = TRY(adopt_nonnull_own_or_enomem(new (nothrow) UDPSocket()));

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

ErrorOr<NonnullOwnPtr<LocalSocket>> LocalSocket::connect(ByteString const& path, PreventSIGPIPE prevent_sigpipe)
{
    auto socket = TRY(adopt_nonnull_own_or_enomem(new (nothrow) LocalSocket(prevent_sigpipe)));

    auto fd = TRY(create_fd(SocketDomain::Local, SocketType::Stream));
    socket->m_helper.set_fd(fd);

    TRY(connect_local(fd, path));

    socket->setup_notifier();
    return socket;
}

ErrorOr<NonnullOwnPtr<LocalSocket>> LocalSocket::adopt_fd(int fd, PreventSIGPIPE prevent_sigpipe)
{
    if (fd < 0) {
        return Error::from_errno(EBADF);
    }

    auto socket = TRY(adopt_nonnull_own_or_enomem(new (nothrow) LocalSocket(prevent_sigpipe)));
    socket->m_helper.set_fd(fd);
    socket->setup_notifier();
    return socket;
}

ErrorOr<int> LocalSocket::receive_fd(int flags)
{
#if defined(AK_OS_SERENITY)
    return Core::System::recvfd(m_helper.fd(), flags);
#elif defined(AK_OS_LINUX) || defined(AK_OS_GNU_HURD) || defined(AK_OS_BSD_GENERIC) || defined(AK_OS_HAIKU)
    union {
        struct cmsghdr cmsghdr;
        char control[CMSG_SPACE(sizeof(int))];
    } cmsgu {};
    char c = 0;
    struct iovec iov {
        .iov_base = &c,
        .iov_len = 1,
    };
    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgu.control;
    msg.msg_controllen = sizeof(cmsgu.control);
    TRY(Core::System::recvmsg(m_helper.fd(), &msg, 0));

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
        return Error::from_string_literal("Malformed message when receiving file descriptor");

    VERIFY(cmsg->cmsg_level == SOL_SOCKET);
    VERIFY(cmsg->cmsg_type == SCM_RIGHTS);
    int fd = *((int*)CMSG_DATA(cmsg));

    if (flags & O_CLOEXEC) {
        auto fd_flags = TRY(Core::System::fcntl(fd, F_GETFD));
        TRY(Core::System::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC));
    }

    return fd;
#else
    (void)flags;
    return Error::from_string_literal("File descriptor passing not supported on this platform");
#endif
}

ErrorOr<void> LocalSocket::send_fd(int fd)
{
#if defined(AK_OS_SERENITY)
    return Core::System::sendfd(m_helper.fd(), fd);
#elif defined(AK_OS_LINUX) || defined(AK_OS_GNU_HURD) || defined(AK_OS_BSD_GENERIC) || defined(AK_OS_HAIKU)
    char c = 'F';
    struct iovec iov {
        .iov_base = &c,
        .iov_len = sizeof(c)
    };

    union {
        struct cmsghdr cmsghdr;
        char control[CMSG_SPACE(sizeof(int))];
    } cmsgu {};

    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgu.control;
    msg.msg_controllen = sizeof(cmsgu.control);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;

    *((int*)CMSG_DATA(cmsg)) = fd;

    TRY(Core::System::sendmsg(m_helper.fd(), &msg, 0));
    return {};
#else
    (void)fd;
    return Error::from_string_literal("File descriptor passing not supported on this platform");
#endif
}

ErrorOr<ssize_t> LocalSocket::send_message(ReadonlyBytes data, int flags, Vector<int, 1> fds)
{
    size_t const num_fds = fds.size();
    if (num_fds == 0)
        return m_helper.write(data, flags | default_flags());
    if (num_fds > MAX_TRANSFER_FDS)
        return Error::from_string_literal("Too many file descriptors to send");

    auto const fd_payload_size = num_fds * sizeof(int);

    alignas(struct cmsghdr) char control_buf[CMSG_SPACE(sizeof(int) * MAX_TRANSFER_FDS)] {};

    // Note: We don't use designated initializers here due to weirdness with glibc's flexible array members.
    auto* header = new (control_buf) cmsghdr {};
    header->cmsg_len = static_cast<socklen_t>(CMSG_LEN(fd_payload_size));
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(header), fds.data(), fd_payload_size);

    struct iovec iov {
        .iov_base = const_cast<u8*>(data.data()),
        .iov_len = data.size(),
    };
    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = header;
    msg.msg_controllen = CMSG_LEN(fd_payload_size);

    return TRY(Core::System::sendmsg(m_helper.fd(), &msg, default_flags() | flags));
}

ErrorOr<Bytes> LocalSocket::receive_message(AK::Bytes buffer, int flags, Vector<int>& fds)
{
    struct iovec iov {
        .iov_base = buffer.data(),
        .iov_len = buffer.size(),
    };

    alignas(struct cmsghdr) char control_buf[CMSG_SPACE(sizeof(int) * MAX_TRANSFER_FDS)] {};

    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control_buf;
    msg.msg_controllen = sizeof(control_buf);

    auto nread = TRY(Core::System::recvmsg(m_helper.fd(), &msg, default_flags() | flags));
    if (nread == 0) {
        m_helper.did_reach_eof_on_read();
        return buffer.trim(nread);
    }

    fds.clear();

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    while (cmsg != nullptr) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            size_t num_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            auto* fd_data = reinterpret_cast<int*>(CMSG_DATA(cmsg));
            for (size_t i = 0; i < num_fds; ++i) {
                fds.append(fd_data[i]);
            }
        }
        AK_IGNORE_DIAGNOSTIC("-Wsign-compare", cmsg = CMSG_NXTHDR(&msg, cmsg));
    }
    return buffer.trim(nread);
}

ErrorOr<pid_t> LocalSocket::peer_pid() const
{
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    pid_t pid;
    socklen_t pid_size = sizeof(pid);
#elif defined(AK_OS_FREEBSD)
    struct xucred creds = {};
    socklen_t creds_size = sizeof(creds);
#elif defined(AK_OS_OPENBSD)
    struct sockpeercred creds = {};
    socklen_t creds_size = sizeof(creds);
#elif defined(AK_OS_NETBSD)
    struct sockcred creds = {};
    socklen_t creds_size = sizeof(creds);
#elif defined(AK_OS_SOLARIS)
    ucred_t* creds = NULL;
    socklen_t creds_size = sizeof(creds);
#elif defined(AK_OS_GNU_HURD)
    return Error::from_errno(ENOTSUP);
#else
    struct ucred creds = {};
    socklen_t creds_size = sizeof(creds);
#endif

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    TRY(System::getsockopt(m_helper.fd(), SOL_LOCAL, LOCAL_PEERPID, &pid, &pid_size));
    return pid;
#elif defined(AK_OS_FREEBSD)
    TRY(System::getsockopt(m_helper.fd(), SOL_LOCAL, LOCAL_PEERCRED, &creds, &creds_size));
    return creds.cr_pid;
#elif defined(AK_OS_NETBSD)
    TRY(System::getsockopt(m_helper.fd(), SOL_SOCKET, SCM_CREDS, &creds, &creds_size));
    return creds.sc_pid;
#elif defined(AK_OS_SOLARIS)
    TRY(System::getsockopt(m_helper.fd(), SOL_SOCKET, SO_RECVUCRED, &creds, &creds_size));
    return ucred_getpid(creds);
#elif !defined(AK_OS_GNU_HURD)
    TRY(System::getsockopt(m_helper.fd(), SOL_SOCKET, SO_PEERCRED, &creds, &creds_size));
    return creds.pid;
#endif
}

ErrorOr<Bytes> LocalSocket::read_without_waiting(Bytes buffer)
{
    return m_helper.read(buffer, MSG_DONTWAIT);
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

}
