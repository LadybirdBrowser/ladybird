/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <LibCore/Socket.h>

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
#    include <LibIPC/TransportMachPort.h>
#elif !defined(AK_OS_WINDOWS)
#    include <LibIPC/TransportSocket.h>
#else
#    include <LibIPC/TransportWindows.h>
#endif

namespace IPC {

// Has the transports that this thread creates during the lifetime of this share one IO thread, where the platform can.
class ScopedSharedIOThread {
    AK_MAKE_NONCOPYABLE(ScopedSharedIOThread);
    AK_MAKE_NONMOVABLE(ScopedSharedIOThread);

public:
#if !defined(AK_OS_MACOS) && !defined(AK_OS_IOS) && !defined(AK_OS_WINDOWS)
    ScopedSharedIOThread() { TransportSocket::set_share_io_thread_on_this_thread(true); }
    ~ScopedSharedIOThread() { TransportSocket::set_share_io_thread_on_this_thread(false); }
#else
    ScopedSharedIOThread() = default;
    // NB: User-provided, so that clang does not see a scope guard as an unused variable.
    ~ScopedSharedIOThread() { }
#endif
};

}
