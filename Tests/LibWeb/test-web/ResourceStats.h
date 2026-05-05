/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>

#if !defined(AK_OS_WINDOWS)
#    include <sys/resource.h>
#endif

namespace TestWeb {

struct ResourceStats {
    size_t open_fds { 0 };
    size_t files { 0 };
    size_t sockets { 0 };
    size_t pipes { 0 };
    size_t shared_memory { 0 };
    size_t other { 0 };
#if !defined(AK_OS_WINDOWS)
    Optional<rlim_t> fd_limit;
#endif
};

ResourceStats resource_stats(bool force_update = false);

}
