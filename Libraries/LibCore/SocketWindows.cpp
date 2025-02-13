/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
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
    bool blocking = !(flags & MSG_DONTWAIT);
    WSABUF buf = make_wsa_buf(buffer);
    DWORD nread = 0;
    DWORD fl = 0;
    OVERLAPPED ov = {};

    if (WSARecv(m_fd, &buf, 1, &nread, &fl, blocking ? NULL : &ov, NULL) == SOCKET_ERROR) {

        auto error = GetLastError();

        if (error == WSA_IO_PENDING) {
            CancelIo(to_handle(m_fd));
            return Error::from_errno(EWOULDBLOCK);
        }

        if (error == WSAECONNRESET)
            return Error::from_errno(ECONNRESET);

        return Error::from_windows_error(error);
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

ErrorOr<size_t> PosixSocketHelper::pending_bytes() const
{
    VERIFY(0 && "Core::PosixSocketHelper::pending_bytes is not implemented");
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
    MUST(System::close(m_fd));
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

}
