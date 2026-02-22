/*
 * Copyright (c) 2026, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CpuBackoff.h>

#ifdef AK_OS_WINDOWS
#    include <AK/Windows.h>
#else
#    include <sched.h>
#endif

namespace AK {

void yield_thread()
{
#ifdef AK_OS_WINDOWS
    (void)SwitchToThread();
#else
    (void)sched_yield();
#endif
}

}
