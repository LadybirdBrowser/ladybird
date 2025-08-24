/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullRefPtr.h>
#include <LibCore/System.h>
#include <RequestServer/RequestPipe.h>

namespace RequestServer {

RequestPipe::~RequestPipe()
{
    MUST(Core::System::close(m_writer_fd));
}

ErrorOr<NonnullRefPtr<RequestPipe>> RequestPipe::try_create()
{
    auto fds = TRY(Core::System::pipe2(O_NONBLOCK));
    return adopt_nonnull_ref_or_enomem(new (nothrow) RequestPipe(fds[0], fds[1]));
}

ErrorOr<ssize_t> RequestPipe::write(Vector<u8> const& bytes)
{
    return Core::System::write(m_writer_fd, bytes);
}

}
