/*
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// This header should be included only in cpp files.
// It should be included after all other files and should be separated from them by a non-#include line to prevent clang-format from changing header order.

#pragma once

#include <AK/Assertions.h>
#include <AK/Platform.h>

#ifdef AK_OS_WINDOWS // needed for Swift
#    define timeval dummy_timeval
#    include <winsock2.h>
#    undef timeval
#    undef IN
#    pragma comment(lib, "ws2_32.lib")
#    include <io.h>
#    include <stdlib.h>

inline void initiate_wsa()
{
    WSADATA wsa;
    WORD version = MAKEWORD(2, 2);
    int rc = WSAStartup(version, &wsa);
    VERIFY(rc == 0 && wsa.wVersion == version);
}

inline void terminate_wsa()
{
    int rc = WSACleanup();
    VERIFY(rc == 0);
}

static void invalid_parameter_handler(wchar_t const*, wchar_t const*, wchar_t const*, unsigned int, uintptr_t)
{
}

inline void override_crt_invalid_parameter_handler()
{
    // Make _get_osfhandle return -1 instead of crashing on invalid fd in release (debug still __debugbreak's)
    _set_invalid_parameter_handler(invalid_parameter_handler);
}

inline void windows_init()
{
    initiate_wsa();
    override_crt_invalid_parameter_handler();
}

inline void windows_shutdown()
{
    terminate_wsa();
}
#endif
