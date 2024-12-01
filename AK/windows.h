/*
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef AK_OS_WINDOWS // needed for Swift
#    define timeval dummy_timeval
#    include <winsock2.h>
#    undef timeval
#    include <io.h>
#endif
