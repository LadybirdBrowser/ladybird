/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>
#include <LibCore/Socket.h>

#if defined(AK_OS_MACOS)
#    include <LibIPC/TransportMachPort.h>
#elif !defined(AK_OS_WINDOWS)
#    include <LibIPC/TransportSocket.h>
#else
#    include <LibIPC/TransportSocketWindows.h>
#endif

namespace IPC {

}
