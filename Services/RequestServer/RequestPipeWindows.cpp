/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullRefPtr.h>
#include <LibCore/System.h>
#include <RequestServer/RequestPipe.h>

#include <AK/Windows.h>

namespace RequestServer {

RequestPipe::~RequestPipe()
{
    MUST(Core::System::close(m_writer_fd));
}

ErrorOr<NonnullRefPtr<RequestPipe>> RequestPipe::try_create()
{
    int socket_fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds));
    int option = 1;
    TRY(Core::System::ioctl(socket_fds[0], FIONBIO, option));
    TRY(Core::System::ioctl(socket_fds[1], FIONBIO, option));
    return adopt_nonnull_ref_or_enomem(new (nothrow) RequestPipe(socket_fds[0], socket_fds[1]));
}

ErrorOr<ssize_t> RequestPipe::write(Vector<u8> const& bytes)
{
    auto sent = ::send(m_writer_fd, reinterpret_cast<char const*>(bytes.data()), bytes.size(), 0);
    if (sent == SOCKET_ERROR)
        return Error::from_windows_error();
    return sent;
}

}
