/*
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// This header should be included only in cpp files.
// It should be included after all other files and should be separated from them by a non-#include line to prevent clang-format from changing header order.

#pragma once

#include <AK/Platform.h>

#ifdef AK_OS_WINDOWS // needed for Swift
#    define timeval dummy_timeval
#    include <winsock2.h>
#    undef timeval
#    undef IN
#    pragma comment(lib, "ws2_32.lib")
#    include <io.h>
#endif
