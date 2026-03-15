/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/SocketAddress.h>
#include <LibCore/System.h>
#include <RequestServer/RequestPipe.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#endif

namespace RequestServer {

RequestPipe::RequestPipe(int const reader_fd, int const writer_fd)
    : m_reader_fd(reader_fd)
    , m_writer_fd(writer_fd)
{
    VERIFY(m_reader_fd >= 0);
    VERIFY(m_writer_fd >= 0);
}

RequestPipe::RequestPipe(RequestPipe&& other)
    : m_reader_fd(exchange(other.m_reader_fd, -1))
    , m_writer_fd(exchange(other.m_writer_fd, -1))
{
}

RequestPipe& RequestPipe::operator=(RequestPipe&& other)
{
    m_reader_fd = exchange(other.m_reader_fd, -1);
    m_writer_fd = exchange(other.m_writer_fd, -1);
    return *this;
}

RequestPipe::~RequestPipe()
{
    if (m_writer_fd != -1)
        MUST(Core::System::close(m_writer_fd));
}

ErrorOr<RequestPipe> RequestPipe::create()
{
    int socket_fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds));
    int option = 1;
    TRY(Core::System::ioctl(socket_fds[0], FIONBIO, &option));
    TRY(Core::System::ioctl(socket_fds[1], FIONBIO, &option));

    // Increase socket buffer sizes from OS default (~8KB on macOS) to allow
    // larger writes/reads per syscall, significantly improving throughput for
    // large response bodies.
    static constexpr int buffer_size = 512 * KiB;
    (void)Core::System::setsockopt(socket_fds[0], SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
    (void)Core::System::setsockopt(socket_fds[1], SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));

    return RequestPipe(socket_fds[0], socket_fds[1]);
}

ErrorOr<size_t> RequestPipe::write(ReadonlyBytes bytes)
{
    return Core::System::send(m_writer_fd, bytes, MSG_NOSIGNAL);
}

}
