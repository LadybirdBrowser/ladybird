/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#if !defined(AK_OS_WINDOWS)
#    include <LibIPC/TransportSocket.h>
#else
#    include <LibIPC/TransportSocketWindows.h>
#endif

namespace IPC {

#if !defined(AK_OS_WINDOWS)
// Unix Domain Sockets
using Transport = TransportSocket;
#else
using Transport = TransportSocketWindows;
#endif

}
