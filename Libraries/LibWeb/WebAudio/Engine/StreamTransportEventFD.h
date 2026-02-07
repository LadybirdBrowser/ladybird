/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Types.h>
#include <LibCore/System.h>
#include <errno.h>

#if defined(AK_OS_LINUX)
#    include <sys/eventfd.h>
#endif

namespace Web::WebAudio::Render {

struct StreamNotifyFds {
    int read_fd { -1 };
    int write_fd { -1 };
};

// Create a nonblocking stream notification channel.
inline ErrorOr<StreamNotifyFds> create_nonblocking_stream_notify_fds()
{
#if defined(AK_OS_LINUX)
    int write_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (write_fd < 0)
        return Error::from_errno(errno);

    int read_fd = TRY(Core::System::dup(write_fd));
    TRY(Core::System::set_close_on_exec(read_fd, true));

    return StreamNotifyFds { .read_fd = read_fd, .write_fd = write_fd };
#else
    auto fds = TRY(Core::System::pipe2(O_CLOEXEC | O_NONBLOCK));
    auto read_fd = fds[0];
    auto write_fd = fds[1];

    return StreamNotifyFds { .read_fd = read_fd, .write_fd = write_fd };
#endif
}

}
