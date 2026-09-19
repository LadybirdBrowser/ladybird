/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <LibWebView/Forward.h>
#include <sys/types.h>

namespace WebView {

// Starts the ProcessReaper helper, which kills the processes that it is told about once this process is gone. The
// kernel does that for us on Linux, but not on macOS.
class WEBVIEW_API ProcessReaper {
    AK_MAKE_NONCOPYABLE(ProcessReaper);
    AK_MAKE_NONMOVABLE(ProcessReaper);

public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<ProcessReaper>> start(ByteString const& executable_path);
    ~ProcessReaper();

    ErrorOr<void> watch(pid_t);

    pid_t pid() const { return m_pid; }

private:
    ProcessReaper(int pipe_fd, pid_t pid);

    int m_pipe_fd { -1 };
    pid_t m_pid { -1 };
};

}
